import SMWEngine
import SwiftUI

@main
struct SuperMarioWorldApp: App {
    @State private var engine = GameEngine()
    #if os(iOS)
    @UIApplicationDelegateAdaptor private var appDelegate: AppDelegate
    #endif

    var body: some Scene {
        WindowGroup {
            GameRootView(engine: engine)
                #if os(macOS)
                .onAppear { FullScreen.enter() }
                #endif
        }
        #if os(macOS)
        .defaultSize(width: 1024, height: 784)
        .windowToolbarStyle(.unifiedCompact)
        .commands { GameCommands(engine: engine) }
        #endif
    }
}

#if os(iOS)
import UIKit

/// Landscape only, on iPad too (Info.plist lists every orientation because
/// iPadOS 26 requires it for windowed multitasking).
final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        .landscape
    }
}
#endif

#if os(macOS)
import AppKit

/// The game always opens in full screen on the Mac (⌃⌘F to leave).
@MainActor
enum FullScreen {
    static func enter() {
        DispatchQueue.main.async {
            guard let window = NSApp.windows.first(where: { $0.isVisible }),
                  !window.styleMask.contains(.fullScreen) else { return }
            window.toggleFullScreen(nil)
        }
    }
}

struct GameCommands: Commands {
    let engine: GameEngine

    var body: some Commands {
        CommandMenu("Jogo") {
            Button(engine.isPaused ? "Continuar" : "Pausar") { engine.togglePause() }
                .keyboardShortcut("p")
            Divider()
            Button("Salvar estado rápido") { engine.quickSave(slot: 1) }
                .keyboardShortcut("s")
            Button("Carregar estado rápido") { engine.quickLoad(slot: 1) }
                .keyboardShortcut("l")
            Divider()
            Button("Reiniciar") { engine.reset() }
                .keyboardShortcut("r", modifiers: [.command, .shift])
        }
    }
}
#endif
