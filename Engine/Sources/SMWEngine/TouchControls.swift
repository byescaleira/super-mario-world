#if os(iOS)
import Observation
import SMWCore
import SwiftUI
import UIKit

/// Geometry shared by the touch-handling UIView and the SwiftUI visuals.
struct TouchLayout {
    struct Button {
        let bit: UInt16
        let label: String
        let center: CGPoint
        let radius: CGFloat
        let color: Color
    }

    var dpadCenter: CGPoint
    var dpadRadius: CGFloat
    var face: [Button]
    var shoulders: [Button]

    /// Landscape only: the game is always full screen.
    static func make(size: CGSize, safe: EdgeInsets) -> TouchLayout {
        let r: CGFloat = 30
        let spacing: CGFloat = 46
        let dpadRadius: CGFloat = 66
        let dpad = CGPoint(x: safe.leading + 40 + dpadRadius, y: size.height - safe.bottom - 34 - dpadRadius)
        let faceCenter = CGPoint(x: size.width - safe.trailing - 40 - spacing - r, y: dpad.y)
        let shoulderY = safe.top + 46
        let snesRed = Color(red: 0.86, green: 0.18, blue: 0.22)
        let snesYellow = Color(red: 0.96, green: 0.76, blue: 0.15)
        let snesBlue = Color(red: 0.20, green: 0.38, blue: 0.86)
        let snesGreen = Color(red: 0.16, green: 0.66, blue: 0.36)
        let face = [
            Button(bit: UInt16(SMW_BTN_A), label: "A", center: CGPoint(x: faceCenter.x + spacing, y: faceCenter.y), radius: r, color: snesRed),
            Button(bit: UInt16(SMW_BTN_B), label: "B", center: CGPoint(x: faceCenter.x, y: faceCenter.y + spacing), radius: r, color: snesYellow),
            Button(bit: UInt16(SMW_BTN_X), label: "X", center: CGPoint(x: faceCenter.x, y: faceCenter.y - spacing), radius: r, color: snesBlue),
            Button(bit: UInt16(SMW_BTN_Y), label: "Y", center: CGPoint(x: faceCenter.x - spacing, y: faceCenter.y), radius: r, color: snesGreen),
        ]
        let shoulders = [
            Button(bit: UInt16(SMW_BTN_L), label: "L", center: CGPoint(x: dpad.x, y: shoulderY), radius: 26, color: .gray),
            Button(bit: UInt16(SMW_BTN_R), label: "R", center: CGPoint(x: faceCenter.x, y: shoulderY), radius: 26, color: .gray),
        ]
        return TouchLayout(dpadCenter: dpad, dpadRadius: dpadRadius, face: face, shoulders: shoulders)
    }

    /// Buttons pressed by a touch at `p`. Touches between two face buttons
    /// press both, so a thumb can hold Y (run) and B (jump) together.
    func buttons(at p: CGPoint) -> UInt16 {
        let dx = p.x - dpadCenter.x, dy = p.y - dpadCenter.y
        let dist = hypot(dx, dy)
        if dist < dpadRadius * 1.35 {
            guard dist > dpadRadius * 0.22 else { return 0 }
            let angle = atan2(-dy, dx) // 0 = right, counter-clockwise
            let sector = Int(((angle + .pi * 2 + .pi / 8).truncatingRemainder(dividingBy: .pi * 2)) / (.pi / 4))
            let dirs: [UInt16] = [
                UInt16(SMW_BTN_RIGHT), UInt16(SMW_BTN_RIGHT | SMW_BTN_UP), UInt16(SMW_BTN_UP), UInt16(SMW_BTN_UP | SMW_BTN_LEFT),
                UInt16(SMW_BTN_LEFT), UInt16(SMW_BTN_LEFT | SMW_BTN_DOWN), UInt16(SMW_BTN_DOWN), UInt16(SMW_BTN_DOWN | SMW_BTN_RIGHT),
            ]
            return dirs[sector % 8]
        }
        var bits: UInt16 = 0
        for b in face where hypot(p.x - b.center.x, p.y - b.center.y) < b.radius * 1.32 { bits |= b.bit }
        for b in shoulders where hypot(p.x - b.center.x, p.y - b.center.y) < b.radius * 1.6 { bits |= b.bit }
        return bits
    }

