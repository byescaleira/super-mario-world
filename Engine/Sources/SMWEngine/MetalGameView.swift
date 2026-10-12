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

    #if os(tvOS)
    // On tvOS a controller's B button (○ on DualSense) also arrives as the
    // UIKit Menu press, which unhandled sends the app to the Home screen. The
    // game view takes focus and swallows Menu and Play/Pause: the game reads
    // every button through GameController (the Siri Remote's Back included),
    // and the TV button still goes Home.
    override var canBecomeFocused: Bool { true }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        guard window != nil else { return }
        DispatchQueue.main.async { [weak self] in
            guard let root = self?.window?.rootViewController else { return }
            root.setNeedsFocusUpdate()
            root.updateFocusIfNeeded()
        }
    }

    private func passThrough(_ presses: Set<UIPress>) -> Set<UIPress> {
        presses.filter { $0.type != .menu && $0.type != .playPause }
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let rest = passThrough(presses)
        if !rest.isEmpty { super.pressesBegan(rest, with: event) }
    }

    override func pressesChanged(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let rest = passThrough(presses)
        if !rest.isEmpty { super.pressesChanged(rest, with: event) }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let rest = passThrough(presses)
        if !rest.isEmpty { super.pressesEnded(rest, with: event) }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let rest = passThrough(presses)
        if !rest.isEmpty { super.pressesCancelled(rest, with: event) }
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
