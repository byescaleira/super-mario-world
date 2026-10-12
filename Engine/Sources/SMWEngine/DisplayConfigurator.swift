import AVFoundation
import CoreMedia
import MetalKit
#if os(tvOS)
import AVKit
#endif

/// Chooses and applies the drawable format: SDR, EDR (extended linear P3 on
/// iPhone/iPad/Mac) or HDR10 PQ (Apple TV), and asks the TV for the matching
/// video mode.
@MainActor
enum DisplayConfigurator {
    static func preferredOutput(hdr: Bool) -> DisplayOutput {
        guard hdr else { return .sdr }
        #if os(tvOS)
        return .pq
        #else
        return .edr
        #endif
    }

    static func apply(_ output: DisplayOutput, to view: MTKView) {
        view.colorPixelFormat = output.pixelFormat
        guard let layer = view.layer as? CAMetalLayer else { return }
        switch output {
        case .sdr:
            layer.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
            #if !os(tvOS)
            layer.wantsExtendedDynamicRangeContent = false
            #endif
        case .edr:
            layer.colorspace = CGColorSpace(name: CGColorSpace.extendedLinearDisplayP3)
            #if !os(tvOS)
            layer.wantsExtendedDynamicRangeContent = true
            #endif
        case .pq:
            layer.colorspace = CGColorSpace(name: CGColorSpace.itur_2100_PQ)
        }
        requestTVMode(hdr: output == .pq, view: view)
    }

    /// How far above SDR white this screen can go (1 = SDR only).
    static func headroom(of view: MTKView) -> Float {
        #if os(macOS)
        return Float(view.window?.screen?.maximumPotentialExtendedDynamicRangeColorComponentValue ?? 1)
        #elseif os(iOS)
        return Float(view.window?.windowScene?.screen.potentialEDRHeadroom ?? 1)
        #else
        return 4 // PQ output is absolute; the TV tone-maps above its peak.
        #endif
    }

    static func maximumFramesPerSecond(of view: MTKView) -> Int {
        #if os(macOS)
        return view.window?.screen?.maximumFramesPerSecond ?? 60
        #elseif os(iOS)
        return view.window?.windowScene?.screen.maximumFramesPerSecond ?? 60
        #else
        return 60 // Apple TV video output tops out at 60 Hz
        #endif
    }

    /// On Apple TV, ask for 60 Hz (the SNES rate) and, for HDR, a Dolby Vision
    /// mode. With "Match Content" enabled the TV switches automatically.
    static func requestTVMode(hdr: Bool, view: MTKView) {
        #if os(tvOS)
        guard let window = view.window else { return }
        var extensions: [CFString: Any] = [:]
        var codec = kCMVideoCodecType_HEVC
        if hdr {
            codec = kCMVideoCodecType_DolbyVisionHEVC
            extensions = [
                kCMFormatDescriptionExtension_ColorPrimaries: kCMFormatDescriptionColorPrimaries_ITU_R_2020,
                kCMFormatDescriptionExtension_TransferFunction: kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ,
                kCMFormatDescriptionExtension_YCbCrMatrix: kCMFormatDescriptionYCbCrMatrix_ITU_R_2020,
            ]
        }
        var format: CMVideoFormatDescription?
        CMVideoFormatDescriptionCreate(allocator: nil, codecType: codec, width: 3840, height: 2160,
                                       extensions: extensions as CFDictionary, formatDescriptionOut: &format)
        if let format {
            window.avDisplayManager.preferredDisplayCriteria = AVDisplayCriteria(refreshRate: 60, formatDescription: format)
        }
        #endif
    }
}
