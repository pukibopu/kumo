import AppKit
import Combine
import CoreVideo
import os
import QuartzCore
import SwiftUI

// Shared engine reference (ADR 0044): the viewport lazily creates the engine
// once its Metal layer exists, then publishes it here so the sidebar and
// inspector can observe it too. nil until then.
final class EngineHolder: ObservableObject {
    @Published var engine: KumoEngine?
}

// The engine-facing NSView (ADR 0044): a CAMetalLayer-backed view driven by a
// CVDisplayLink. All engine/render logic lives behind KumoEngine; this class
// only forwards layout and pumps the tick.
final class KumoMetalView: NSView {
    private var engine: KumoEngine?
    private var displayLink: CVDisplayLink?
    // Checked on the display-link thread so at most one tick is ever queued on
    // the main runloop; unguarded dispatching floods it at vsync rate and
    // starves UI event handling.
    private let tickQueued = OSAllocatedUnfairLock(initialState: false)
    var holder: EngineHolder?

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        wantsLayer = true
    }

    override func makeBackingLayer() -> CALayer {
        let layer = CAMetalLayer()
        layer.pixelFormat = .bgra8Unorm
        return layer
    }

    override func layout() {
        super.layout()
        guard let metalLayer = layer as? CAMetalLayer else { return }
        let scale = window?.backingScaleFactor ?? 1.0
        metalLayer.contentsScale = scale
        let pixelWidth = max(UInt32(1), UInt32((bounds.width * scale).rounded()))
        let pixelHeight = max(UInt32(1), UInt32((bounds.height * scale).rounded()))

        if engine == nil {
            engine = KumoEngine(layerUsingDefaults: metalLayer)
            guard let engine else {
                NSLog("kumo: engine failed to start")
                return
            }
            holder?.engine = engine
            startDisplayLink()
        }
        engine?.resizeWidth(pixelWidth, height: pixelHeight)
    }

    private func startDisplayLink() {
        var link: CVDisplayLink?
        CVDisplayLinkCreateWithActiveCGDisplays(&link)
        guard let link else { return }
        CVDisplayLinkSetOutputHandler(link) { [weak self] _, _, _, _, _ in
            guard let self else { return kCVReturnSuccess }
            let shouldDispatch = self.tickQueued.withLock { queued -> Bool in
                if queued { return false }
                queued = true
                return true
            }
            if shouldDispatch {
                DispatchQueue.main.async { self.fireFrame() }
            }
            return kCVReturnSuccess
        }
        CVDisplayLinkStart(link)
        displayLink = link
    }

    // Display link callbacks arrive off the main queue and are re-dispatched
    // here; this guard drops a callback that outpaces the previous tick
    // instead of letting them pile up on the main queue.
    private func fireFrame() {
        defer { tickQueued.withLock { $0 = false } }
        guard let engine else { return }
        if !engine.tick() {
            stopAndTerminate()
        }
    }

    private func stopAndTerminate() {
        if let displayLink {
            CVDisplayLinkStop(displayLink)
        }
        displayLink = nil
        engine = nil
        NSApplication.shared.terminate(nil)
    }

    deinit {
        if let displayLink {
            CVDisplayLinkStop(displayLink)
        }
    }
}

// SwiftUI host for the viewport (ADR 0044): still owns the lazy KumoEngine
// creation, but publishes the result through `holder` so the scene tree and
// inspector can observe it alongside the render loop.
struct Viewport: NSViewRepresentable {
    let holder: EngineHolder

    func makeNSView(context: Context) -> KumoMetalView {
        let view = KumoMetalView(frame: .zero)
        view.holder = holder
        return view
    }

    func updateNSView(_ nsView: KumoMetalView, context: Context) {
        // Nothing to push from SwiftUI state yet; layout drives everything.
    }
}
