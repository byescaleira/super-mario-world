import GameController
import SMWCore
import SwiftUI
import UniformTypeIdentifiers

/// Entry view shared by the iOS, iPadOS, macOS and tvOS apps.
public struct GameRootView: View {
    @State private var engine: GameEngine
    @Environment(\.scenePhase) private var scenePhase

    public init(engine: GameEngine) { _engine = State(initialValue: engine) }

    public var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            switch engine.phase {
            case .loading:
                ProgressView().tint(.white)
            case .needsROM(let message):
                ROMImportView(engine: engine, message: message)
            case .playing:
                GameScreen(engine: engine)
            }
        }
        .preferredColorScheme(.dark)
        .task { engine.boot() }
        #if os(macOS)
        // Losing focus pauses the game (and its music) like a console game would.
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didResignActiveNotification)) { _ in
            engine.setPaused(true)
        }
        #endif
        .onChange(of: scenePhase) { _, phase in
            switch phase {
            case .background: engine.suspend()
            case .inactive: engine.setPaused(true)
            case .active: engine.resume()
            @unknown default: break
            }
        }
    }
}

struct GameScreen: View {
    let engine: GameEngine

    var body: some View {
        // Measure inside the safe area, then lay out over the full screen.
        GeometryReader { geo in
            let safe = geo.safeAreaInsets
            let full = CGSize(width: geo.size.width + safe.leading + safe.trailing,
                              height: geo.size.height + safe.top + safe.bottom)
            ZStack {
                MetalGameView(engine: engine)
                    #if os(iOS)
                    .handlesGameControllerEvents(matching: .gamepad)
                    #endif
                #if os(iOS)
                if engine.input.touchControlsVisible && !engine.isPaused {
                    let layout = TouchLayout.make(size: full, safe: safe)
                    TouchControls(engine: engine, layout: layout)
                    TouchOverlay(engine: engine, safe: safe)
                }
                #endif
            if engine.isRewinding || engine.isFastForwarding {
                VStack {
                    HStack {
                        Label(engine.isRewinding ? "Rebobinando" : "Avanço rápido",
                              systemImage: engine.isRewinding ? "backward.fill" : "forward.fill")
                            .font(.headline)
                            .padding(.horizontal, 14).padding(.vertical, 8)
                            .background(.ultraThinMaterial, in: Capsule())
                        Spacer()
                    }
                    Spacer()
                }
                .padding(24)
                .allowsHitTesting(false)
            }
            if let perf = engine.performanceText {
                VStack {
                    Text(perf)
                        .font(.caption.monospacedDigit())
                        .padding(.horizontal, 10).padding(.vertical, 5)
                        .background(.black.opacity(0.55), in: Capsule())
                        .padding(.top, 8)
                    Spacer()
                }
                .allowsHitTesting(false)
            }
            if let toast = engine.toast {
                VStack {
                    Spacer()
                    Text(toast)
                        .font(.headline)
                        .padding(.horizontal, 18).padding(.vertical, 10)
                        .background(.ultraThinMaterial, in: Capsule())
                        .padding(.bottom, 40)
                }
                .transition(.opacity)
                .allowsHitTesting(false)
            }
            if engine.isPaused {
                PauseMenuView(engine: engine)
                    .transition(.opacity)
            }
            }
            .frame(width: full.width, height: full.height)
            .ignoresSafeArea()
        }
        .animation(.easeOut(duration: 0.18), value: engine.isPaused)
        .animation(.easeOut(duration: 0.2), value: engine.toast)
        #if os(iOS)
        .persistentSystemOverlays(.hidden)
        .statusBarHidden()
        .defersSystemGestures(on: .all)
        #endif
    }
}

#if os(iOS)
/// Pause, Start/Select and rewind/fast-forward buttons that complement the
/// system GCVirtualController (D-pad, ABXY, L/R).
struct TouchOverlay: View {
    let engine: GameEngine
    let safe: EdgeInsets

    var body: some View {
        VStack {
            HStack {
                Button { engine.setPaused(true) } label: {
                    Image(systemName: "pause.fill").font(.title3).frame(width: 44, height: 44)
                }
                .buttonStyle(.glass)
                Spacer()
                HoldButton(symbol: "backward.fill") { engine.input.touchRewind = $0 }
                HoldButton(symbol: "forward.fill") { engine.input.touchFastForward = $0 }
            }
            .padding(.top, safe.top + 12)
            Spacer()
            HStack(spacing: 18) {
                HoldButton(title: "SELECT") { held in
                    engine.input.touchButtons = held ? engine.input.touchButtons | UInt16(SMW_BTN_SELECT)
                                                     : engine.input.touchButtons & ~UInt16(SMW_BTN_SELECT)
                }
                HoldButton(title: "START") { held in
                    engine.input.touchButtons = held ? engine.input.touchButtons | UInt16(SMW_BTN_START)
                                                     : engine.input.touchButtons & ~UInt16(SMW_BTN_START)
                }
            }
            .padding(.bottom, max(safe.bottom, 12) + 4)
        }
        .padding(.leading, max(safe.leading, 20))
        .padding(.trailing, max(safe.trailing, 20))
    }
}

struct HoldButton: View {
    var symbol: String?
    var title: String?
    let onChange: (Bool) -> Void
    @State private var held = false

    init(symbol: String, onChange: @escaping (Bool) -> Void) {
        self.symbol = symbol
        self.onChange = onChange
    }

    init(title: String, onChange: @escaping (Bool) -> Void) {
        self.title = title
        self.onChange = onChange
    }

    var body: some View {
        Group {
            if let symbol {
                Image(systemName: symbol).font(.title3).frame(width: 44, height: 44)
            } else if let title {
                Text(title).font(.caption.bold()).frame(width: 74, height: 30)
            }
        }
        .glassEffect(held ? .regular.tint(.white.opacity(0.35)) : .regular, in: Capsule())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { _ in if !held { held = true; onChange(true) } }
                .onEnded { _ in held = false; onChange(false) }
        )
    }
}
#endif

struct ROMImportView: View {
    let engine: GameEngine
    let message: String?
    @State private var importing = false

    var body: some View {
        VStack(spacing: 20) {
            Image(systemName: "gamecontroller.fill").font(.system(size: 56))
            Text("Super Mario World").font(.largeTitle.bold())
            Text(message ?? "Gere a ROM a partir do SMWDisX (scripts/build_rom.sh) antes de compilar, ou importe o seu cartucho (.sfc / .smc).")
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 520)
            #if !os(tvOS)
            Button("Importar ROM…") { importing = true }
                .buttonStyle(.glassProminent)
                .controlSize(.large)
            #endif
        }
        .padding(40)
        #if !os(tvOS)
        .fileImporter(isPresented: $importing, allowedContentTypes: [.data]) { result in
            if case .success(let url) = result { engine.importROM(at: url) }
        }
        #endif
    }
}
