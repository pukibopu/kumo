import AppKit
import CoreVideo
import QuartzCore
import SwiftUI

// The engine-facing NSView (ADR 0044): a CAMetalLayer-backed view driven by a
// CVDisplayLink. All engine/render logic lives behind KumoEngine; this class
// only forwards layout and pumps the tick.
final class KumoMetalView: NSView {
    private var engine: KumoEngine?
    private var displayLink: CVDisplayLink?
    private var frameInFlight = false

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
            guard engine != nil else {
                NSLog("kumo: engine failed to start")
                return
            }
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
            DispatchQueue.main.async { self.fireFrame() }
            return kCVReturnSuccess
        }
        CVDisplayLinkStart(link)
        displayLink = link
    }

    // Display link callbacks arrive off the main queue and are re-dispatched
    // here; this guard drops a callback that outpaces the previous tick
    // instead of letting them pile up on the main queue.
    private func fireFrame() {
        guard !frameInFlight, let engine else { return }
        frameInFlight = true
        defer { frameInFlight = false }
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

// SwiftUI host for the viewport (M6.75 slice G2: viewport + resize + clean
// shutdown only — no chat, no inspector; those are later slices).
struct Viewport: NSViewRepresentable {
    func makeNSView(context: Context) -> KumoMetalView {
        KumoMetalView(frame: .zero)
    }

    func updateNSView(_ nsView: KumoMetalView, context: Context) {
        // Nothing to push from SwiftUI state yet; layout drives everything.
    }
}
