import CoreGraphics
import SMWCore
import Foundation
import ImageIO
import UniformTypeIdentifiers

public struct SaveSlot: Identifiable, Sendable {
    public let id: Int
    public let date: Date?
    public let thumbnail: CGImage?
    public var isEmpty: Bool { date == nil }
}

/// Battery saves (SRAM) and save-state slots under Application Support.
@MainActor
final class SaveStore {
    static let slotCount = 6
    private let root: URL

    init() {
        root = URL.applicationSupportDirectory.appending(path: "Saves", directoryHint: .isDirectory)
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    private func stateURL(_ slot: Int) -> URL { root.appending(path: "slot-\(slot).state") }
    private func thumbURL(_ slot: Int) -> URL { root.appending(path: "slot-\(slot).png") }
    private var resumeURL: URL { root.appending(path: "resume.state") }


    // Save states are compressed and versioned (StateCodec); a state from an
    // incompatible build is simply ignored. The cartridge save lives in
    // SaveVault and is never part of what a state restores.
    func saveState(_ data: Data, slot: Int, thumbnail: CGImage?) {
        try? StateCodec.encode(data).write(to: stateURL(slot), options: .atomic)
        if let thumbnail { Self.writePNG(thumbnail, to: thumbURL(slot)) }
    }

    func loadState(slot: Int) -> Data? {
        (try? Data(contentsOf: stateURL(slot))).flatMap { StateCodec.decode($0, expectedSize: EmulatorCore.stateSize) }
    }

    func saveResume(_ data: Data) { try? StateCodec.encode(data).write(to: resumeURL, options: .atomic) }
    func loadResume() -> Data? {
        (try? Data(contentsOf: resumeURL)).flatMap { StateCodec.decode($0, expectedSize: EmulatorCore.stateSize) }
    }
    func clearResume() { try? FileManager.default.removeItem(at: resumeURL) }

    func slots() -> [SaveSlot] {
        (1...Self.slotCount).map { i in
            let attrs = try? FileManager.default.attributesOfItem(atPath: stateURL(i).path)
            let date = attrs?[.modificationDate] as? Date
            var image: CGImage?
            if date != nil, let src = CGImageSourceCreateWithURL(thumbURL(i) as CFURL, nil) {
                image = CGImageSourceCreateImageAtIndex(src, 0, nil)
            }
            return SaveSlot(id: i, date: date, thumbnail: image)
        }
    }

    static func writePNG(_ image: CGImage, to url: URL) {
        guard let dst = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil) else { return }
        CGImageDestinationAddImage(dst, image, nil)
        CGImageDestinationFinalize(dst)
    }

    /// Copies a BGRA frame (`width` columns of a `pitch`-wide buffer) into a CGImage.
    static func image(fromFrame pixels: UnsafePointer<UInt32>, width: Int, pitch: Int) -> CGImage? {
        let data = Data(bytes: pixels, count: pitch * 224 * 4)
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        return CGImage(width: width, height: 224, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: pitch * 4,
                       space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: info, provider: provider,
                       decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
}

/// Rewind history as reversible deltas against the newest snapshot.
@MainActor
final class RewindBuffer {
    private var head: [UInt8]
    private var scratch: [UInt8]
    private var deltaScratch: [UInt8]
    private var deltas: [[UInt8]] = []
    private var totalBytes = 0
    private var hasHead = false
    private let budget: Int

    init(stateSize: Int, budgetBytes: Int = 48 << 20) {
        head = [UInt8](repeating: 0, count: stateSize)
        scratch = head
        deltaScratch = [UInt8](repeating: 0, count: smw_delta_bound(stateSize))
        budget = budgetBytes
    }

    var isEmpty: Bool { deltas.isEmpty }

    func reset() {
        deltas.removeAll()
        totalBytes = 0
        hasHead = false
    }

    /// Records the current machine state.
    func push(_ core: EmulatorCore) {
        scratch.withUnsafeMutableBytes { core.saveState(into: $0.baseAddress!) }
        if hasHead {
            let n = head.withUnsafeBufferPointer { h in
                scratch.withUnsafeBufferPointer { s in
                    deltaScratch.withUnsafeMutableBufferPointer { d in
                        smw_delta_encode(s.baseAddress, h.baseAddress, h.count, d.baseAddress)
                    }
                }
            }
            let delta = Array(deltaScratch[0..<n])
            deltas.append(delta)
            totalBytes += delta.count
            while totalBytes > budget, !deltas.isEmpty {
                totalBytes -= deltas.removeFirst().count
            }
        }
        swap(&head, &scratch)
        hasHead = true
    }

    /// Steps one snapshot back and loads it into the core.
    func pop(into core: EmulatorCore) -> Bool {
        guard hasHead, let delta = deltas.popLast() else { return false }
        totalBytes -= delta.count
        head.withUnsafeMutableBufferPointer { h in
            delta.withUnsafeBufferPointer { d in smw_delta_apply(h.baseAddress, d.baseAddress, d.count) }
        }
        head.withUnsafeBytes { _ = core.loadState(from: $0.baseAddress!) }
        return true
    }
}
