#pragma once

#include <kumo/renderer/forward_renderer.h>
#include <kumo/scene/scene.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace kumo::facade {

// One captured pre-change state: the scene value plus the renderer-side
// per-material factors and custom shader sources the scene indices refer to.
struct SceneState {
    scene::Scene world;
    std::vector<renderer::ForwardRenderer::MaterialParams> materials;
    std::vector<std::optional<std::string>> shaderSources;
};

// Bounded undo/redo of SceneStates (ADR 0044): capture/apply are injected so
// the stack itself stays renderer-free and unit-testable.
class UndoStack {
public:
    using Capture = std::function<SceneState()>;
    using Apply = std::function<void(const SceneState&)>;

    UndoStack(Capture capture, Apply apply, std::size_t depth = 100);

    // Records the current state as the undo point for a labeled change.
    void recordBefore(std::string label);

    bool canUndo() const;
    bool canRedo() const;
    const std::string* undoLabel() const;
    const std::string* redoLabel() const;

    bool undo(); // captures current for redo, applies the snapshot
    bool redo();

private:
    struct Entry {
        std::string label;
        SceneState state;
    };

    Capture capture_;
    Apply apply_;
    std::size_t depth_;
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
};

} // namespace kumo::facade
