import Foundation
import SMWCore

/// Swift face of the C runtime. Confined to the main actor, where the frame
/// loop runs (one frame costs ~1 ms, well inside a 16.6 ms vsync).
@MainActor
final class EmulatorCore {
    private let handle: OpaquePointer
    private var audioScratch = [Int16](repeating: 0, count: 2048 * 2)

    init?(rom: Data) {
        guard let h = rom.withUnsafeBytes({ raw -> OpaquePointer? in
            guard let base = raw.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return nil }
            return smw_create(base, raw.count)
        }) else { return nil }
        handle = h
    }

    isolated deinit {
        smw_destroy(handle)
    }

    func reset(hard: Bool) { smw_reset(handle, hard) }

    func setFramebuffer(_ pointer: UnsafeMutablePointer<UInt32>?) {
        smw_set_framebuffer(handle, pointer, Int32(SMW_MAX_FRAME_WIDTH))
    }

    func setWidescreen(_ margin: Int) { smw_set_widescreen(handle, Int32(margin)) }

    var frameWidth: Int { Int(smw_frame_width(handle)) }

    func setMotionCapture(_ on: Bool) { smw_set_motion_capture(handle, on) }

    /// Draws the moment `t` between the last two displayed frames.
    func renderInterpolated(into pixels: UnsafeMutablePointer<UInt32>, t: Float) -> Bool {
        smw_render_interpolated(handle, pixels, Int32(SMW_MAX_FRAME_WIDTH), t)
    }

    func setButtons(_ buttons: UInt16, port: Int) { smw_set_buttons(handle, Int32(port), buttons) }

    func runFrame() { smw_run_frame(handle) }

    /// Moves this frame's audio into the output ring.
    func drainAudio(into ring: OpaquePointer) {
        audioScratch.withUnsafeMutableBufferPointer { buf in
            let n = smw_read_audio(handle, buf.baseAddress, Int32(buf.count / 2))
            if n > 0 { smw_audio_ring_push(ring, buf.baseAddress, n) }
        }
    }

    func discardAudio() {
        audioScratch.withUnsafeMutableBufferPointer { buf in
            while smw_read_audio(handle, buf.baseAddress, Int32(buf.count / 2)) > 0 {}
        }
    }

    var soundEvents: [UInt16] {
        (0..<Int(smw_sound_event_count(handle))).map { smw_sound_event(handle, Int32($0)) }
    }

    func setHDAudio(_ on: Bool) {
        smw_set_audio_interpolation(handle, on ? SMW_AUDIO_INTERP_CUBIC : SMW_AUDIO_INTERP_GAUSSIAN)
    }

    func wram(_ address: Int) -> UInt8 { smw_wram(handle)[address & 0x1FFFF] }

    var frameCount: UInt64 { smw_frame_count(handle) }

    // MARK: SRAM (battery save)

    var sram: Data {
        var length = 0
        let p = smw_sram(handle, &length)!
        return Data(bytes: p, count: length)
    }

    func takeSRAMDirty() -> Bool { smw_sram_take_dirty(handle) }

    func loadSRAM(_ data: Data) {
        var length = 0
        let p = smw_sram(handle, &length)!
        data.withUnsafeBytes { raw in
            _ = memcpy(p, raw.baseAddress!, min(length, raw.count))
        }
    }

    // MARK: Save states

    static let stateSize = smw_state_size()

    func saveState(into buffer: UnsafeMutableRawPointer) { smw_save_state(handle, buffer) }

    func saveState() -> Data {
        var data = Data(count: Self.stateSize)
        data.withUnsafeMutableBytes { smw_save_state(handle, $0.baseAddress!) }
        return data
    }

    @discardableResult
    func loadState(from buffer: UnsafeRawPointer) -> Bool {
        smw_load_state(handle, buffer, Self.stateSize)
    }

    @discardableResult
    func loadState(_ data: Data) -> Bool {
        data.withUnsafeBytes { smw_load_state(handle, $0.baseAddress!, $0.count) }
    }
}
