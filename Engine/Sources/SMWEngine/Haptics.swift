import CoreHaptics
import GameController

/// Game-aware rumble. The core reports the sound commands SMW sends to its
/// audio CPU each frame; the IDs (documented in SMWDisX constants.asm) tell us
/// exactly what just happened in the game, so feedback lines up perfectly with
/// the action without touching game code.
@MainActor
final class Haptics {
    private struct Effect {
        var intensity: Float
        var sharpness: Float
        var duration: TimeInterval = 0 // 0 = transient
    }

    private var engines: [ObjectIdentifier: CHHapticEngine] = [:]
    private var deviceEngine: CHHapticEngine?
    private var rumbling: CHHapticAdvancedPatternPlayer?
    var enabled = true

    // port 0
    private static let port0: [UInt8: Effect] = [
        1: Effect(intensity: 0.55, sharpness: 0.9),            // bonk
        2: Effect(intensity: 0.5, sharpness: 0.4),             // splat
        3: Effect(intensity: 0.6, sharpness: 0.7),             // kick
        4: Effect(intensity: 0.4, sharpness: 0.2, duration: 0.25), // pipe
        5: Effect(intensity: 0.35, sharpness: 0.5),            // midway
        8: Effect(intensity: 0.8, sharpness: 0.8),             // spin kill
        10: Effect(intensity: 0.45, sharpness: 0.3, duration: 0.2), // power-up
        11: Effect(intensity: 0.7, sharpness: 0.6),            // switch
        15: Effect(intensity: 0.6, sharpness: 0.6),            // fly hit
        19: Effect(intensity: 0.55, sharpness: 0.6), 20: Effect(intensity: 0.6, sharpness: 0.6),
        21: Effect(intensity: 0.65, sharpness: 0.65), 22: Effect(intensity: 0.7, sharpness: 0.7),
        23: Effect(intensity: 0.75, sharpness: 0.7), 24: Effect(intensity: 0.8, sharpness: 0.75),
        25: Effect(intensity: 0.85, sharpness: 0.8),           // stomp combo 1-7
        31: Effect(intensity: 1.0, sharpness: 0.4, duration: 0.5), // boss dead
        35: Effect(intensity: 0.9, sharpness: 0.2, duration: 0.35), // fall
        39: Effect(intensity: 0.7, sharpness: 0.5), 41: Effect(intensity: 0.7, sharpness: 0.5), // fireworks
    ]
    // port 1
    private static let port1: [UInt8: Effect] = [
        1: Effect(intensity: 0.25, sharpness: 0.5),            // jump
    ]
    // port 3
    private static let port3: [UInt8: Effect] = [
        2: Effect(intensity: 0.45, sharpness: 0.7),            // item block
        4: Effect(intensity: 0.3, sharpness: 0.6),             // spin jump
        5: Effect(intensity: 0.4, sharpness: 0.3, duration: 0.15), // 1-up
        7: Effect(intensity: 0.85, sharpness: 0.9),            // shatter
        8: Effect(intensity: 0.5, sharpness: 0.4),             // spring
        9: Effect(intensity: 1.0, sharpness: 0.6),             // kapow
        15: Effect(intensity: 0.3, sharpness: 0.2, duration: 0.2), // door
    ]
    // port 2 (music)
    private static let deathMusic: UInt8 = 9

    init() {
        #if os(iOS)
        if CHHapticEngine.capabilitiesForHardware().supportsHaptics {
            deviceEngine = try? CHHapticEngine()
            deviceEngine?.isAutoShutdownEnabled = true
            try? deviceEngine?.start()
        }
        #endif
    }

    func controllerConnected(_ controller: GCController) {
        guard let engine = controller.haptics?.createEngine(withLocality: .default) else { return }
        engine.isAutoShutdownEnabled = true
        try? engine.start()
        engines[ObjectIdentifier(controller)] = engine
    }

    func controllerDisconnected(_ controller: GCController) {
        engines[ObjectIdentifier(controller)]?.stop()
        engines[ObjectIdentifier(controller)] = nil
    }

    /// `useDevice` routes feedback to the phone itself (touch controls).
    func handle(events: [UInt16], useDevice: Bool) {
        guard enabled, !events.isEmpty else { return }
        for e in events {
            let port = Int(e >> 8), id = UInt8(e & 0xFF)
            var effect: Effect?
            switch port {
            case 0:
                if id == 33 { startRumble(useDevice: useDevice); continue }   // rumbling on
                if id == 34 { stopRumble(); continue }                        // rumbling off
                effect = Self.port0[id]
            case 1: effect = Self.port1[id]
            case 2: if id == Self.deathMusic { effect = Effect(intensity: 0.9, sharpness: 0.3, duration: 0.6) }
            case 3: effect = Self.port3[id]
            default: break
            }
            if let effect { play(effect, useDevice: useDevice) }
        }
    }

    private var targets: [CHHapticEngine] {
        var list = Array(engines.values)
        if list.isEmpty, let deviceEngine { list.append(deviceEngine) }
        return list
    }

    private func targets(useDevice: Bool) -> [CHHapticEngine] {
        if useDevice, let deviceEngine { return [deviceEngine] }
        return targets
    }

    private func play(_ effect: Effect, useDevice: Bool) {
        let params = [
            CHHapticEventParameter(parameterID: .hapticIntensity, value: effect.intensity),
            CHHapticEventParameter(parameterID: .hapticSharpness, value: effect.sharpness),
        ]
        let event = effect.duration > 0
            ? CHHapticEvent(eventType: .hapticContinuous, parameters: params, relativeTime: 0, duration: effect.duration)
            : CHHapticEvent(eventType: .hapticTransient, parameters: params, relativeTime: 0)
        guard let pattern = try? CHHapticPattern(events: [event], parameters: []) else { return }
        for engine in targets(useDevice: useDevice) {
            try? engine.makePlayer(with: pattern).start(atTime: CHHapticTimeImmediate)
        }
    }

    private func startRumble(useDevice: Bool) {
        let params = [
            CHHapticEventParameter(parameterID: .hapticIntensity, value: 0.5),
            CHHapticEventParameter(parameterID: .hapticSharpness, value: 0.1),
        ]
        let event = CHHapticEvent(eventType: .hapticContinuous, parameters: params, relativeTime: 0, duration: 30)
        guard let pattern = try? CHHapticPattern(events: [event], parameters: []),
              let engine = targets(useDevice: useDevice).first else { return }
        rumbling = try? engine.makeAdvancedPlayer(with: pattern)
        try? rumbling?.start(atTime: CHHapticTimeImmediate)
    }

    private func stopRumble() {
        try? rumbling?.stop(atTime: CHHapticTimeImmediate)
        rumbling = nil
    }
}
