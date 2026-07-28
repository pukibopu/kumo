import SwiftUI

// Product shell entry point (ADR 0044): the window hosts the engine viewport
// only. Chat and inspector arrive in later M6.75 slices.
@main
struct KumoApp: App {
    var body: some Scene {
        WindowGroup("kumo") {
            Viewport()
                .frame(minWidth: 960, minHeight: 540)
        }
    }
}
