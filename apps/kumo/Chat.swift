import AppKit
import SwiftUI
import UniformTypeIdentifiers

// Chat + tool log + destructive-confirmation surface (ADR 0044 slice G4): a
// collapsible pane under the viewport, one tab per agent session. Both
// sessions are polled and drained every tick regardless of which tab is
// visible, or the engine-side transcript_ accumulates unbounded (KumoEngine's
// drainTranscript: doc comment).
struct ChatView: View {
    @ObservedObject var holder: EngineHolder

    private enum ChatTab: Hashable { case scene, shader, director }

    @State private var selectedAgent: ChatTab = .scene
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
                Text("场景").tag(ChatTab.scene)
                Text("Shader").tag(ChatTab.shader)
                Text("导演").tag(ChatTab.director)
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
            case .director:
                DirectorView(holder: holder)
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

    // Reference images (MB-5): copies live in the temp dir until sent.
    @State private var attachments: [URL] = []
    @State private var detailHigh = false
    @State private var showImporter = false
    private static let maxAttachments = 3

    // The director pipeline owns both sessions while it runs (MC): manual
    // chat input would inject into its turn protocol, so it locks here.
    private func inputLocked(_ engine: KumoEngine) -> Bool {
        engine.agentBusy(kind) || engine.directorActive()
    }

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
                if !attachments.isEmpty {
                    HStack(spacing: 6) {
                        ForEach(attachments, id: \.self) { url in
                            HStack(spacing: 2) {
                                Image(systemName: "photo")
                                Text(url.lastPathComponent).lineLimit(1)
                                Button {
                                    attachments.removeAll { $0 == url }
                                } label: {
                                    Image(systemName: "xmark.circle.fill")
                                }
                                .buttonStyle(.plain)
                            }
                            .font(.caption)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(.quaternary, in: Capsule())
                        }
                        Picker("画质", selection: $detailHigh) {
                            Text("低").tag(false)
                            Text("高").tag(true)
                        }
                        .pickerStyle(.segmented)
                        .frame(width: 90)
                        Spacer()
                    }
                    .padding(.horizontal, 8)
                }
                HStack {
                    Button {
                        showImporter = true
                    } label: {
                        Image(systemName: "paperclip")
                    }
                    .help("附加参考图（png/jpeg，最多 \(Self.maxAttachments) 张）")
                    .disabled(inputLocked(engine) ||
                             attachments.count >= Self.maxAttachments)
                    Button {
                        pasteImage()
                    } label: {
                        Image(systemName: "doc.on.clipboard")
                    }
                    .help("粘贴剪贴板里的图片")
                    .disabled(inputLocked(engine) ||
                             attachments.count >= Self.maxAttachments)
                    TextField("输入消息，回车发送", text: $input)
                        .textFieldStyle(.roundedBorder)
                        .disabled(inputLocked(engine))
                        .onSubmit { send(engine: engine) }
                    Button("发送") { send(engine: engine) }
                        .disabled(inputLocked(engine) ||
                                 input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
                .padding([.horizontal, .bottom], 8)
                .fileImporter(isPresented: $showImporter,
                             allowedContentTypes: [.png, .jpeg],
                             allowsMultipleSelection: true) { result in
                    guard case .success(let urls) = result else { return }
                    for url in urls where attachments.count < Self.maxAttachments {
                        importAttachment(url)
                    }
                }
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
        let sent: Bool
        if attachments.isEmpty {
            sent = engine.submit(kind, text: input)
        } else {
            sent = engine.submit(kind, text: input,
                                imagePaths: attachments.map(\.path),
                                imageDetail: detailHigh ? "high" : "low")
        }
        if sent {
            input = ""
            attachments = []
        }
    }

    // Reads under the importer's security scope, then decodes, downscales and
    // re-encodes off the main thread: a full-resolution photo must neither
    // freeze the UI nor reach the provider request unshrunk.
    private func importAttachment(_ url: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        let data = try? Data(contentsOf: url)
        if scoped { url.stopAccessingSecurityScopedResource() }
        guard let data, data.count <= Self.maxSourceBytes else { return }
        storeProcessed(data)
    }

    private func pasteImage() {
        let pasteboard = NSPasteboard.general
        var data = pasteboard.data(forType: .png)
        if data == nil { data = pasteboard.data(forType: .tiff) }
        guard let data, data.count <= Self.maxSourceBytes else { return }
        storeProcessed(data)
    }

    private nonisolated static let maxSourceBytes = 50 * 1024 * 1024
    private nonisolated static let maxLongSidePixels = 1024

    private func storeProcessed(_ data: Data) {
        Task.detached(priority: .userInitiated) {
            guard let png = Self.downscaledPng(data) else { return }
            let target = FileManager.default.temporaryDirectory
                .appendingPathComponent("kumo_ref_\(UUID().uuidString).png")
            guard (try? png.write(to: target)) != nil else { return }
            await MainActor.run {
                if attachments.count < Self.maxAttachments {
                    attachments.append(target)
                }
            }
        }
    }

    private nonisolated static func downscaledPng(_ data: Data) -> Data? {
        guard let rep = NSBitmapImageRep(data: data) else { return nil }
        let width = rep.pixelsWide
        let height = rep.pixelsHigh
        guard width > 0, height > 0 else { return nil }
        if max(width, height) <= maxLongSidePixels {
            return rep.representation(using: .png, properties: [:])
        }
        let scale = Double(maxLongSidePixels) / Double(max(width, height))
        let outWidth = max(1, Int(Double(width) * scale))
        let outHeight = max(1, Int(Double(height) * scale))
        guard let out = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: outWidth,
                                         pixelsHigh: outHeight, bitsPerSample: 8,
                                         samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                         colorSpaceName: .deviceRGB, bytesPerRow: 0,
                                         bitsPerPixel: 0),
              let context = NSGraphicsContext(bitmapImageRep: out) else { return nil }
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = context
        NSImage(data: data)?.draw(in: NSRect(x: 0, y: 0, width: outWidth, height: outHeight))
        NSGraphicsContext.restoreGraphicsState()
        return out.representation(using: .png, properties: [:])
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
