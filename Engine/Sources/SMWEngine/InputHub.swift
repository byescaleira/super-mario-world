import GameController
import Observation
import SMWCore

public enum MenuInput: Sendable {
    case up, down, left, right, confirm, back, togglePause
}

/// Unifies every input source Apple platforms offer — MFi / Xbox / PlayStation
/// / Switch controllers, the Siri Remote, hardware keyboards and (on iPhone and
/// iPad) on-screen touch controls — into SNES joypad state, polled once per
/// frame for the lowest possible latency.
@MainActor
@Observable
public final class InputHub {
    public private(set) var controllers: [GCController] = []
    public private(set) var hasPhysicalController = false
    public private(set) var hasKeyboard = false

    /// Buttons held through the SwiftUI touch overlay (Start / Select).
    @ObservationIgnored var touchButtons: UInt16 = 0
    /// Buttons held on the on-screen controller (D-pad, ABXY, L/R).
    @ObservationIgnored var touchPad: UInt16 = 0
    /// On-screen controls show while no physical controller is attached.
    public private(set) var touchControlsVisible = false
    @ObservationIgnored var wantsTouchControls = true { didSet { refresh() } }
    @ObservationIgnored var onControllerConnected: ((GCController) -> Void)?
    @ObservationIgnored var onControllerDisconnected: ((GCController) -> Void)?

    @ObservationIgnored private var observers: [NSObjectProtocol] = []
    @ObservationIgnored private var previousMenuBits: UInt32 = 0

