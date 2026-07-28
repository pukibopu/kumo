import SwiftUI

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
            .onChange(of: holder.engine == nil) { _, _ in refresh() }
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
}
