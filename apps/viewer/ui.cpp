#include "ui.h"

#include <kumo/math/math.h>

#include <Metal/Metal.hpp>

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_metal.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <utility>

namespace ui {

namespace {

using kumo::agent::AgentSession;

void addCjkFont() {
    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {"/System/Library/Fonts/PingFang.ttc",
                                "/System/Library/Fonts/Hiragino Sans GB.ttc"};
    for (const char* path : candidates) {
        if (std::filesystem::exists(path) &&
            io.Fonts->AddFontFromFileTTF(path, 16.0f, nullptr,
                                         io.Fonts->GetGlyphRangesChineseSimplifiedCommon()) !=
                nullptr) {
            return;
        }
    }
    io.Fonts->AddFontDefault();
}

const char* statusText(AgentSession::Status status) {
    switch (status) {
    case AgentSession::Status::WaitingForModel:
        return "思考中…";
    case AgentSession::Status::RunningTool:
        return "执行工具中…";
    case AgentSession::Status::WaitingForConfirmation:
        return "等待确认…";
    case AgentSession::Status::Idle:
        break;
    }
    return "输入消息，回车发送";
}

// Chat transcript entries and the tool log can exceed ImGui::TextWrapped's
// ~3KB internal format buffer (long tool JSON, summaries); building the
// string ourselves and printing it unformatted avoids the truncation.
void wrappedLine(const std::string& line) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(line.c_str());
    ImGui::PopTextWrapPos();
}

// Window body only (transcript child + status line + input row); the caller
// owns Begin/End so multiple sessions can share one window as tabs. Render-only:
// the caller has already drained the session's transcript into panel.entries.
void drawChatContents(ChatPanel& panel, AgentSession* session,
                      kumo::facade::EngineRuntime::Notice* notice) {
    using Entry = AgentSession::TranscriptEntry;

    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f;
    ImGui::BeginChild("##transcript", {0.0f, -footer});
    for (const Entry& entry : panel.entries) {
        switch (entry.kind) {
        case Entry::Kind::User:
            wrappedLine(std::format("你: {}", entry.text));
            break;
        case Entry::Kind::Assistant:
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 210, 255, 255));
            wrappedLine(std::format("助手: {}", entry.text));
            ImGui::PopStyleColor();
            break;
        case Entry::Kind::ToolCall:
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 150, 150, 255));
            wrappedLine(std::format("→ {} {}", entry.toolName, entry.json));
            ImGui::PopStyleColor();
            break;
        case Entry::Kind::ToolResult:
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 170, 120, 255));
            wrappedLine(std::format("← {} {}", entry.toolName, entry.json));
            ImGui::PopStyleColor();
            break;
        case Entry::Kind::Error:
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 110, 110, 255));
            wrappedLine(std::format("错误: {}", entry.text));
            ImGui::PopStyleColor();
            break;
        case Entry::Kind::Info:
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 150, 150, 255));
            wrappedLine(std::format("· {}", entry.text));
            ImGui::PopStyleColor();
            break;
        }
    }
    if (panel.scrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        panel.scrollToBottom = false;
    }
    ImGui::EndChild();

    if (session == nullptr) {
        ImGui::TextWrapped("未配置模型：在 kumo.config.json 填写 provider（参考 "
                           "kumo.config.example.json），或用 --offline 运行离线演示脚本。");
    } else {
        const AgentSession::Status status = session->status();
        std::string statusLine = statusText(status);
        if (notice != nullptr) {
            if (status == AgentSession::Status::WaitingForModel) {
                if (const std::string retry = notice->get(); !retry.empty()) {
                    statusLine = retry;
                }
            } else {
                notice->clear();
            }
        }
        ImGui::TextUnformatted(statusLine.c_str());
        const bool canSend = !session->busy();
        ImGui::BeginDisabled(!canSend);
        ImGui::SetNextItemWidth(-60.0f);
        bool send = ImGui::InputText("##input", panel.input.data(), panel.input.size(),
                                     ImGuiInputTextFlags_EnterReturnsTrue);
        if (send) {
            // EnterReturnsTrue clears the active id; without this every message
            // would need a mouse re-click on the input box.
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::SameLine();
        send = ImGui::Button("发送") || send;
        ImGui::EndDisabled();
        if (send && canSend && panel.input[0] != '\0') {
            if (session->submit(panel.input.data())) {
                panel.input[0] = '\0';
            }
        }
    }
}

bool hasToolEntries(const ChatPanel& panel) {
    using Entry = AgentSession::TranscriptEntry;
    return std::any_of(panel.entries.begin(), panel.entries.end(), [](const Entry& entry) {
        return entry.kind == Entry::Kind::ToolCall || entry.kind == Entry::Kind::ToolResult;
    });
}

void drawToolLog(const ChatPanel& panel) {
    using Entry = AgentSession::TranscriptEntry;
    for (const Entry& entry : panel.entries) {
        if (entry.kind == Entry::Kind::ToolCall) {
            wrappedLine(std::format("→ {} {}", entry.toolName, entry.json));
        } else if (entry.kind == Entry::Kind::ToolResult) {
            wrappedLine(std::format("← {} {}", entry.toolName, entry.json));
        }
    }
}

// Drains a session's transcript into its panel unconditionally, regardless of
// which tab is active: the tool log (ADR 0022) must stay live for the inactive
// session too, and an undrained transcript_ would otherwise accumulate forever.
void drainInto(ChatPanel& panel, AgentSession* session) {
    using Entry = AgentSession::TranscriptEntry;
    if (session == nullptr) {
        return;
    }
    for (Entry& entry : session->drainTranscript()) {
        panel.entries.push_back(std::move(entry));
        panel.scrollToBottom = true;
    }
}

} // namespace

