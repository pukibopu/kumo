#pragma once

#include <kumo/agent/confirmation_gate.h>
#include <kumo/agent/mcp_server.h>
#include <kumo/agent/session.h>
#include <kumo/agent/tool_registry.h>
#include <kumo/asset/procedural_sky.h>
#include <kumo/core/main_thread_queue.h>
#include <kumo/facade/frame_dirty.h>
#include <kumo/facade/orbit_camera.h>
#include <kumo/facade/undo_stack.h>
#include <kumo/math/math.h>
#include <kumo/renderer/forward_renderer.h>
#include <kumo/scene/scene.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace kumo::gpu {
class Device;
}

namespace kumo::facade {

// Everything above the window/input layer: scene, renderer, agents and the MCP
// surface, assembled once and shared by every shell (GLFW dev viewer, product
// GUI). The shell owns the device/surface and the frame loop; the runtime owns
// the rest. Declaration order inside is the destruction contract the shells
// previously had to maintain by hand.
class EngineRuntime {
public:
    struct Desc {
        std::filesystem::path modelPath;
        std::filesystem::path envPath;
        std::filesystem::path configPath;  // kumo.config.json
        std::filesystem::path envFilePath; // .env
        std::filesystem::path shaderDir;   // KUMO_SHADER_DIR
        bool offline = false;
        bool confirmDestructive = false;
        bool demoPrimitives = false;
        bool mcp = false;
        std::string appVersion;
    };

    // Thread-safe retry notice the shells poll for their status line; provider
    // retry callbacks land here from the worker thread (ADR 0030).
    class Notice {
    public:
        void set(std::string text);
        void clear();
        std::string get() const;

    private:
        mutable std::mutex mutex_;
        std::string text_;
    };

    // Loads assets, uploads the scene, assembles registries/providers/sessions
    // per the config. Returns null on any fatal failure (already logged).
    static std::unique_ptr<EngineRuntime> create(gpu::Device& device, const Desc& desc);
    ~EngineRuntime();

    EngineRuntime(const EngineRuntime&) = delete;
    EngineRuntime& operator=(const EngineRuntime&) = delete;

    // Inspector-facing entity listing/detail (ADR 0044); ids are the
    // "index:generation" wire form scene_list uses.
    struct EntityInfo {
        std::string id;
        std::string name;
        std::string primitive;
    };
    struct EntityDetail {
        bool found = false;
        std::string id;
        std::string name;
        std::string primitive;
        math::float3 position{0.0f, 0.0f, 0.0f};
        math::float3 eulerDeg{0.0f, 0.0f, 0.0f};
        math::float3 scale{1.0f, 1.0f, 1.0f};
        bool hasMaterial = false;
        renderer::ForwardRenderer::MaterialParams material;
        bool hasCustomShader = false;
    };

    std::vector<EntityInfo> listEntities() const;
    EntityDetail entityDetail(const std::string& id) const;

    // beginEdit opens a pending undo point; the first setEntity*/clearEntityShader
    // call that succeeds after it commits that snapshot into an undo step, so
    // multi-field edits (and repeated calls during one drag gesture) collapse
    // into a single step. A failed call leaves the pending snapshot open for
    // the next attempt.
    void beginEdit(const std::string& label);
    // Validates like the scene tools (positive scale, finite); returns false and
    // leaves the entity untouched on bad input. Commits the pending undo point
    // opened by beginEdit on success; leaves it pending on failure.
    bool setEntityTransform(const std::string& id, math::float3 position, math::float3 eulerDeg,
                            math::float3 scale);
    // Validates finiteness; commits the pending undo point opened by beginEdit
    // on success, leaves it pending on failure.
    bool setEntityMaterial(const std::string& id,
                           const renderer::ForwardRenderer::MaterialParams& params);
    std::optional<std::string> entityShaderSource(const std::string& id) const; // custom only
    bool clearEntityShader(const std::string& id); // opens and commits its own undo point
    std::filesystem::path generatedShaderPath(const std::string& id) const; // empty when none

    // Sun light (index 0) snapshot for the product GUI's light panel; mirrors
    // apps/viewer/ui.cpp's LightSettings::syncFrom formula. `found` is false
    // when index 0 does not exist (empty scene), in which case every other
    // field is meaningless.
    struct LightState {
        bool found = false;
        float azimuthDeg = 0.0f;
        float elevationDeg = 0.0f;
        float intensity = 0.0f;
        math::float3 color{1.0f, 1.0f, 1.0f};
    };
    LightState sunLightState() const;
    // Mirrors LightSettings::apply. Same pending-commit contract as
    // setEntityTransform: beginEdit opens the undo point, this commits it on
    // success. False (no light 0) leaves the pending snapshot open.
    bool setSunLight(float azimuthDeg, float elevationDeg, float intensity, math::float3 color);

    // Bakes `desc` and swaps it in as the active IBL environment. Does not
    // manage undo itself: the agent-tool path gets it for free from the
    // ToolRegistry BeforeInvoke/AfterInvoke hook below, same as every other
    // scene tool. False and unchanged on a bake or renderer failure; logged
    // either way.
    bool applyEnvironment(const asset::ProceduralSkyDesc& desc);

    bool undoAvailable() const;
    bool redoAvailable() const;
    std::string undoLabel() const; // "" when none
    std::string redoLabel() const; // "" when none
    bool undo();
    bool redo();

    // Once per frame, before rendering: drains tool work. Returns false when the
    // runtime wants the app to quit (MCP client hung up).
    bool pump();
    void render(gpu::CommandEncoder& encoder, gpu::Texture* output,
                const renderer::ForwardRenderer::Overlay& overlay = {});
    void resize(gpu::Extent2D size);

