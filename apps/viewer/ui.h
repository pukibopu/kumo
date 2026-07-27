#pragma once

#include <kumo/agent/confirmation_gate.h>
#include <kumo/agent/session.h>
#include <kumo/rhi/rhi.h>
#include <kumo/scene/light.h>

#include <array>
#include <mutex>
#include <string>
#include <vector>

struct GLFWwindow;

// Development panels (ADR 0043: interim tooling; the product GUI is a separate
// milestone). Panel labels are Chinese, engine-facing strings stay English.
namespace ui {

// Creates the ImGui context with a CJK-capable font and both backends.
void init(kumo::rhi::Device& device, GLFWwindow* window);
void shutdown();
void beginFrame(kumo::rhi::RenderPassEncoder& pass);
void endFrame(kumo::rhi::CommandEncoder& encoder, kumo::rhi::RenderPassEncoder& pass);

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
// may be null.
void drawLightPanel(LightSettings& settings, kumo::scene::Light* light);
void drawMaterialPanel(float& metallic, float& roughness);

struct ChatPanel {
    std::array<char, 1024> input{};
    std::vector<kumo::agent::AgentSession::TranscriptEntry> entries;
    bool scrollToBottom = false;
};

// Provider retry callbacks land here from the worker thread; the panel shows
// the text while the session waits on the model (ADR 0030).
class RetryNotice {
public:
    void set(std::string text);
    void clear();
    std::string get() const;

private:
    mutable std::mutex mutex_;
    std::string text_;
};

// `session` may be null (no provider configured): the panel renders a hint
// instead of the input. Drains the session transcript every frame. `notice`
// may be null.
void drawChatPanel(ChatPanel& panel, kumo::agent::AgentSession* session, RetryNotice* notice);

// Modal for a destructive tool awaiting approval; `gate` may be null.
void drawConfirmDialog(kumo::agent::ConfirmationGate* gate);

} // namespace ui
