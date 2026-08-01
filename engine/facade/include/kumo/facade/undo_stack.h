#pragma once

#include <kumo/asset/procedural_sky.h>
#include <kumo/renderer/forward_renderer.h>
#include <kumo/scene/scene.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace kumo::facade {

// One active environment: `file` empty means the procedural sky in `sky` is
// active; a non-empty `file` names an HDR under <assetDir>/env/ and `sky` is
// then unused (still present/defaulted so the type stays copyable value data).
struct EnvironmentSource {
    std::string file;
    asset::ProceduralSkyDesc sky;
    bool operator==(const EnvironmentSource&) const = default;
};

// One captured pre-change state: the scene value plus the renderer-side
// per-material factors and custom shader sources the scene indices refer to.
struct SceneState {
    scene::Scene world;
    std::vector<renderer::ForwardRenderer::MaterialParams> materials;
    std::vector<std::optional<std::string>> shaderSources;
    // nullopt means "the loaded startup HDR is active, not an environment_set
    // choice"; applying this snapshot only re-bakes when it differs from the
    // current value (EnvironmentSource::operator== is defaulted), so undoing
    // an unrelated edit never re-triggers an IBL bake.
    std::optional<EnvironmentSource> environment;
};

// Bounded undo/redo of SceneStates (ADR 0044): capture/apply are injected so
// the stack itself stays renderer-free and unit-testable.
class UndoStack {
public:
    using Capture = std::function<SceneState()>;
    using Apply = std::function<void(const SceneState&)>;

    UndoStack(Capture capture, Apply apply, std::size_t depth = 100);

    // Captures now; the snapshot only becomes an undo step on commitPending().
    // A stale pending snapshot (one begun but never committed or discarded) is
    // silently discarded first, so a gesture abandoned before commit never
    // leaks into a later one.
    void beginPending(std::string label);
    void commitPending(); // no-op without a pending snapshot; clears redo
    void discardPending();
    bool hasPending() const;

    bool canUndo() const;
    bool canRedo() const;
    const std::string* undoLabel() const;
    const std::string* redoLabel() const;

    // Both discard any pending snapshot first: a gesture abandoned before
    // commit must not leak into a later commit.
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
    std::optional<Entry> pending_;
};

} // namespace kumo::facade
