import Foundation
import Observation

public enum VideoFilter: String, CaseIterable, Identifiable, Sendable {
    case pixelPerfect, smooth, crt
    public var id: String { rawValue }
    public var title: String {
        switch self {
        case .pixelPerfect: "Pixel Perfect"
        case .smooth: "Suave (Scale4x)"
        case .crt: "CRT"
        }
    }
}

public enum AspectMode: String, CaseIterable, Identifiable, Sendable {
    case television, squarePixels, stretch
    public var id: String { rawValue }
    public var title: String {
        switch self {
        case .television: "TV original (pixel 8:7)"
        case .squarePixels: "Pixels quadrados"
        case .stretch: "Esticar"
        }
    }
    /// Width/height of one SNES pixel on screen. A CRT TV showed the 256x224
    /// picture at 4:3, i.e. pixels 8:7 wide.
    var pixelAspect: Double? {
        switch self {
        case .television: 8.0 / 7.0
        case .squarePixels: 1
        case .stretch: nil
        }
    }
}

/// User preferences. Everything here is presentation or comfort: none of it
/// alters how the game itself runs.
@MainActor
@Observable
public final class GameSettings {
    public var filter: VideoFilter { didSet { save() } }
    public var aspect: AspectMode { didSet { save() } }
    public var crtColors: Bool { didSet { save() } }
    public var crtCurvature: Bool { didSet { save() } }
    public var scanlineStrength: Double { didSet { save() } }
    public var hdAudio: Bool { didSet { save() } }
    public var volume: Double { didSet { save() } }
    /// Frames of run-ahead latency reduction (0 = off).
    public var runAhead: Int { didSet { save() } }
    public var haptics: Bool { didSet { save() } }
    public var showTouchControls: Bool { didSet { save() } }
    public var rewind: Bool { didSet { save() } }
    /// Show more of the level at the sides to fill wide screens.
    public var widescreen: Bool { didSet { save() } }
    public var hdr: Bool { didSet { save() } }
    #if os(tvOS)
    // The game's art is SDR: sent as HDR10 / Dolby Vision, a TV shows it dim
    // and washed out, so the TV now gets SDR unless HDR is turned on. New key,
    // so installs that saved the old default switch too.
    private static let hdrKey = "hdr.tv2"
    private static let hdrDefault = false
    #else
    private static let hdrKey = "hdr"
    private static let hdrDefault = true
    #endif
    /// Brightest HDR highlight, in multiples of SDR reference white.
    public var hdrPeak: Double { didSet { save() } }
    public var wideGamut: Bool { didSet { save() } }
    /// On 120 Hz screens, show PPU-rendered in-between frames (off by
    /// default: SNES pixels can't move by half a pixel, so steps can look
    /// uneven; with it off each real frame is simply shown twice).
    public var motion120: Bool { didSet { save() } }
    /// Parallax backgrounds glide at their real speed instead of stepping by
    /// whole SNES pixels (display only; Pixel Perfect filter).
    public var smoothParallax: Bool { didSet { save() } }
    public var showPerformance: Bool { didSet { save() } }

    private let defaults: UserDefaults
    private var loading = true

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        filter = VideoFilter(rawValue: defaults.string(forKey: "filter") ?? "") ?? .pixelPerfect
        aspect = AspectMode(rawValue: defaults.string(forKey: "aspect") ?? "") ?? .television
        crtColors = defaults.object(forKey: "crtColors") as? Bool ?? false
        crtCurvature = defaults.object(forKey: "crtCurvature") as? Bool ?? true
        scanlineStrength = defaults.object(forKey: "scanlineStrength") as? Double ?? 0.7
        hdAudio = defaults.object(forKey: "hdAudio") as? Bool ?? false
        volume = defaults.object(forKey: "volume") as? Double ?? 0.9
        runAhead = defaults.object(forKey: "runAhead") as? Int ?? 1
        haptics = defaults.object(forKey: "haptics") as? Bool ?? true
        showTouchControls = defaults.object(forKey: "showTouchControls") as? Bool ?? true
        rewind = defaults.object(forKey: "rewind") as? Bool ?? true
        widescreen = defaults.object(forKey: "widescreen") as? Bool ?? true
        hdr = defaults.object(forKey: Self.hdrKey) as? Bool ?? Self.hdrDefault
        hdrPeak = defaults.object(forKey: "hdrPeak") as? Double ?? 2.5
        wideGamut = defaults.object(forKey: "wideGamut") as? Bool ?? false
        motion120 = defaults.object(forKey: "interpolatedFrames") as? Bool ?? false
        smoothParallax = defaults.object(forKey: "smoothParallax") as? Bool ?? true
        showPerformance = defaults.object(forKey: "showPerformance") as? Bool ?? false
        loading = false
    }

    private func save() {
        guard !loading else { return }
        defaults.set(filter.rawValue, forKey: "filter")
        defaults.set(aspect.rawValue, forKey: "aspect")
        defaults.set(crtColors, forKey: "crtColors")
        defaults.set(crtCurvature, forKey: "crtCurvature")
        defaults.set(scanlineStrength, forKey: "scanlineStrength")
        defaults.set(hdAudio, forKey: "hdAudio")
        defaults.set(volume, forKey: "volume")
        defaults.set(runAhead, forKey: "runAhead")
        defaults.set(haptics, forKey: "haptics")
        defaults.set(showTouchControls, forKey: "showTouchControls")
        defaults.set(rewind, forKey: "rewind")
        defaults.set(widescreen, forKey: "widescreen")
        defaults.set(hdr, forKey: Self.hdrKey)
        defaults.set(hdrPeak, forKey: "hdrPeak")
        defaults.set(wideGamut, forKey: "wideGamut")
        defaults.set(motion120, forKey: "interpolatedFrames")
        defaults.set(smoothParallax, forKey: "smoothParallax")
        defaults.set(showPerformance, forKey: "showPerformance")
    }
}