    public init() {
        GCController.shouldMonitorBackgroundEvents = false
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.refresh() }
        })
        observers.append(nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.refresh() }
        })
        observers.append(nc.addObserver(forName: .GCKeyboardDidConnect, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.refresh() }
        })
        observers.append(nc.addObserver(forName: .GCKeyboardDidDisconnect, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.refresh() }
        })
        GCController.startWirelessControllerDiscovery {}
        refresh()
    }

    private func isVirtual(_ c: GCController) -> Bool { false }

    private func refresh() {
        let previous = controllers
        controllers = GCController.controllers()
        let physical = controllers.filter { !isVirtual($0) }
        hasPhysicalController = !physical.isEmpty
        hasKeyboard = GCKeyboard.coalesced != nil
        for (i, c) in physical.enumerated() {
            c.playerIndex = i < 4 ? GCControllerPlayerIndex(rawValue: i)! : .indexUnset
        }
        for c in controllers where !previous.contains(where: { $0 === c }) && !isVirtual(c) {
            configure(c)
            onControllerConnected?(c)
        }
        for c in previous where !controllers.contains(where: { $0 === c }) {
            onControllerDisconnected?(c)
        }
        #if DEBUG
        print("[input] controllers:", controllers.map { "\($0.vendorName ?? "?") ext=\($0.extendedGamepad != nil)" },
              "keyboard:", hasKeyboard)
        #endif
        #if os(iOS)
        let forced = ProcessInfo.processInfo.arguments.contains("-SMWForceTouchControls")
        touchControlsVisible = wantsTouchControls && (!hasPhysicalController || forced)
        if !touchControlsVisible { touchPad = 0 }
        #endif
    }

    /// Per-controller touches: Mario-red light bar, PS / Home button routed to
    /// the pause menu, and DualSense adaptive triggers that give the rewind /
    /// fast-forward triggers a soft click of resistance.
    private func configure(_ c: GCController) {
        c.light?.color = GCColor(red: 0.85, green: 0.08, blue: 0.10)
        c.extendedGamepad?.buttonHome?.preferredSystemGestureState = .disabled
        if let ds = c.extendedGamepad as? GCDualSenseGamepad {
            ds.leftTrigger.setModeFeedbackWithStartPosition(0.2, resistiveStrength: 0.35)
            ds.rightTrigger.setModeFeedbackWithStartPosition(0.2, resistiveStrength: 0.35)
        }
    }

    // MARK: Polling

    /// Joypad state for the two SNES ports.
    func poll() -> (UInt16, UInt16) {
        var ports: [UInt16] = [0, 0]
        var physicalIndex = 0
        for c in controllers {
            let bits = Self.read(c)
            if isVirtual(c) {
                ports[0] |= bits
            } else {
                if physicalIndex < 2 { ports[physicalIndex] |= bits }
                physicalIndex += 1
            }
        }
        ports[0] |= readKeyboard() | touchButtons | touchPad
        return (ports[0], ports[1])
    }

    private static func read(_ c: GCController) -> UInt16 {
        var b: UInt16 = 0
        func set(_ on: Bool, _ bit: Int) { if on { b |= UInt16(bit) } }
        if let g = c.extendedGamepad {
            // Positional mapping: matches the SNES pad layout on every brand.
            set(g.buttonA.isPressed, SMW_BTN_B)
            set(g.buttonB.isPressed, SMW_BTN_A)
            set(g.buttonX.isPressed, SMW_BTN_Y)
            set(g.buttonY.isPressed, SMW_BTN_X)
            set(g.leftShoulder.isPressed, SMW_BTN_L)
            set(g.rightShoulder.isPressed, SMW_BTN_R)
            set(g.buttonMenu.isPressed, SMW_BTN_START)
            set(g.buttonOptions?.isPressed ?? false, SMW_BTN_SELECT)
            let sx = g.leftThumbstick.xAxis.value, sy = g.leftThumbstick.yAxis.value
            set(g.dpad.up.isPressed || sy > 0.5, SMW_BTN_UP)
            set(g.dpad.down.isPressed || sy < -0.5, SMW_BTN_DOWN)
            set(g.dpad.left.isPressed || sx < -0.5, SMW_BTN_LEFT)
            set(g.dpad.right.isPressed || sx > 0.5, SMW_BTN_RIGHT)
        } else if let m = c.microGamepad {
            // Siri Remote: swipe surface = D-pad, click = jump, play/pause = run.
            m.reportsAbsoluteDpadValues = false
            set(m.buttonA.isPressed, SMW_BTN_B)
            set(m.buttonX.isPressed, SMW_BTN_Y)
            set(m.dpad.up.value > 0.4, SMW_BTN_UP)
            set(m.dpad.down.value > 0.4, SMW_BTN_DOWN)
            set(m.dpad.left.value > 0.4, SMW_BTN_LEFT)
            set(m.dpad.right.value > 0.4, SMW_BTN_RIGHT)
        }
        return b
    }

    private func readKeyboard() -> UInt16 {
        guard let k = GCKeyboard.coalesced?.keyboardInput else { return 0 }
        var b: UInt16 = 0
        func key(_ code: GCKeyCode, _ bit: Int) {
            if k.button(forKeyCode: code)?.isPressed == true { b |= UInt16(bit) }
        }
        key(.upArrow, SMW_BTN_UP)
        key(.downArrow, SMW_BTN_DOWN)
        key(.leftArrow, SMW_BTN_LEFT)
        key(.rightArrow, SMW_BTN_RIGHT)
        key(.keyZ, SMW_BTN_B)
        key(.keyX, SMW_BTN_A)
        key(.keyA, SMW_BTN_Y)
        key(.keyS, SMW_BTN_X)
        key(.keyQ, SMW_BTN_L)
        key(.keyW, SMW_BTN_R)
        key(.returnOrEnter, SMW_BTN_START)
        key(.rightShift, SMW_BTN_SELECT)
        key(.tab, SMW_BTN_SELECT)
        return b
    }

    /// Touch overlay hold buttons.
    @ObservationIgnored var touchRewind = false
    @ObservationIgnored var touchFastForward = false

    /// Comfort hotkeys: hold left trigger / Backspace to rewind, right trigger /
    /// backtick to fast-forward. Shoulders stay SNES L/R.
    func hotkeys() -> (rewind: Bool, fastForward: Bool) {
        var rewind = touchRewind, ff = touchFastForward
        for c in controllers where !isVirtual(c) {
            if let g = c.extendedGamepad {
                rewind = rewind || g.leftTrigger.value > 0.5
                ff = ff || g.rightTrigger.value > 0.5
            }
        }
        if let k = GCKeyboard.coalesced?.keyboardInput {
            rewind = rewind || k.button(forKeyCode: .deleteOrBackspace)?.isPressed == true
            ff = ff || k.button(forKeyCode: .graveAccentAndTilde)?.isPressed == true
        }
        return (rewind, ff)
    }

    // MARK: Menu navigation (edge-triggered)

    private enum MenuBit: UInt32 {
        case up = 1, down = 2, left = 4, right = 8, confirm = 16, back = 32, pause = 64
    }

    /// Edge-triggered menu events from every source. Call once per frame.
    func menuEvents() -> [MenuInput] {
        var bits: UInt32 = 0
        func set(_ on: Bool, _ bit: MenuBit) { if on { bits |= bit.rawValue } }
        for c in controllers where !isVirtual(c) {
            if let g = c.extendedGamepad {
                set(g.dpad.up.isPressed || g.leftThumbstick.yAxis.value > 0.6, .up)
                set(g.dpad.down.isPressed || g.leftThumbstick.yAxis.value < -0.6, .down)
                set(g.dpad.left.isPressed || g.leftThumbstick.xAxis.value < -0.6, .left)
                set(g.dpad.right.isPressed || g.leftThumbstick.xAxis.value > 0.6, .right)
                set(g.buttonA.isPressed, .confirm)
                set(g.buttonB.isPressed, .back)
                let touchpad = (g as? GCDualSenseGamepad)?.touchpadButton.isPressed
                    ?? (g as? GCDualShockGamepad)?.touchpadButton.isPressed ?? false
                let home = (g.buttonHome?.isPressed ?? false) || touchpad
                set(home || (g.buttonMenu.isPressed && (g.buttonOptions?.isPressed ?? false)), .pause)
            } else if let m = c.microGamepad {
                set(m.dpad.up.value > 0.5, .up)
                set(m.dpad.down.value > 0.5, .down)
                set(m.buttonA.isPressed, .confirm)
            }
        }
        if let k = GCKeyboard.coalesced?.keyboardInput {
            func key(_ code: GCKeyCode, _ bit: MenuBit) { set(k.button(forKeyCode: code)?.isPressed == true, bit) }
            key(.upArrow, .up)
            key(.downArrow, .down)
            key(.leftArrow, .left)
            key(.rightArrow, .right)
            key(.returnOrEnter, .confirm)
            key(.keyZ, .confirm)
            key(.escape, .pause)
        }
        let pressed = bits & ~previousMenuBits
        previousMenuBits = bits
        var out: [MenuInput] = []
        if pressed & MenuBit.pause.rawValue != 0 { out.append(.togglePause) }
        if pressed & MenuBit.up.rawValue != 0 { out.append(.up) }
        if pressed & MenuBit.down.rawValue != 0 { out.append(.down) }
        if pressed & MenuBit.left.rawValue != 0 { out.append(.left) }
        if pressed & MenuBit.right.rawValue != 0 { out.append(.right) }
        if pressed & MenuBit.confirm.rawValue != 0 { out.append(.confirm) }
        if pressed & MenuBit.back.rawValue != 0 { out.append(.back) }
        return out
    }
}
