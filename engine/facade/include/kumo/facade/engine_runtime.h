#pragma once

#include <kumo/agent/confirmation_gate.h>
#include <kumo/agent/mcp_server.h>
#include <kumo/agent/session.h>
#include <kumo/agent/tool_registry.h>
#include <kumo/core/main_thread_queue.h>
#include <kumo/renderer/forward_renderer.h>
#include <kumo/scene/scene.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace kumo::rhi {
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
    static std::unique_ptr<EngineRuntime> create(rhi::Device& device, const Desc& desc);
    ~EngineRuntime();

    EngineRuntime(const EngineRuntime&) = delete;
    EngineRuntime& operator=(const EngineRuntime&) = delete;

    // Once per frame, before rendering: drains tool work. Returns false when the
    // runtime wants the app to quit (MCP client hung up).
    bool pump();
    void render(rhi::CommandEncoder& encoder, rhi::Texture* output,
                const renderer::ForwardRenderer::Overlay& overlay = {});
    void resize(rhi::Extent2D size);

    scene::Scene& world();
    renderer::ForwardRenderer& renderer();
    MainThreadQueue& queue();
    agent::AgentSession* sceneSession();
    agent::AgentSession* shaderSession();
    agent::ConfirmationGate* confirmGate();
    bool reloadPipelines();

    Notice& sceneRetryNotice();
    Notice& shaderRetryNotice();

    // Scene persistence (the K/L behavior, shell-agnostic). `path` is the full
    // file path; the shell resolves it (e.g. cwd / "kumo_scene.json").
    bool saveScene(const std::filesystem::path& path) const;
    bool loadScene(const std::filesystem::path& path);

private:
    EngineRuntime() = default;

    // Members below are declared in the order app.cpp's locals used to be: that
    // order is the destruction contract (reverse of declaration). Sessions/mcp
    // machinery are declared last so they tear down first, while the scene and
    // renderer they reach into via raw pointers/references outlive them.
    rhi::Device* device_ = nullptr;
    std::filesystem::path modelPath_;

    renderer::ForwardRenderer renderer_;
    scene::Scene world_;
    rhi::Extent2D extent_{};

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
