#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kumo::agent {

// Wire description of one tool. `parametersSchema` is a serialized JSON Schema
// object, `description` is English because both are model-facing (ADR 0028).
// `destructive` gates the confirmation prompt (ADR 0022).
struct ToolDef {
    std::string name;
    std::string description;
    std::string parametersSchema;
    bool destructive = false;
};

// Takes the raw arguments JSON the model produced, returns the result JSON.
using ToolHandler = std::function<std::string(std::string_view argsJson)>;

// The {"status":"error","message":...} envelope every tool-result error uses;
// the single definition of that wire shape (ADR 0028).
std::string errorJson(std::string_view message);

class ToolRegistry {
public:
    // Fires at the top of every invoke(), before the name is even resolved
    // against defs_. The composition root uses this to record an undo point
    // ahead of every tool call, scene_list/shader_read/viewer_screenshot
    // excepted (ADR 0044); null by default.
    using BeforeInvoke = std::function<void(std::string_view name)>;

    // False when the name is empty or already taken; defs() reports registrations
    // in the order they were added.
    bool add(ToolDef def, ToolHandler handler);

    void setBeforeInvoke(BeforeInvoke hook);

    std::span<const ToolDef> defs() const;
    const ToolDef* find(std::string_view name) const;

    // Never fails and never throws. An unknown tool, malformed arguments or a
    // handler-level failure all come back as {"status":"error","message":"..."}:
    // that is protocol payload the model reads and corrects itself with, not an
    // error channel for the caller (ADR 0028).
    // Handlers mutate scene and renderer state, so callers on a worker thread must
    // route this through MainThreadQueue instead of calling it directly (ADR 0005).
    std::string invoke(std::string_view name, std::string_view argsJson) const;

private:
    std::vector<ToolDef> defs_;
    std::vector<ToolHandler> handlers_;
    BeforeInvoke beforeInvoke_;
};

} // namespace kumo::agent
