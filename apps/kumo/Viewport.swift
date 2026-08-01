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

    // Left-drag orbits the camera, matching the GLFW viewer (ADR 0039); no
    // modifier handling needed. Tracked in view-local coordinates.
    private var lastDragLocation: NSPoint = .zero

    // WASD pan (product GUI only; the GLFW viewer has no such input). Physical
    // key codes (ANSI layout) so the bindings track key position, not the
    // current input-language mapping. Cleared on flagsChanged/resignFirstResponder
    // so a key does not read as stuck down after the app loses focus mid-press
    // (Cmd+Tab, Mission Control, ...).
    private static let kVKANSI_A: UInt16 = 0x00
    private static let kVKANSI_S: UInt16 = 0x01
    private static let kVKANSI_D: UInt16 = 0x02
    private static let kVKANSI_W: UInt16 = 0x0D
    private static let wasdKeyCodes: Set<UInt16> = [kVKANSI_A, kVKANSI_S, kVKANSI_D, kVKANSI_W]
    private var pressedKeys: Set<UInt16> = []
    private var lastPanTime: CFTimeInterval?

    override var acceptsFirstResponder: Bool { true }

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
        applyKeyboardPan(engine: engine)
        if !engine.tick() {
            stopAndTerminate()
        }
    }

    // WASD pan (ADR 0039 arbitration via panCameraForward): speed scales with
    // the current orbit distance so the pan feels the same relative to the
    // framing whether zoomed in on a small prop or looking at a whole scene.
    private func applyKeyboardPan(engine: KumoEngine) {
        let now = CACurrentMediaTime()
        // Clamped: a modal panel or system sleep while a key is held stalls the
        // ticks, and an unbounded dt would turn the backlog into one giant pan
        // that teleports the camera out of the scene.
        let dt = min(lastPanTime.map { now - $0 } ?? 0, 0.1)
        lastPanTime = now
        guard !pressedKeys.isEmpty, dt > 0 else { return }

        let speed = Float(max(1.0, Double(engine.orbitDistance()))) * Float(dt) * 1.5
        var forward: Float = 0
        var right: Float = 0
        if pressedKeys.contains(Self.kVKANSI_W) { forward += speed }
        if pressedKeys.contains(Self.kVKANSI_S) { forward -= speed }
        if pressedKeys.contains(Self.kVKANSI_D) { right += speed }
        if pressedKeys.contains(Self.kVKANSI_A) { right -= speed }
        if forward != 0 || right != 0 {
            engine.panCameraForward(forward, right: right)
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

    override func mouseDown(with event: NSEvent) {
        lastDragLocation = convert(event.locationInWindow, from: nil)
    }

    override func mouseDragged(with event: NSEvent) {
        guard let engine else { return }
        let location = convert(event.locationInWindow, from: nil)
        let dx = Float(location.x - lastDragLocation.x)
        // AppKit y grows upward; the runtime's orbit camera expects GLFW's
        // y-down convention, so the delta is negated.
        let dy = Float(-(location.y - lastDragLocation.y))
        lastDragLocation = location
        engine.orbitRotateDX(dx, dy: dy)
    }

    override func scrollWheel(with event: NSEvent) {
        guard let engine else { return }
        // Precise trackpad deltas are ~10-100x a GLFW wheel notch; 0.05 maps
        // them into roughly the same range as a mouse wheel step.
        let scale: Float = event.hasPreciseScrollingDeltas ? 0.05 : 1.0
        engine.orbitZoom(Float(event.scrollingDeltaY) * scale)
    }

    // Only handled while this view is first responder (clicking the viewport
    // makes it so, standard AppKit); a text field elsewhere keeps first
    // responder and these overrides are simply never called.
    override func keyDown(with event: NSEvent) {
        guard Self.wasdKeyCodes.contains(event.keyCode) else {
            super.keyDown(with: event)
            return
        }
        pressedKeys.insert(event.keyCode)
    }

    override func keyUp(with event: NSEvent) {
        guard Self.wasdKeyCodes.contains(event.keyCode) else {
            super.keyUp(with: event)
            return
        }
        pressedKeys.remove(event.keyCode)
    }

    // Modifier changes often coincide with the app losing key focus (Cmd+Tab,
    // Mission Control); clear so a WASD key never reads as stuck down.
    override func flagsChanged(with event: NSEvent) {
        pressedKeys.removeAll()
        super.flagsChanged(with: event)
    }

    override func resignFirstResponder() -> Bool {
        pressedKeys.removeAll()
        return super.resignFirstResponder()
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
