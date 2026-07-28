import SwiftUI

// Chat + tool log + destructive-confirmation surface (ADR 0044 slice G4): a
// collapsible pane under the viewport, one tab per agent session. Both
// sessions are polled and drained every tick regardless of which tab is
// visible, or the engine-side transcript_ accumulates unbounded (KumoEngine's
// drainTranscript: doc comment).
struct ChatView: View {
    @ObservedObject var holder: EngineHolder

    @State private var selectedAgent: KumoAgentKind = .scene
    @State private var sceneEntries: [KumoTranscriptEntry] = []
    @State private var shaderEntries: [KumoTranscriptEntry] = []
    @State private var sceneInput = ""
    @State private var shaderInput = ""
    @State private var showToolLog = false
    @State private var confirmPrompt: KumoConfirmPrompt?
    @State private var showConfirmAlert = false

    private let pollTimer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(spacing: 0) {
            Picker("助手", selection: $selectedAgent) {
                Text("场景").tag(KumoAgentKind.scene)
                Text("Shader").tag(KumoAgentKind.shader)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .padding([.horizontal, .top], 8)
            .padding(.bottom, 4)

            switch selectedAgent {
            case .scene:
                AgentChatPane(holder: holder, kind: .scene, entries: sceneEntries,
                             input: $sceneInput)
            case .shader:
                AgentChatPane(holder: holder, kind: .shader, entries: shaderEntries,
                             input: $shaderInput)
            default:
                EmptyView()
            }

            Divider()
            DisclosureGroup("工具日志", isExpanded: $showToolLog) {
                toolLogView
                    .frame(height: 120)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 6)
        }
        .onReceive(pollTimer) { _ in poll() }
        .onChange(of: holder.engine == nil) { _, _ in poll() }
        .alert("助手请求执行破坏性操作", isPresented: $showConfirmAlert,
              presenting: confirmPrompt) { prompt in
            Button("允许") { holder.engine?.resolveConfirm(prompt.promptId, approved: true) }
            Button("拒绝", role: .cancel) {
                holder.engine?.resolveConfirm(prompt.promptId, approved: false)
            }
        } message: { prompt in
            Text("\(prompt.toolName)\n\(prompt.argumentsJson)")
        }
    }

    // Drains both sessions unconditionally (matches apps/viewer/ui.cpp's
    // drainInto: the tool log must stay live for the inactive agent too) and
    // refreshes the confirmation prompt, one at a time (the gate queues the
    // rest).
    private func poll() {
        guard let engine = holder.engine else { return }
        sceneEntries.append(contentsOf: engine.drainTranscript(.scene))
        shaderEntries.append(contentsOf: engine.drainTranscript(.shader))
        let latest = engine.pendingConfirm()
        if confirmPrompt?.promptId != latest?.promptId {
            confirmPrompt = latest
            showConfirmAlert = latest != nil
        }
    }

    private var toolLogView: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 2) {
                ForEach(Array(toolLogLines.enumerated()), id: \.offset) { _, line in
                    Text(line)
                        .font(.system(.caption2, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                        .textSelection(.enabled)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 4)
        }
    }

    // Both agents' tool entries, agent order preserved, simply concatenated
    // (scene then shader) with a tag prefix rather than interleaved by time.
    private var toolLogLines: [String] {
        var lines: [String] = []
        for entry in sceneEntries where entry.kind == .toolCall || entry.kind == .toolResult {
            lines.append(toolLogLine(prefix: "[场景]", entry: entry))
        }
        for entry in shaderEntries where entry.kind == .toolCall || entry.kind == .toolResult {
            lines.append(toolLogLine(prefix: "[Shader]", entry: entry))
        }
        return lines
    }

    private func toolLogLine(prefix: String, entry: KumoTranscriptEntry) -> String {
        let arrow = entry.kind == .toolCall ? "→" : "←"
        return "\(prefix) \(arrow) \(entry.toolName) \(entry.json)"
    }
}

// One agent's transcript + input row. Render-only: ChatView drains the
// session and owns `entries`/`input`.
private struct AgentChatPane: View {
    @ObservedObject var holder: EngineHolder
    let kind: KumoAgentKind
    let entries: [KumoTranscriptEntry]
    @Binding var input: String

    var body: some View {
        if let engine = holder.engine, engine.agentAvailable(kind) {
            VStack(spacing: 4) {
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 3) {
                            ForEach(Array(entries.enumerated()), id: \.offset) { _, entry in
                                entryView(entry)
                            }
                            Color.clear.frame(height: 1).id("bottom")
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 8)
                    }
                    .onChange(of: entries.count) { _, _ in
                        proxy.scrollTo("bottom", anchor: .bottom)
                    }
                }
                Text(engine.agentStatusLine(kind))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 8)
                HStack {
                    TextField("输入消息，回车发送", text: $input)
                        .textFieldStyle(.roundedBorder)
                        .disabled(engine.agentBusy(kind))
                        .onSubmit { send(engine: engine) }
                    Button("发送") { send(engine: engine) }
                        .disabled(engine.agentBusy(kind) ||
                                 input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
                .padding([.horizontal, .bottom], 8)
            }
        } else {
            VStack {
                Spacer()
                Text(holder.engine?.agentHint(kind) ?? "引擎启动中…")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .padding()
                Spacer()
            }
            .frame(maxWidth: .infinity)
        }
    }

    private func send(engine: KumoEngine) {
        let text = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        if engine.submit(kind, text: input) {
            input = ""
        }
    }

    @ViewBuilder
    private func entryView(_ entry: KumoTranscriptEntry) -> some View {
        switch entry.kind {
        case .user:
            plain("你: \(entry.text)")
        case .assistant:
            plain("助手: \(entry.text)").foregroundStyle(.blue)
        case .toolCall:
            mono("→ \(entry.toolName) \(entry.json)").foregroundStyle(.secondary)
        case .toolResult:
            mono("← \(entry.toolName) \(entry.json)").foregroundStyle(.green)
        case .error:
            plain("错误: \(entry.text)").foregroundStyle(.red)
        case .info:
            plain("· \(entry.text)").foregroundStyle(.secondary)
        default:
            EmptyView()
        }
    }

    private func plain(_ text: String) -> some View {
        Text(text)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
            .textSelection(.enabled)
    }

    private func mono(_ text: String) -> some View {
        Text(text)
            .font(.system(.caption, design: .monospaced))
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
            .textSelection(.enabled)
    }
}
