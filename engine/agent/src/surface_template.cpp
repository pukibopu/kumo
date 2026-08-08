#include "surface_template.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <format>

namespace kumo::agent::surface {

namespace {

constexpr std::string_view kParamsMarker = "//KUMO_SURFACE_PARAMS";
constexpr std::string_view kFunctionMarker = "//KUMO_SURFACE_FUNCTION";
constexpr std::string_view kRequiredSignature = "void kumoSurface(";

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool isIdentifier(std::string_view text) {
    if (text.empty() || std::isdigit(static_cast<unsigned char>(text.front())) != 0) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), isIdentChar);
}

bool containsWord(std::string_view haystack, std::string_view word) {
    std::size_t pos = 0;
    while ((pos = haystack.find(word, pos)) != std::string_view::npos) {
        const bool startOk = pos == 0 || !isIdentChar(haystack[pos - 1]);
        const std::size_t end = pos + word.size();
        const bool endOk = end >= haystack.size() || !isIdentChar(haystack[end]);
        if (startOk && endOk) {
            return true;
        }
        pos = end;
    }
    return false;
}

// The whole line holding `marker`, including its trailing newline when present.
struct MarkerLine {
    std::size_t begin = 0;
    std::size_t end = 0;
    int lineNumber = 0; // 1-based
};

std::optional<MarkerLine> findMarkerLine(std::string_view text, std::string_view marker) {
    const std::size_t markerPos = text.find(marker);
    if (markerPos == std::string_view::npos) {
        return std::nullopt;
    }
    MarkerLine out;
    const std::size_t lineStart = text.rfind('\n', markerPos);
    out.begin = lineStart == std::string_view::npos ? 0 : lineStart + 1;
    const std::size_t lineEnd = text.find('\n', markerPos);
    out.end = lineEnd == std::string_view::npos ? text.size() : lineEnd + 1;
    out.lineNumber =
        1 + static_cast<int>(std::count(
                text.begin(), text.begin() + static_cast<std::ptrdiff_t>(out.begin), '\n'));
    return out;
}

int lineCount(std::string_view text) {
    return 1 + static_cast<int>(std::count(text.begin(), text.end(), '\n'));
}

} // namespace

std::optional<std::string> findForbiddenConstruct(std::string_view functionSource) {
    static constexpr std::array<std::string_view, 4> kSubstrings = {"#version", "#include",
                                                                    "gl_FragCoord", "frame."};
    for (const std::string_view needle : kSubstrings) {
        if (functionSource.find(needle) != std::string_view::npos) {
            return std::string(needle);
        }
    }
    static constexpr std::array<std::string_view, 5> kWords = {"layout", "uniform", "main", "while",
                                                               "do"};
    for (const std::string_view word : kWords) {
        if (containsWord(functionSource, word)) {
            return std::string(word);
        }
    }
    return std::nullopt;
}

std::expected<std::vector<ParamSlot>, std::string>
computeLayout(std::span<const ParamDecl> params) {
    if (params.size() > kMaxParams) {
        return std::unexpected(std::format("at most {} params are supported", kMaxParams));
    }
    static constexpr std::array<std::string_view, 4> kReserved = {"baseColor", "metallicRoughness",
                                                                  "emissive", "uvTiling"};
    std::vector<ParamSlot> layout;
    std::uint32_t cursor = kPrefixSize;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const ParamDecl& param = params[i];
        if (!isIdentifier(param.name)) {
            return std::unexpected(
                std::format("param name '{}' is not a valid identifier", param.name));
        }
        if (std::find(kReserved.begin(), kReserved.end(), param.name) != kReserved.end()) {
            return std::unexpected(
                std::format("param name '{}' collides with a fixed member", param.name));
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (params[j].name == param.name) {
                return std::unexpected(std::format("duplicate param name '{}'", param.name));
            }
        }
        if (param.isVec4) {
            cursor = (cursor + 15u) & ~15u;
        }
        layout.push_back({.name = param.name, .isVec4 = param.isVec4, .offset = cursor});
        cursor += param.isVec4 ? 16u : 4u;
        if (cursor > kMaxBlockSize) {
            return std::unexpected(
                std::format("params exceed the {} byte factor block budget", kMaxBlockSize));
        }
    }
    return layout;
}