    func contains(_ p: CGPoint) -> Bool {
        if hypot(p.x - dpadCenter.x, p.y - dpadCenter.y) < dpadRadius * 1.35 { return true }
        return (face + shoulders).contains { hypot(p.x - $0.center.x, p.y - $0.center.y) < $0.radius * 1.7 }
    }
}

@MainActor
@Observable
final class TouchState {
    var pressed: UInt16 = 0
}

/// Multi-touch surface: tracks every finger independently and lets fingers
/// slide between buttons, which SwiftUI gestures can't do.
final class TouchPadView: UIView {
    var layout: TouchLayout?
    var onChange: ((UInt16) -> Void)?
    private var touchBits: [ObjectIdentifier: UInt16] = [:]
    private var lastBits: UInt16 = 0
    private let feedback = UIImpactFeedbackGenerator(style: .light)

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        backgroundColor = .clear
    }

    required init?(coder: NSCoder) { fatalError() }

    override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
        layout?.contains(point) ?? false
    }

    private func update(_ touches: Set<UITouch>, ended: Bool) {
        guard let layout else { return }
        for t in touches {
            if ended { touchBits[ObjectIdentifier(t)] = nil } else {
                touchBits[ObjectIdentifier(t)] = layout.buttons(at: t.location(in: self))
            }
        }
        let bits = touchBits.values.reduce(0, |)
        if bits != lastBits {
            if bits & ~lastBits != 0 { feedback.impactOccurred(intensity: 0.6) }
            lastBits = bits
            onChange?(bits)
        }
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) { update(touches, ended: false) }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) { update(touches, ended: false) }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { update(touches, ended: true) }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { update(touches, ended: true) }
}

struct TouchPadRepresentable: UIViewRepresentable {
    let layout: TouchLayout
    let onChange: (UInt16) -> Void

    func makeUIView(context: Context) -> TouchPadView {
        let v = TouchPadView()
        v.layout = layout
        v.onChange = onChange
        return v
    }

    func updateUIView(_ v: TouchPadView, context: Context) {
        v.layout = layout
        v.onChange = onChange
    }
}

/// On-screen controller: SNES-coloured face buttons, D-pad and shoulders.
struct TouchControls: View {
    let engine: GameEngine
    let layout: TouchLayout
    @State private var state = TouchState()

    var body: some View {
        ZStack {
            dpad(layout)
            ForEach(layout.face + layout.shoulders, id: \.label) { b in
                button(b, pressed: state.pressed & b.bit != 0, pill: b.label == "L" || b.label == "R")
            }
            TouchPadRepresentable(layout: layout) { bits in
                state.pressed = bits
                engine.input.touchPad = bits
            }
        }
    }

    private func dpad(_ layout: TouchLayout) -> some View {
        let r = layout.dpadRadius
        let p = state.pressed
        return ZStack {
            Circle().fill(.clear).frame(width: r * 2, height: r * 2)
                .glassEffect(.regular, in: Circle())
            ForEach(0..<4, id: \.self) { i in
                let bit = [SMW_BTN_UP, SMW_BTN_RIGHT, SMW_BTN_DOWN, SMW_BTN_LEFT][i]
                Image(systemName: "arrowtriangle.up.fill")
                    .font(.system(size: r * 0.32))
                    .foregroundStyle(.white.opacity(p & UInt16(bit) != 0 ? 1 : 0.55))
                    .offset(y: -r * 0.6)
                    .rotationEffect(.degrees(Double(i) * 90))
            }
        }
        .position(layout.dpadCenter)
        .allowsHitTesting(false)
    }

    private func button(_ b: TouchLayout.Button, pressed: Bool, pill: Bool) -> some View {
        Text(b.label)
            .font(.system(size: pill ? 15 : 20, weight: .bold, design: .rounded))
            .foregroundStyle(.white)
            .frame(width: pill ? b.radius * 2.6 : b.radius * 2, height: pill ? b.radius * 1.3 : b.radius * 2)
            .glassEffect(.regular.tint(b.color.opacity(pressed ? 0.85 : 0.45)).interactive(), in: Capsule())
            .scaleEffect(pressed ? 0.92 : 1)
            .animation(.easeOut(duration: 0.08), value: pressed)
            .position(b.center)
            .allowsHitTesting(false)
    }
}
#endif
