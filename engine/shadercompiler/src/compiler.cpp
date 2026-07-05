#include <kumo/shaderc/compiler.h>

#include "internal.h"

#include <kumo/core/assert.h>
#include <kumo/core/file.h>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <format>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace kumo::shaderc {
namespace {

void ensureGlslangInitialized() {
    static std::once_flag flag;
    std::call_once(flag, [] { glslang::InitializeProcess(); });
}

EShLanguage toEShLanguage(Stage stage) {
    switch (stage) {
    case Stage::Vertex:
        return EShLangVertex;
    case Stage::Fragment:
        return EShLangFragment;
    case Stage::Compute:
        return EShLangCompute;
    }
    KUMO_ASSERT(false);
    return EShLangVertex;
}

class FileIncluder : public glslang::TShader::Includer {
public:
    explicit FileIncluder(const std::vector<std::string>& dirs) : dirs_(dirs) {}

    IncludeResult* includeLocal(const char* headerName, const char* includerName, size_t) override {
        const std::filesystem::path includer(includerName);
        if (includer.has_parent_path()) {
            if (IncludeResult* result = readPath(includer.parent_path() / headerName)) {
                return result;
            }
        }
        return searchDirs(headerName);
    }
    IncludeResult* includeSystem(const char* headerName, const char*, size_t) override {
        return searchDirs(headerName);
    }
    void releaseInclude(IncludeResult* result) override {
        if (result != nullptr) {
            delete static_cast<std::string*>(result->userData);
            delete result;
        }
    }

private:
    IncludeResult* readPath(const std::filesystem::path& path) {
        auto content = readTextFile(path);
        if (!content) {
            return nullptr;
        }
        // glslang owns the IncludeResult until releaseInclude; the backing string
        // must outlive it, so hand ownership to userData and free it there.
        auto* owned = new std::string(std::move(*content));
        return new IncludeResult(path.string(), owned->data(), owned->size(), owned);
    }

    IncludeResult* searchDirs(const char* headerName) {
        for (const std::string& dir : dirs_) {
            const std::filesystem::path path = dir.empty()
                                                   ? std::filesystem::path(headerName)
                                                   : std::filesystem::path(dir) / headerName;
            if (IncludeResult* result = readPath(path)) {
                return result;
            }
        }
        return nullptr;
    }

    const std::vector<std::string>& dirs_;
};

std::string trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(begin, end - begin + 1));
}

bool isAllDigits(std::string_view text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool parseInt(std::string_view text, int& out) {
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

CompileError parseErrorLine(std::string_view line) {
    const std::string_view body = std::string_view(line).substr(sizeof("ERROR:") - 1);
    const std::string rest = trim(body);

    const auto colon1 = rest.find(':');
    if (colon1 != std::string::npos) {
        const auto colon2 = rest.find(':', colon1 + 1);
        if (colon2 != std::string::npos) {
            const std::string loc = trim(std::string_view(rest).substr(0, colon1));
            const std::string lineText =
                trim(std::string_view(rest).substr(colon1 + 1, colon2 - colon1 - 1));
            int lineNo = 0;
            if (parseInt(lineText, lineNo)) {
                CompileError error;
                error.file = isAllDigits(loc) ? std::string{} : loc;
                error.line = lineNo;
                error.message = trim(std::string_view(rest).substr(colon2 + 1));
                return error;
            }
        }
    }
    return CompileError{.file = {}, .line = 0, .message = std::string(line), .secondStage = false};
}

bool isSummaryLine(std::string_view line) {
    // glslang appends a trailing "ERROR: N compilation errors.  No code generated."
    // count line that is not an actual diagnostic.
    return line.find("compilation errors.") != std::string_view::npos ||
           line.find("No code generated") != std::string_view::npos;
}

void parseInfoLog(std::string_view log, std::vector<CompileError>& errors) {
    std::istringstream stream{std::string(log)};
    std::string raw;
    while (std::getline(stream, raw)) {
        const std::string line = trim(raw);
        if (line.rfind("ERROR:", 0) == 0 && !isSummaryLine(line)) {
            errors.push_back(parseErrorLine(line));
        }
    }
}

} // namespace

CompileResult compileGlsl(std::string_view source, Stage stage, const CompileOptions& options) {
    ensureGlslangInitialized();

    const EShLanguage language = toEShLanguage(stage);
    glslang::TShader shader(language);

    const std::string src(source);
    const char* strings[] = {src.c_str()};
    const int lengths[] = {static_cast<int>(src.size())};
    // Leave the main source at string index 0 so glslang reports its errors with
    // an empty file field; included files are named by the includer.
    shader.setStringsWithLengths(strings, lengths, 1);

    shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);

    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    const TBuiltInResource* resources = GetDefaultResources();

    FileIncluder includer(options.includeDirs);
    std::vector<CompileError> errors;

    if (!shader.parse(resources, 460, false, messages, includer)) {
        parseInfoLog(shader.getInfoLog(), errors);
        if (errors.empty()) {
            errors.push_back({.file = {}, .line = 0, .message = shader.getInfoLog()});
        }
        return std::unexpected(std::move(errors));
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        parseInfoLog(program.getInfoLog(), errors);
        if (errors.empty()) {
            errors.push_back({.file = {}, .line = 0, .message = program.getInfoLog()});
        }
        return std::unexpected(std::move(errors));
    }

    glslang::TIntermediate* intermediate = program.getIntermediate(language);
    KUMO_ASSERT(intermediate != nullptr);

    CompiledShader result;
    {
        spv::SpvBuildLogger logger;
        glslang::SpvOptions spvOptions;
        spvOptions.disableOptimizer = true;
        glslang::GlslangToSpv(*intermediate, result.spirv, &logger, &spvOptions);
    }
    if (result.spirv.empty()) {
        errors.push_back(
            {.file = {}, .line = 0, .message = "SPIR-V generation produced no output"});
        return std::unexpected(std::move(errors));
    }

    std::vector<CompileError> mslErrors = detail::translateToMsl(result, stage);
    if (!mslErrors.empty()) {
        errors.insert(errors.end(), std::make_move_iterator(mslErrors.begin()),
                      std::make_move_iterator(mslErrors.end()));
        return std::unexpected(std::move(errors));
    }

    return result;
}

std::string formatError(const CompileError& error, std::string_view sourceName) {
    const std::string_view name = error.file.empty() ? sourceName : std::string_view(error.file);
    return std::format("{}:{}: {}", name, error.line, error.message);
}

} // namespace kumo::shaderc
