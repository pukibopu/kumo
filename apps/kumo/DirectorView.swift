import AppKit
import SwiftUI

// Director pipeline pane (MC): brief + tier in, stage checklist and the
// orchestrator's event stream (notes, critic screenshots, verdicts) out.
// Chat input across both agent tabs is disabled while the pipeline runs;
// this pane owns the cancel affordance.
struct DirectorView: View {
    @ObservedObject var holder: EngineHolder

    @State private var brief = ""
    @State private var hero = false
    @State private var state = "idle"
    @State private var events: [KumoDirectorEvent] = []

    private let pollTimer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    private static let stages: [(key: String, label: String)] = [
        ("directing", "导演规划"), ("building", "场景搭建"), ("materials", "材质"),
        ("critique", "评审"), ("repair_build", "修复·场景"), ("repair_materials", "修复·材质"),
    ]

    private var engine: KumoEngine? { holder.engine }
    private var active: Bool { engine?.directorActive() ?? false }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let engine, !engine.directorConfigured() {
                Text("未配置导演角色（kumo.config.json 的 agents.director），将直接由场景助手搭建。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            HStack(spacing: 8) {
                TextField("一句话描述想要的场景…", text: $brief)
                    .textFieldStyle(.roundedBorder)
                    .disabled(active)
                    .onSubmit { start() }
                Picker("档位", selection: $hero) {
                    Text("标准").tag(false)
                    Text("精修").tag(true)
                }
                .pickerStyle(.segmented)
                .frame(width: 110)
                .labelsHidden()
                .disabled(active)
                if active {
                    Button("取消") { engine?.directorCancel() }
                } else {
                    Button("开拍") { start() }
                        .keyboardShortcut(.defaultAction)
                        .disabled(brief.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
            stageChecklist
            Divider()
            eventStream
        }
        .padding(8)
        .onReceive(pollTimer) { _ in poll() }
    }

    private var stageChecklist: some View {
        HStack(spacing: 10) {
            ForEach(Self.stages, id: \.key) { stage in
                let reached = events.contains { $0.stage == stage.key }
                let current = state == stage.key
                Label(stage.label,
                      systemImage: current ? "circle.dotted" : reached ? "checkmark.circle" : "circle")
                    .font(.caption)
                    .foregroundStyle(current ? .primary : reached ? Color.secondary : Color.secondary.opacity(0.5))
            }
            Spacer()
            Text(stateLabel)
                .font(.caption.bold())
                .foregroundStyle(state == "failed" ? .red : .secondary)
        }
    }

    private var stateLabel: String {
        switch state {
        case "idle": return "待命"
        case "done": return "完成"
        case "failed": return "失败"
        case "cancelled": return "已取消"
        default: return "进行中"
        }
    }

    private var eventStream: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 6) {
                    ForEach(Array(events.enumerated()), id: \.offset) { index, event in
                        eventRow(event).id(index)
                    }
                }
                .padding(.vertical, 4)
            }
            .onChange(of: events.count) { _, count in
                if count > 0 { proxy.scrollTo(count - 1, anchor: .bottom) }
            }
        }
    }

    @ViewBuilder
    private func eventRow(_ event: KumoDirectorEvent) -> some View {
        switch event.kind {
        case .stage:
            Text("— \(stageLabel(event.text)) —")
                .font(.caption.bold())
                .frame(maxWidth: .infinity)
        case .screenshots:
            VStack(alignment: .leading, spacing: 4) {
                Text("评审视图（\(event.text)）").font(.caption).foregroundStyle(.secondary)
                HStack(spacing: 4) {
                    ForEach(event.imagePaths, id: \.self) { path in
                        if let image = NSImage(contentsOfFile: path) {
                            Image(nsImage: image)
                                .resizable()
                                .scaledToFit()
                                .frame(maxHeight: 90)
                                .clipShape(RoundedRectangle(cornerRadius: 4))
                        }
                    }
                }
            }
        case .verdict:
            Text(verdictSummary(event.text))
                .font(.caption)
                .padding(6)
                .background(.quaternary, in: RoundedRectangle(cornerRadius: 6))
        case .error:
            Text(event.text).font(.caption).foregroundStyle(.red)
        default:
            Text(event.text).font(.caption).foregroundStyle(.secondary)
        }
    }

    private func stageLabel(_ key: String) -> String {
        Self.stages.first { $0.key == key }?.label
            ?? (key == "done" ? "完成" : key == "failed" ? "失败" : key == "cancelled" ? "已取消" : key)
    }

    private func verdictSummary(_ raw: String) -> String {
        guard let data = raw.data(using: .utf8),
              let parsed = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return raw }
        let verdict = parsed["verdict"] as? String ?? "?"
        let score = (parsed["score"] as? NSNumber)?.doubleValue
        let issues = (parsed["issues"] as? [[String: Any]]) ?? []
        var out = verdict == "pass" ? "评审通过" : "需要修改"
        if let score { out += String(format: "（%.1f 分）", score) }
        for issue in issues {
            if let note = issue["note"] as? String {
                out += "\n· \(note)"
            }
        }
        if let praise = parsed["praise"] as? String, !praise.isEmpty {
            out += "\n👍 \(praise)"
        }
        return out
    }

    private func start() {
        let trimmed = brief.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty, let engine else { return }
        events.removeAll()
        _ = engine.directorStart(trimmed, hero: hero)
        poll()
    }

    private func poll() {
        guard let engine else { return }
        state = engine.directorState()
        events.append(contentsOf: engine.directorDrainEvents())
    }
}