void init(kumo::rhi::Device& device, GLFWwindow* window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // no imgui.ini in the working directory
    ImGui::StyleColorsDark();
    addCjkFont();
    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplMetal_Init(static_cast<MTL::Device*>(device.nativeHandles().device));
}

void shutdown() {
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void beginFrame(kumo::rhi::RenderPassEncoder& pass) {
    ImGui_ImplMetal_NewFrame(
        static_cast<MTL::RenderPassDescriptor*>(pass.nativePassDescriptorHandle()));
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void endFrame(kumo::rhi::CommandEncoder& encoder, kumo::rhi::RenderPassEncoder& pass) {
    ImGui::Render();
    ImGui_ImplMetal_RenderDrawData(
        ImGui::GetDrawData(), static_cast<MTL::CommandBuffer*>(encoder.nativeCommandBufferHandle()),
        static_cast<MTL::RenderCommandEncoder*>(pass.nativeEncoderHandle()));
}

void drawStatsPanel(int fbWidth, int fbHeight) {
    ImGui::SetNextWindowPos({10.0f, 10.0f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("stats");
    ImGui::Text("%.1f fps (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
    ImGui::Text("drawable %dx%d", fbWidth, fbHeight);
    ImGui::Text("S: save screenshot");
    ImGui::Text("K/L: save/load scene");
    ImGui::End();
}

void LightSettings::apply(kumo::scene::Light& light) const {
    const float az = kumo::math::radians(azimuthDeg);
    const float el = kumo::math::radians(elevationDeg);
    const kumo::math::float3 toSource{std::cos(el) * std::sin(az), std::sin(el),
                                      std::cos(el) * std::cos(az)};
    light.direction = -toSource;
    light.intensity = intensity;
    light.color = {color[0], color[1], color[2]};
}

void LightSettings::syncFrom(const kumo::scene::Light& light) {
    const kumo::math::float3 toSource = -light.direction;
    const float len = length(toSource);
    if (len > 1e-4f) {
        const kumo::math::float3 dir = toSource / len;
        elevationDeg = kumo::math::degrees(std::asin(std::clamp(dir.y, -1.0f, 1.0f)));
        azimuthDeg = kumo::math::degrees(std::atan2(dir.x, dir.z));
    }
    intensity = light.intensity;
    color = {light.color.x, light.color.y, light.color.z};
}

void drawLightPanel(LightSettings& settings, kumo::scene::Light* light) {
    ImGui::SetNextWindowPos({10.0f, 110.0f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("light");
    bool changed = ImGui::SliderFloat("azimuth", &settings.azimuthDeg, -180.0f, 180.0f);
    changed = ImGui::SliderFloat("elevation", &settings.elevationDeg, -85.0f, 85.0f) || changed;
    changed = ImGui::SliderFloat("intensity", &settings.intensity, 0.0f, 10.0f) || changed;
    changed = ImGui::ColorEdit3("color", settings.color.data()) || changed;
    ImGui::End();
    if (changed && light != nullptr) {
        settings.apply(*light);
    }
}

void drawMaterialPanel(float& metallic, float& roughness) {
    ImGui::SetNextWindowPos({10.0f, 270.0f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("material");
    ImGui::SliderFloat("metallic x", &metallic, 0.0f, 1.0f);
    ImGui::SliderFloat("roughness x", &roughness, 0.0f, 1.0f);
    ImGui::End();
}

void drawAgentPanels(ChatPanel& scenePanel, kumo::agent::AgentSession* sceneSession,
                     kumo::facade::EngineRuntime::Notice* sceneNotice, ChatPanel& shaderPanel,
                     kumo::agent::AgentSession* shaderSession,
                     kumo::facade::EngineRuntime::Notice* shaderNotice) {
    drainInto(scenePanel, sceneSession);
    drainInto(shaderPanel, shaderSession);

    ImGui::SetNextWindowPos({960.0f, 10.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({310.0f, 500.0f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("助手");
    if (ImGui::BeginTabBar("##agents")) {
        if (ImGui::BeginTabItem("场景")) {
            drawChatContents(scenePanel, sceneSession, sceneNotice);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Shader")) {
            if (shaderSession == nullptr) {
                ImGui::TextWrapped(
                    "未配置 shader 模型：在 kumo.config.json 的 agents.shader 填 model（云端如 "
                    "OpenAI 需同时填 base_url 与 api_key）。");
            } else {
                drawChatContents(shaderPanel, shaderSession, shaderNotice);
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void drawToolLogPanel(const ChatPanel& scenePanel, const ChatPanel* shaderPanel) {
    ImGui::SetNextWindowPos({960.0f, 520.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({310.0f, 190.0f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("工具日志");
    drawToolLog(scenePanel);
    if (shaderPanel != nullptr && hasToolEntries(*shaderPanel)) {
        ImGui::Separator();
        ImGui::TextUnformatted("— shader —");
        drawToolLog(*shaderPanel);
    }
    ImGui::End();
}

void drawConfirmDialog(kumo::agent::ConfirmationGate* gate) {
    if (gate == nullptr) {
        return;
    }
    const auto prompt = gate->pending();
    if (!prompt.has_value()) {
        return;
    }
    if (!ImGui::IsPopupOpen("确认操作")) {
        ImGui::OpenPopup("确认操作");
    }
    if (ImGui::BeginPopupModal("确认操作", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("助手请求执行破坏性操作：%s", prompt->toolName.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 150, 150, 255));
        ImGui::TextWrapped("%s", prompt->argumentsJson.c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button("允许", {100.0f, 0.0f})) {
            gate->resolve(prompt->id, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("拒绝", {100.0f, 0.0f})) {
            gate->resolve(prompt->id, false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace ui
