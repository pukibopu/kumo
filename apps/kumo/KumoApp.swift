import AppKit
import SwiftUI
import UniformTypeIdentifiers

// Product shell entry point (ADR 0044): NavigationSplitView with a scene-tree
// sidebar, the engine viewport with a collapsible chat pane, and an inspector
// on the right; plus a Settings scene for per-agent endpoints and API keys.
@main
struct KumoApp: App {
    @StateObject private var holder = EngineHolder()

    @State private var entities: [KumoEntityInfo] = []
    @State private var selectedEntityId: String?
    @State private var refreshToken = 0
    @State private var canUndo = false
    @State private var canRedo = false
    @State private var undoLabel = ""
    @State private var redoLabel = ""
    @State private var showChat = true

    // The timer only needs to catch MCP/agent-driven changes made off the
    // toolbar/inspector; edits and undo/redo already refresh immediately via
    // handleEdit(), so this interval just bounds that worst case.
    private let refreshTimer = Timer.publish(every: 1.5, on: .main, in: .common).autoconnect()

    var body: some Scene {
        WindowGroup("kumo") {
            NavigationSplitView {
                SceneTreeView(holder: holder, entities: entities, selection: $selectedEntityId)
            } detail: {
                HStack(spacing: 0) {
                    VSplitView {
                        Viewport(holder: holder)
                        if showChat {
                            ChatView(holder: holder)
                                .frame(minHeight: 160, idealHeight: 220)
                        }
                    }
                    Divider()
                    InspectorView(holder: holder, entityId: selectedEntityId,
                                 refreshToken: refreshToken, onChanged: handleEdit)
                        .frame(width: 300)
                }
            }
            .frame(minWidth: 960, minHeight: 540)
            .toolbar {
                ToolbarItemGroup {
                    Button("聊天") { showChat.toggle() }
                        .keyboardShortcut("j", modifiers: [.command, .shift])
                    Button("撤销", action: performUndo)
                        .disabled(!canUndo)
                        .help(undoLabel)
                        .keyboardShortcut("z", modifiers: [.command])
                    Button("重做", action: performRedo)
                        .disabled(!canRedo)
                        .help(redoLabel)
                        .keyboardShortcut("z", modifiers: [.command, .shift])
                }
            }
            .onReceive(refreshTimer) { _ in refresh() }
            .onChange(of: holder.engine == nil) { _, isNilNow in
                refresh()
                if !isNilNow { restoreSceneOnLaunchIfNeeded() }
            }
        }
        .commands {
            CommandGroup(after: .newItem) {
                Divider()
                Button("保存场景…") { saveScene() }
                    .keyboardShortcut("s", modifiers: [.command])
                    .disabled(holder.engine == nil)
                Button("打开场景…") { openScene() }
                    .keyboardShortcut("o", modifiers: [.command])
                    .disabled(holder.engine == nil)
            }
        }
        Settings {
            SettingsView(holder: holder)
        }
    }

    private func refresh() {
        guard let engine = holder.engine else { return }
        entities = engine.listEntities()
        canUndo = engine.undoAvailable()
        canRedo = engine.redoAvailable()
        undoLabel = engine.undoLabel()
        redoLabel = engine.redoLabel()
    }

    private func performUndo() {
        guard let engine = holder.engine, engine.undo() else { return }
        handleEdit()
    }

    private func performRedo() {
        guard let engine = holder.engine, engine.redo() else { return }
        handleEdit()
    }

    // Bumps refreshToken so the inspector reloads the (possibly reverted)
    // selected entity's detail, in addition to the tree/undo-state refresh.
    private func handleEdit() {
        refreshToken += 1
        refresh()
    }

    // Scene save/load (Cmd+S/Cmd+O): both run the panel modally from a
    // user-initiated menu command, on the main thread.
    private func saveScene() {
        guard let engine = holder.engine else { return }
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "kumo_scene.json"
        panel.allowedContentTypes = [.json]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        if engine.saveScene(toPath: url.path) {
            UserDefaults.standard.set(url.path, forKey: "lastScenePath")
        } else {
            NSLog("kumo: scene save failed: \(url.path)")
        }
    }

    private func openScene() {
        guard let engine = holder.engine else { return }
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        if engine.loadScene(fromPath: url.path) {
            UserDefaults.standard.set(url.path, forKey: "lastScenePath")
            handleEdit()
        } else {
            NSLog("kumo: scene load failed: \(url.path)")
        }
    }

    // Launch restore (Settings' "启动时恢复上次场景" toggle): failure just logs,
    // no dialog, since there is no user-initiated action to attach one to.
    private func restoreSceneOnLaunchIfNeeded() {
        guard let engine = holder.engine else { return }
        guard UserDefaults.standard.bool(forKey: "restoreSceneOnLaunch") else { return }
        guard let path = UserDefaults.standard.string(forKey: "lastScenePath"),
              FileManager.default.fileExists(atPath: path) else { return }
        if engine.loadScene(fromPath: path) {
            handleEdit()
        } else {
            NSLog("kumo: failed to restore scene at launch: \(path)")
        }
    }
}
