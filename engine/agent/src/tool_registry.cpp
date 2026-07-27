#include <kumo/agent/tool_registry.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <utility>

namespace kumo::agent {

namespace {

std::string errorJson(std::string_view message) {
    return nlohmann::json{{"status", "error"}, {"message", message}}.dump();
}

} // namespace

bool ToolRegistry::add(ToolDef def, ToolHandler handler) {
    if (def.name.empty() || handler == nullptr || find(def.name) != nullptr) {
        return false;
    }
    defs_.push_back(std::move(def));
    handlers_.push_back(std::move(handler));
    return true;
}

std::span<const ToolDef> ToolRegistry::defs() const {
    return defs_;
}

const ToolDef* ToolRegistry::find(std::string_view name) const {
    for (const ToolDef& def : defs_) {
        if (def.name == name) {
            return &def;
        }
    }
    return nullptr;
}

std::string ToolRegistry::invoke(std::string_view name, std::string_view argsJson) const {
    for (std::size_t i = 0; i < defs_.size(); ++i) {
        if (defs_[i].name != name) {
            continue;
        }
        if (!argsJson.empty()) {
            const nlohmann::json args = nlohmann::json::parse(argsJson, nullptr, false);
            if (args.is_discarded() || !args.is_object()) {
                return errorJson("tool arguments must be a JSON object");
            }
        }
        try {
            return handlers_[i](argsJson);
        } catch (const std::exception& e) {
            // Dependency exceptions stop at the module boundary (ADR 0035).
            return errorJson(e.what());
        }
    }
    return errorJson(std::string("unknown tool: ").append(name));
}

} // namespace kumo::agent
