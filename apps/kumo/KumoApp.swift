import SwiftUI

// Product shell entry point (ADR 0044): NavigationSplitView with a scene-tree
// sidebar, the engine viewport, and an inspector on the right. Chat arrives in
// a later M6.75 slice.
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

    // 2 Hz is enough to reflect MCP/agent-driven changes without being a
    // meaningful CPU cost; edits and undo/redo also refresh immediately.
    private let refreshTimer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some Scene {
        WindowGroup("kumo") {
            NavigationSplitView {
                SceneTreeView(holder: holder, entities: entities, selection: $selectedEntityId)
            } detail: {
                HStack(spacing: 0) {
                    Viewport(holder: holder)
                    Divider()
                    InspectorView(holder: holder, entityId: selectedEntityId,
                                 refreshToken: refreshToken, onChanged: handleEdit)
                        .frame(width: 300)
                }
            }
            .frame(minWidth: 960, minHeight: 540)
            .toolbar {
                ToolbarItemGroup {
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
