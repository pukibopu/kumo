#include <kumo/facade/scene_spec.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <format>

namespace kumo::facade {

namespace {

using nlohmann::json;

// The first balanced top-level {...} in `text`, string-literal aware so a
// brace inside a JSON string cannot unbalance the scan.
std::string_view firstJsonObject(std::string_view text) {
    const std::size_t begin = text.find('{');
    if (begin == std::string_view::npos) {
        return {};
    }
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t i = begin; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) {
                return text.substr(begin, i - begin + 1);
            }
        }
    }
    return {};
}

std::string readString(const json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::vector<std::string> readStringArray(const json& obj, const char* key) {
    std::vector<std::string> out;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) {
        return out;
    }
    for (const auto& item : *it) {
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        }
    }
    return out;
}

std::string readObjectDump(const json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_object() && !it->empty()
               ? it->dump(-1, ' ', false, json::error_handler_t::replace)
               : std::string();
}

void appendList(std::string& out, const char* label, const std::vector<std::string>& items) {
    if (items.empty()) {
        return;
    }
    out += label;
    for (std::size_t i = 0; i < items.size(); ++i) {
        out += i == 0 ? " " : ", ";
        out += items[i];
    }
    out += '\n';
}

void appendJson(std::string& out, const char* label, const std::string& jsonText) {
    if (!jsonText.empty()) {
        out += std::format("{} {}\n", label, jsonText);
    }
}

} // namespace

std::expected<SceneSpec, std::string> parseSceneSpec(std::string_view text) {
    const std::string_view objectText = firstJsonObject(text);
    if (objectText.empty()) {
        return std::unexpected("no JSON object found in the reply");
    }
    const json parsed = json::parse(objectText, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected("the scene plan is not valid JSON");
    }

    SceneSpec spec;
    spec.raw = std::string(objectText);
    spec.styleTier = readString(parsed, "style_tier");
    spec.palette = readStringArray(parsed, "palette");
    spec.cameraJson = readObjectDump(parsed, "camera");
    spec.assets = readStringArray(parsed, "assets");
    spec.lightingJson = readObjectDump(parsed, "lighting");
    spec.banned = readStringArray(parsed, "banned");
    spec.budgetsJson = readObjectDump(parsed, "budgets");
    if (const auto it = parsed.find("elements"); it != parsed.end() && it->is_array()) {
        for (const json& element : *it) {
            if (!element.is_object()) {
                continue;
            }
            SceneSpecElement out;
            out.name = readString(element, "name");
            if (out.name.empty()) {
                continue;
            }
            out.build = readString(element, "build");
            out.materialIntent = readString(element, "material_intent");
            spec.elements.push_back(std::move(out));
        }
    }
    if (spec.elements.empty() && spec.assets.empty()) {
        return std::unexpected("the scene plan lists no elements and no assets");
    }
    return spec;
}

std::string sliceSpec(const SceneSpec& spec, SpecStage stage) {
    std::string out = stage == SpecStage::Build
                          ? "Scene plan from the director. Build exactly this; deviate only "
                            "where a step fails, and say so when you do.\n"
                          : "Material pass for the scene just built. Apply the director's "
                            "material intents below.\n";
    if (!spec.styleTier.empty()) {
        out += std::format("Style tier: {} (never mix tiers)\n", spec.styleTier);
    }
    appendList(out, "Palette:", spec.palette);
    appendList(out, "Banned:", spec.banned);
    if (stage == SpecStage::Build) {
        appendJson(out, "Camera plan:", spec.cameraJson);
        appendJson(out, "Lighting plan:", spec.lightingJson);
        appendJson(out, "Budgets:", spec.budgetsJson);
        appendList(out, "Library assets to use:", spec.assets);
        bool any = false;
        for (const SceneSpecElement& element : spec.elements) {
            if (element.build.empty()) {
                continue;
            }
            if (!any) {
                out += "Elements to build:\n";
                any = true;
            }
            out += std::format("- {}: {}\n", element.name, element.build);
        }
    } else {
        bool any = false;
        for (const SceneSpecElement& element : spec.elements) {
            if (element.materialIntent.empty()) {
                continue;
            }
            if (!any) {
                out += "Element material intents:\n";
                any = true;
            }
            out += std::format("- {}: {}\n", element.name, element.materialIntent);
        }
        if (!any) {
            out += "No per-element material intents; improve materials where the build left "
                   "obvious flat defaults.\n";
        }
    }
    return out;
}

} // namespace kumo::facade
