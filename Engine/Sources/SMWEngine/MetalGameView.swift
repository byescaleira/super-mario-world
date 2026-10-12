import MetalKit
import SwiftUI

/// MTKView subclass that also swallows key presses so the system doesn't beep
/// while the keyboard is read through GameController.
final class GameMTKView: MTKView {
    #if os(macOS)
    override var acceptsFirstResponder: Bool { true }
    override func keyDown(with event: NSEvent) {}
    override func keyUp(with event: NSEvent) {}
    override func flagsChanged(with event: NSEvent) {}
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        window?.makeFirstResponder(self)
    }
    #endif
}

/// Drives the view from its own display link instead of MTKView's internal
/// timer, so the engine gets vsync-quantized timestamps: the moment the main
/// thread happens to run the callback jitters by milliseconds, and measuring
/// that made the game skip or repeat frames while the screen scrolled.
@MainActor
final class GameViewCoordinator: NSObject, MTKViewDelegate {
    let engine: GameEngine
    private weak var view: MTKView?
    private var link: CADisplayLink?
    private var linkRate: Float = 0
    init(engine: GameEngine) { self.engine = engine }

    func makeView() -> MTKView {
        let view = GameMTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.colorPixelFormat = .bgra8Unorm // replaced by the engine's SDR/HDR choice
        view.framebufferOnly = true
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        view.isPaused = true
        view.enableSetNeedsDisplay = false
        view.delegate = self
        self.view = view
        engine.attach(view: view)
        #if os(macOS)
        let link = view.displayLink(target: self, selector: #selector(step(_:)))
        #else
        let link = CADisplayLink(target: self, selector: #selector(step(_:)))
        #endif
        link.add(to: .main, forMode: .common)
        self.link = link
        return view
    }

    func stop() {
        link?.invalidate()
        link = nil
    }

    @objc private func step(_ link: CADisplayLink) {
        guard let view else {
            stop()
            return
        }
        let rate = engine.preferredFrameRate
        if rate != linkRate {
            linkRate = rate
            link.preferredFrameRateRange = CAFrameRateRange(minimum: rate, maximum: rate, preferred: rate)
        }
        engine.setDisplayTiming(timestamp: link.timestamp, target: link.targetTimestamp)
        view.draw()
    }

    nonisolated func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    nonisolated func draw(in view: MTKView) {
        MainActor.assumeIsolated { engine.tick(view: view) }
    }
}

#if os(macOS)
struct MetalGameView: NSViewRepresentable {
    let engine: GameEngine
    func makeCoordinator() -> GameViewCoordinator { GameViewCoordinator(engine: engine) }
    func makeNSView(context: Context) -> MTKView { context.coordinator.makeView() }
    func updateNSView(_ nsView: MTKView, context: Context) {}
    static func dismantleNSView(_ nsView: MTKView, coordinator: GameViewCoordinator) { coordinator.stop() }
}
#else
struct MetalGameView: UIViewRepresentable {
    let engine: GameEngine
    func makeCoordinator() -> GameViewCoordinator { GameViewCoordinator(engine: engine) }
    func makeUIView(context: Context) -> MTKView { context.coordinator.makeView() }
    func updateUIView(_ uiView: MTKView, context: Context) {}
    static func dismantleUIView(_ uiView: MTKView, coordinator: GameViewCoordinator) { coordinator.stop() }
}
#endif
