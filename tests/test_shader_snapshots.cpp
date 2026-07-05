#include <doctest/doctest.h>

#include <kumo/core/file.h>
#include <kumo/shaderc/compiler.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace kumo;

namespace fs = std::filesystem;

namespace {

std::string serializeReflection(const shaderc::Reflection& reflection) {
    std::string out;
    for (const shaderc::ReflectionBinding& binding : reflection.bindings) {
        out += "binding set=" + std::to_string(binding.set) +
               " binding=" + std::to_string(binding.binding) + " type=" + binding.type +
               " name=" + binding.name + "\n";
    }
    out += "push_constant_size=" + std::to_string(reflection.pushConstantSize) + "\n";
    out += "vertex_input_locations=";
    for (std::size_t i = 0; i < reflection.vertexInputLocations.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += std::to_string(reflection.vertexInputLocations[i]);
    }
    out += "\n";
    return out;
}

std::string formatErrors(const std::string& sourceName,
                         const std::vector<shaderc::CompileError>& errors) {
    std::string out;
    for (const shaderc::CompileError& err : errors) {
        out += (err.file.empty() ? sourceName : err.file) + ":" + std::to_string(err.line) + ": " +
               err.message + "\n";
    }
    return out;
}

} // namespace

TEST_CASE("shader reflection snapshots") {
    const bool update = std::getenv("KUMO_UPDATE_SNAPSHOTS") != nullptr;

    std::vector<fs::path> shaders;
    for (const fs::directory_entry& entry : fs::directory_iterator(KUMO_SHADER_DIR)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string ext = entry.path().extension().string();
        if (ext == ".vert" || ext == ".frag") {
            shaders.push_back(entry.path());
        }
    }
    std::sort(shaders.begin(), shaders.end());
    REQUIRE_FALSE(shaders.empty());

    for (const fs::path& shader : shaders) {
        const std::string name = shader.filename().string();
        CAPTURE(name);

        auto source = readTextFile(shader);
        REQUIRE_MESSAGE(source.has_value(), source.error());

        const shaderc::Stage stage =
            shader.extension() == ".vert" ? shaderc::Stage::Vertex : shaderc::Stage::Fragment;
        auto compiled = shaderc::compileGlsl(*source, stage, {.sourceName = name});
        if (!compiled) {
            FAIL_CHECK("compile failed for " << name << ":\n"
                                             << formatErrors(name, compiled.error()));
            continue;
        }

        const std::string actual = serializeReflection(compiled->reflection);
        const fs::path snapshot = fs::path(KUMO_SNAPSHOT_DIR) / (name + ".reflect.txt");

        if (update) {
            std::error_code ec;
            fs::create_directories(KUMO_SNAPSHOT_DIR, ec);
            std::ofstream out(snapshot, std::ios::binary | std::ios::trunc);
            out.write(actual.data(), static_cast<std::streamsize>(actual.size()));
            REQUIRE(out.good());
            continue;
        }

        auto expected = readTextFile(snapshot);
        if (!expected) {
            FAIL_CHECK("missing snapshot for " << name << " at " << snapshot.string()
                                               << "; run the test with KUMO_UPDATE_SNAPSHOTS=1 to "
                                                  "create it");
            continue;
        }
        CHECK(actual == *expected);
    }
}
