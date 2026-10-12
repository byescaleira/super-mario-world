import Foundation
import GameKit
import GroupActivities

// MARK: - SharePlay

/// "Play together" activity: start it in a FaceTime call (or from Messages)
/// and the other person's device joins the same game as Luigi.
struct PlayTogetherActivity: GroupActivity {
    static let activityIdentifier = "com.byescaleira.smw.play-together"

    var metadata: GroupActivityMetadata {
        var m = GroupActivityMetadata()
        m.title = "Super Mario World"
        m.subtitle = "Jogar juntos: Mario e Luigi"
        m.type = .generic
        m.supportsContinuationOnTV = true
        return m
    }
}

private struct WirePacket: Codable, Sendable {
    var bytes: Data
}

@MainActor
final class SharePlayTransport: NetplayTransport {
    let name = "SharePlay"
    private(set) var peerName = "Amigo no FaceTime"
    let isHost: Bool
    var onMessage: ((Data) -> Void)?
    var onDisconnect: (() -> Void)?

    private let session: GroupSession<PlayTogetherActivity>
    private let reliable: GroupSessionMessenger
    private let unreliable: GroupSessionMessenger
    private var tasks: [Task<Void, Never>] = []

    /// The host is the participant with the lowest id: every device computes
    /// the same answer without any negotiation.
    init(session: GroupSession<PlayTogetherActivity>) {
        self.session = session
        reliable = GroupSessionMessenger(session: session, deliveryMode: .reliable)
        unreliable = GroupSessionMessenger(session: session, deliveryMode: .unreliable)
        let ids = session.activeParticipants.map(\.id.uuidString).sorted()
        isHost = ids.first == session.localParticipant.id.uuidString
        for messenger in [reliable, unreliable] {
            tasks.append(Task { [weak self] in
                for await (packet, _) in messenger.messages(of: WirePacket.self) {
                    self?.onMessage?(packet.bytes)
                }
            })
        }
        tasks.append(Task { [weak self] in
            for await participants in session.$activeParticipants.values where participants.count < 2 {
                self?.onDisconnect?()
                return
            }
        })
        tasks.append(Task { [weak self] in
            for await state in session.$state.values {
                if case .invalidated = state { self?.onDisconnect?() }
            }
        })
    }

    func send(_ data: Data, reliable isReliable: Bool) {
        let messenger = isReliable ? reliable : unreliable
        let packet = WirePacket(bytes: data)
        Task { try? await messenger.send(packet) }
    }

    func leave() {
        tasks.forEach { $0.cancel() }
        session.leave()
    }
}

// MARK: - Game Center match

@MainActor
final class GameCenterTransport: NSObject, NetplayTransport, GKMatchDelegate {
    let name = "Game Center"
    let peerName: String
    let isHost: Bool
    var onMessage: ((Data) -> Void)?
    var onDisconnect: (() -> Void)?
    private let match: GKMatch

    init(match: GKMatch) {
        self.match = match
        let other = match.players.first
        peerName = other?.displayName ?? "Jogador do Game Center"
        isHost = GKLocalPlayer.local.gamePlayerID < (other?.gamePlayerID ?? "")
        super.init()
        match.delegate = self
    }

    func send(_ data: Data, reliable: Bool) {
        try? match.sendData(toAllPlayers: data, with: reliable ? .reliable : .unreliable)
    }

    func leave() {
        match.delegate = nil
        match.disconnect()
    }

    nonisolated func match(_ match: GKMatch, didReceive data: Data, fromRemotePlayer player: GKPlayer) {
        Task { @MainActor [weak self] in self?.onMessage?(data) }
    }

    nonisolated func match(_ match: GKMatch, player: GKPlayer, didChange state: GKPlayerConnectionState) {
        guard state == .disconnected else { return }
        Task { @MainActor [weak self] in self?.onDisconnect?() }
    }
}

/// Finds a two-player match through the system Game Center UI.
@MainActor
final class Matchmaker: NSObject, GKMatchmakerViewControllerDelegate {
    private var completion: ((GKMatch?) -> Void)?

    func findMatch(invite: GKInvite? = nil, completion: @escaping (GKMatch?) -> Void) {
        self.completion = completion
        let vc: GKMatchmakerViewController?
        if let invite {
            vc = GKMatchmakerViewController(invite: invite)
        } else {
            let request = GKMatchRequest()
            request.minPlayers = 2
            request.maxPlayers = 2
            request.inviteMessage = "Vamos jogar Super Mario World juntos!"
            vc = GKMatchmakerViewController(matchRequest: request)
        }
        guard let vc else {
            completion(nil)
            return
        }
        vc.matchmakerDelegate = self
        Presenter.present(vc)
    }

    nonisolated func matchmakerViewController(_ viewController: GKMatchmakerViewController, didFind match: GKMatch) {
        nonisolated(unsafe) let found = match // GameKit delivers this on the main thread
        MainActor.assumeIsolated {
            dismiss(viewController)
            completion?(found)
            completion = nil
        }
    }

    nonisolated func matchmakerViewControllerWasCancelled(_ viewController: GKMatchmakerViewController) {
        MainActor.assumeIsolated {
            dismiss(viewController)
            completion?(nil)
            completion = nil
        }
    }

    nonisolated func matchmakerViewController(_ viewController: GKMatchmakerViewController, didFailWithError error: Error) {
        MainActor.assumeIsolated {
            dismiss(viewController)
            completion?(nil)
            completion = nil
        }
    }

    private func dismiss(_ vc: GKMatchmakerViewController) {
        #if canImport(UIKit)
        vc.dismiss(animated: true)
        #else
        vc.dismiss(nil)
        #endif
    }
}
