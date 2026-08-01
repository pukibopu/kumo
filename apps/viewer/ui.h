#pragma once

#include <kumo/facade/engine_runtime.h>
#include <kumo/gpu/gpu.h>
#include <kumo/scene/light.h>

#include <array>
#include <string>
#include <vector>

struct GLFWwindow;

// Development panels (ADR 0043: interim tooling; the product GUI is a separate
// milestone). Panel labels are Chinese, engine-facing strings stay English.
namespace ui {

// Creates the ImGui context with a CJK-capable font and both backends.
void init(kumo::gpu::Device& device, GLFWwindow* window);
void shutdown();
void beginFrame(kumo::gpu::RenderPassEncoder& pass);
void endFrame(kumo::gpu::CommandEncoder& encoder, kumo::gpu::RenderPassEncoder& pass);

void drawStatsPanel(int fbWidth, int fbHeight);

// Slider mirror of the primary light: apply() pushes slider state to the scene,
// syncFrom() pulls agent-made changes back so the sliders track them.
struct LightSettings {
    float azimuthDeg = -45.0f;
    float elevationDeg = 40.0f;
    float intensity = 3.0f;
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};

    void apply(kumo::scene::Light& light) const;
    void syncFrom(const kumo::scene::Light& light);
};

// Applies slider edits to `light` in the same frame they happen, so the caller
// never has to arbitrate between stale panel state and agent changes. `light`
// may be null. `shadowsEnabled` mirrors ForwardRenderer::shadowsEnabled(); the
// caller applies it back to the renderer.
void drawLightPanel(LightSettings& settings, kumo::scene::Light* light, bool& shadowsEnabled);
void drawMaterialPanel(float& metallic, float& roughness);

struct ChatPanel {
    std::array<char, 1024> input{};
    std::vector<kumo::agent::AgentSession::TranscriptEntry> entries;
    bool scrollToBottom = false;
};

// One 助手 window with a 场景/Shader tab pair; either session may be null (the
// tab then shows its configuration hint).
void drawAgentPanels(ChatPanel& scenePanel, kumo::agent::AgentSession* sceneSession,
                     kumo::facade::EngineRuntime::Notice* sceneNotice, ChatPanel& shaderPanel,
                     kumo::agent::AgentSession* shaderSession,
                     kumo::facade::EngineRuntime::Notice* shaderNotice);

// Full, untruncated record of every tool call and result (ADR 0022). `shaderPanel`
// may be null; when non-null and it holds tool entries, its log is appended
// after a separator.
void drawToolLogPanel(const ChatPanel& scenePanel, const ChatPanel* shaderPanel);

// Modal for a destructive tool awaiting approval; `gate` may be null.
void drawConfirmDialog(kumo::agent::ConfirmationGate* gate);

} // namespace ui
