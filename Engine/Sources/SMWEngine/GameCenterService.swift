import GameKit
import Observation
import SMWCore
#if canImport(UIKit)
import UIKit
#else
import AppKit
#endif

/// Presents system view controllers (Game Center, SharePlay invitations) from
/// whatever window is in front, on every platform.
@MainActor
enum Presenter {
    #if canImport(UIKit)
    static func present(_ vc: UIViewController) {
        let scene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
            .first { $0.activationState == .foregroundActive } ?? UIApplication.shared.connectedScenes.first as? UIWindowScene
        guard var top = scene?.keyWindow?.rootViewController ?? scene?.windows.first?.rootViewController else { return }
        while let next = top.presentedViewController { top = next }
        top.present(vc, animated: true)
    }
    #else
    static func present(_ vc: NSViewController) {
        guard let host = NSApp.keyWindow?.contentViewController ?? NSApp.windows.first?.contentViewController else { return }
        host.presentAsSheet(vc)
    }
    #endif
}

/// Game Center: sign-in, the access point, achievements read straight from
/// the game's own progress variables (SMWDisX rammap.asm) and a score board.
/// IDs must match the ones configured in App Store Connect (see README).
@MainActor
@Observable
public final class GameCenterService {
    public private(set) var isAuthenticated = false
    public private(set) var playerName: String?
    /// Invitations accepted from Messages / Game Center arrive here.
    @ObservationIgnored var onInviteAccepted: ((GKInvite) -> Void)?

    @ObservationIgnored private var reported: Set<String>
    @ObservationIgnored private var bestScoreSubmitted: Int
    @ObservationIgnored private var lastCutscene: UInt8 = 0
    @ObservationIgnored private let listener = InviteListener()

    public init() {
        reported = Set(UserDefaults.standard.stringArray(forKey: "gc.reported") ?? [])
        bestScoreSubmitted = UserDefaults.standard.integer(forKey: "gc.bestScore")
    }

    func authenticate() {
        GKLocalPlayer.local.authenticateHandler = { [weak self] vc, _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                if let vc {
                    Presenter.present(vc)
                    return
                }
                self.isAuthenticated = GKLocalPlayer.local.isAuthenticated
                self.playerName = self.isAuthenticated ? GKLocalPlayer.local.displayName : nil
                if self.isAuthenticated {
                    GKAccessPoint.shared.location = .topTrailing
                    GKAccessPoint.shared.showHighlights = true
                    self.listener.onInvite = { [weak self] invite in self?.onInviteAccepted?(invite) }
                    GKLocalPlayer.local.register(self.listener)
                }
            }
        }
    }

    /// The access point shows only while the game is paused.
    func setAccessPointVisible(_ visible: Bool) {
        guard isAuthenticated else { return }
        GKAccessPoint.shared.isActive = visible
    }

    func showDashboard() {
        guard isAuthenticated else {
            authenticate()
            return
        }
        GKAccessPoint.shared.trigger(state: .achievements) {}
    }

    // MARK: Progress tracking

    /// Called about once a second with the running game's RAM.
    /// `player` is 0 for Mario, 1 for Luigi (the online guest).
    func observe(wram: (Int) -> UInt8, player: Int) {
        guard isAuthenticated else { return }
        let exits = Int(wram(Int(SYM_ExitsCompleted)))
        if exits >= 1 { unlock("smw.exits.first") }
        if exits >= 48 { unlock("smw.exits.half") }
        if exits >= 96 { unlock("smw.exits.all") }

        let colors = ["green", "yellow", "blue", "red"]
        var pressed = 0
        for (i, color) in colors.enumerated() where wram(Int(SYM_SwitchBlockFlags) + i) != 0 {
            unlock("smw.switch.\(color)")
            pressed += 1
        }
        if pressed == 4 { unlock("smw.switch.all") }

        if wram(Int(SYM_DragonCoinsCollected)) >= 5 { unlock("smw.dragoncoins") }

        // Castle-destruction cutscenes 1-7 (one per Koopaling), 8 = Bowser beaten.
        let cutscene = wram(Int(SYM_CutsceneID))
        if cutscene != lastCutscene {
            lastCutscene = cutscene
            let kids = ["iggy", "morton", "lemmy", "ludwig", "roy", "wendy", "larry"]
            if (1...7).contains(cutscene) { unlock("smw.castle.\(kids[Int(cutscene) - 1])") }
            if cutscene == 8 { unlock("smw.bowser") }
        }

        // Score: 3 bytes per player, shown on screen multiplied by 10.
        let base = Int(SYM_PlayerScore) + player * 3
        let score = (Int(wram(base)) | Int(wram(base + 1)) << 8 | Int(wram(base + 2)) << 16) * 10
        if score > bestScoreSubmitted, score - bestScoreSubmitted >= 1000 || score >= 999_990 {
            submit(score: score)
        }
    }

    private func unlock(_ id: String) {
        guard !reported.contains(id) else { return }
        reported.insert(id)
        UserDefaults.standard.set(Array(reported), forKey: "gc.reported")
        let achievement = GKAchievement(identifier: id)
        achievement.percentComplete = 100
        achievement.showsCompletionBanner = true
        GKAchievement.report([achievement]) { [weak self] error in
            guard error != nil else { return }
            DispatchQueue.main.async { self?.reported.remove(id) } // retry later
        }
    }

    private func submit(score: Int) {
        bestScoreSubmitted = score
        UserDefaults.standard.set(score, forKey: "gc.bestScore")
        GKLeaderboard.submitScore(score, context: 0, player: GKLocalPlayer.local,
                                  leaderboardIDs: ["smw.score"]) { _ in }
    }
}

/// Receives accepted Game Center invitations.
private final class InviteListener: NSObject, GKLocalPlayerListener, @unchecked Sendable {
    nonisolated(unsafe) var onInvite: ((GKInvite) -> Void)?

    func player(_ player: GKPlayer, didAccept invite: GKInvite) {
        nonisolated(unsafe) let accepted = invite // GameKit calls listeners on the main thread
        let handler = onInvite
        DispatchQueue.main.async { handler?(accepted) }
    }
}
