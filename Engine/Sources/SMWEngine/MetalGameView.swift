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

@MainActor
final class GameViewCoordinator: NSObject, MTKViewDelegate {
    let engine: GameEngine
    init(engine: GameEngine) { self.engine = engine }

    func makeView() -> MTKView {
        let view = GameMTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.colorPixelFormat = .bgra8Unorm // replaced by the engine's SDR/HDR choice
        view.framebufferOnly = true
        view.preferredFramesPerSecond = 60
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        view.isPaused = false
        view.enableSetNeedsDisplay = false
        view.delegate = self
        engine.attach(view: view)
        return view
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
}
#else
struct MetalGameView: UIViewRepresentable {
    let engine: GameEngine
    func makeCoordinator() -> GameViewCoordinator { GameViewCoordinator(engine: engine) }
    func makeUIView(context: Context) -> MTKView { context.coordinator.makeView() }
    func updateUIView(_ uiView: MTKView, context: Context) {}
}
#endif