    // Orbit camera input (ADR 0039), shared by every shell: dx/dy are in the
    // GLFW convention (cursor y grows downward); a shell whose input is
    // y-up (e.g. AppKit) must flip the sign before calling. Both mark the
    // camera moved this frame so pump()'s arbitration applies it instead of
    // syncing from the scene camera, and both call markDirty().
    void orbitRotate(float dx, float dy);
    void orbitZoom(float delta);
    // Same arbitration contract; forward/right are meters (product GUI's WASD
    // pan).
    void orbitPan(float forward, float right);
    // Read-only mirror of OrbitCamera::distance(), for scaling pan speed by
    // the current framing.
    float orbitDistance() const;

    // Render-on-demand (product shell only; the GLFW viewer ignores this and
    // renders unconditionally). Every state change the runtime can observe
    // calls markDirty(); the shell's tick calls consumeRenderNeeded() to
    // decide whether to acquire a drawable and render this frame at all.
    void markDirty();
    bool consumeRenderNeeded();

    scene::Scene& world();
    renderer::ForwardRenderer& renderer();
    MainThreadQueue& queue();
    agent::AgentSession* sceneSession();
    agent::AgentSession* shaderSession();
    agent::ConfirmationGate* confirmGate();
    bool reloadPipelines();

    // Hot-applies whatever the config file/env say now: re-resolves the agent
    // config and rebuilds the scene/shader sessions and providers from scratch,
    // tearing down the existing ones first. Env seeding is the app's concern
    // (a later pass); this only re-reads what is already on disk/in the
    // environment. Refuses (false, logged) while either session is busy —
    // AgentSession's destructor can abort/join a mid-turn session safely, but
    // silently killing a turn the user is waiting on is worse than asking them
    // to wait. Tool registries, the scene and the renderer are untouched.
    bool reloadAgentSessions();

    Notice& sceneRetryNotice();
    Notice& shaderRetryNotice();

    // Scene persistence (the K/L behavior, shell-agnostic). `path` is the full
    // file path; the shell resolves it (e.g. cwd / "kumo_scene.json").
    bool saveScene(const std::filesystem::path& path) const;
    bool loadScene(const std::filesystem::path& path);

private:
    // Not '= default': undo_ needs capture/apply lambdas bound to `this`.
    EngineRuntime();

    SceneState captureSceneState() const;
    void applySceneState(const SceneState& state);
    // Config resolution + session assembly shared by create() and
    // reloadAgentSessions(); `isReload` pushes an initial transcript Info entry
    // into the freshly built sessions instead of leaving the wiped history
    // unexplained. Assumes sceneSession_/shaderSession_/sceneProvider_/
    // shaderProvider_/confirmGate_ are already torn down.
    void assembleAgentSessions(bool isReload);

    // Members below are declared in the order app.cpp's locals used to be: that
    // order is the destruction contract (reverse of declaration). Sessions/mcp
    // machinery are declared last so they tear down first, while the scene and
    // renderer they reach into via raw pointers/references outlive them.
    gpu::Device* device_ = nullptr;
    std::filesystem::path modelPath_;
    std::filesystem::path envPath_;
    std::filesystem::path generatedShaderDir_;

    // Retained so reloadAgentSessions() can redo the same config-resolution
    // branch create() ran, without needing the full Desc kept alive.
    std::filesystem::path configPath_;
    std::filesystem::path envFilePath_;
    bool offline_ = false;
    bool confirmDestructiveOverride_ = false;

    renderer::ForwardRenderer renderer_;
    scene::Scene world_;
    gpu::Extent2D extent_{};

    // nullopt while the loaded HDR (envPath_) is active; set once an
    // environment_set tool call (or a saved scene with one) swaps in a
    // procedural sky, so undo/save know which to restore.
    std::optional<asset::ProceduralSkyDesc> environmentSky_;

    // Orbit camera arbitration (ADR 0039): orbitMoved_ tracks whether input
    // moved the camera this frame; pump() applies orbit_ onto world_.camera
    // when it did, otherwise syncs orbit_ from whatever the scene camera
    // holds (agent camera_set, scene load, ...).
    OrbitCamera orbit_;
    bool orbitMoved_ = false;

    // Unified undo (ADR 0044), pending-commit model: agent tool calls open a
    // pending point in the ToolRegistry BeforeInvoke hook below and resolve it
    // (commit/discard) in the paired AfterInvoke hook once the result is
    // known; inspector-driven edits open one in beginEdit and commit it from
    // the setEntity*/clearEntityShader call that actually succeeds.
    UndoStack undo_;

    // 2 mirrors ForwardRenderer::kFrameSlots (private, not reachable here):
    // one render per dirty pending frame so both double-buffered slots get
    // the new state before the app goes idle again.
    FrameDirty frameDirty_{2};

    MainThreadQueue mainQueue_;
    agent::ToolRegistry sceneToolRegistry_;
    agent::ToolRegistry shaderToolRegistry_;
    agent::ToolRegistry mcpToolRegistry_;

    Notice sceneRetryNotice_;
    Notice shaderRetryNotice_;
    std::optional<agent::ConfirmationGate> confirmGate_;
    std::unique_ptr<agent::ILLMProvider> sceneProvider_;
    std::unique_ptr<agent::ILLMProvider> shaderProvider_;
    std::optional<agent::AgentSession> sceneSession_;
    std::optional<agent::AgentSession> shaderSession_;

    std::optional<agent::McpServer> mcpServer_;
    std::atomic<bool> mcpEof_{false};
    std::atomic<bool> mcpStop_{false};
    std::atomic<bool> mcpReaderDone_{false};
    std::thread mcpReader_;
};

} // namespace kumo::facade