std::expected<SplicedSurface, std::string> spliceSurface(std::string_view templateText,
                                                         std::string_view functionSource,
                                                         std::span<const ParamDecl> params) {
    if (functionSource.find(kRequiredSignature) == std::string_view::npos) {
        return std::unexpected(std::format(
            "the function source must define '{}inout SurfaceOutputs s, in SurfaceInputs i)'",
            kRequiredSignature));
    }
    if (const std::optional<std::string> forbidden = findForbiddenConstruct(functionSource)) {
        return std::unexpected(
            std::format("'{}' is not allowed in a surface function; use shader_write_full for "
                        "full-shader control",
                        *forbidden));
    }
    auto layout = computeLayout(params);
    if (!layout.has_value()) {
        return std::unexpected(layout.error());
    }

    SplicedSurface out;
    out.layout = std::move(*layout);
    std::string paramLines;
    for (const ParamSlot& slot : out.layout) {
        paramLines += std::format("    {} {};\n", slot.isVec4 ? "vec4" : "float", slot.name);
        out.dataSize = std::max(out.dataSize, slot.offset + (slot.isVec4 ? 16u : 4u) - kPrefixSize);
    }

    const std::optional<MarkerLine> paramsMarker = findMarkerLine(templateText, kParamsMarker);
    const std::optional<MarkerLine> functionMarker = findMarkerLine(templateText, kFunctionMarker);
    if (!paramsMarker.has_value() || !functionMarker.has_value() ||
        functionMarker->begin <= paramsMarker->end) {
        return std::unexpected("surface template is missing its splice markers");
    }

    std::string functionBlock(functionSource);
    if (functionBlock.empty() || functionBlock.back() != '\n') {
        functionBlock += '\n';
    }

    out.source.reserve(templateText.size() + paramLines.size() + functionBlock.size());
    out.source.append(templateText.substr(0, paramsMarker->begin));
    out.source.append(paramLines);
    out.source.append(
        templateText.substr(paramsMarker->end, functionMarker->begin - paramsMarker->end));
    out.source.append(functionBlock);
    out.source.append(templateText.substr(functionMarker->end));

    // Marker lines are replaced 1:1 by the generated text, so the function's
    // first line = marker line shifted by the params insertion delta.
    const int paramLineDelta = static_cast<int>(params.size()) - 1;
    out.functionFirstLine = functionMarker->lineNumber + paramLineDelta;
    out.functionLineCount = lineCount(functionBlock) - 1;
    return out;
}

std::vector<std::byte> packDefaults(std::span<const ParamDecl> params,
                                    std::span<const ParamSlot> layout) {
    std::uint32_t dataSize = 0;
    for (const ParamSlot& slot : layout) {
        dataSize = std::max(dataSize, slot.offset + (slot.isVec4 ? 16u : 4u) - kPrefixSize);
    }
    std::vector<std::byte> data(dataSize);
    for (std::size_t i = 0; i < layout.size() && i < params.size(); ++i) {
        const std::size_t bytes = layout[i].isVec4 ? 16 : 4;
        std::memcpy(data.data() + (layout[i].offset - kPrefixSize), params[i].value, bytes);
    }
    return data;
}

std::optional<int> functionLine(const SplicedSurface& spliced, int compiledLine) {
    if (compiledLine < spliced.functionFirstLine ||
        compiledLine >= spliced.functionFirstLine + spliced.functionLineCount) {
        return std::nullopt;
    }
    return compiledLine - spliced.functionFirstLine + 1;
}

} // namespace kumo::agent::surface
