import AVFoundation
import SMWCore

/// Real-time audio: an AVAudioSourceNode pulls straight from the lock-free C
/// ring (no Swift allocation or locking on the render thread) and resamples
/// 32 kHz S-DSP output to the hardware rate.
@MainActor
final class AudioOutput {
    /// Holds the C ring; only touched through its thread-safe C API.
    private final class RingBox: @unchecked Sendable {
        let ring: OpaquePointer
        nonisolated(unsafe) var volume: Float = 1
        init() { ring = smw_audio_ring_create(8192, Double(SMW_AUDIO_RATE)) }
        deinit { smw_audio_ring_destroy(ring) }
    }

    private let engine = AVAudioEngine()
    private let box = RingBox()
    private var source: AVAudioSourceNode?
    private var started = false

    var ring: OpaquePointer { box.ring }

    var volume: Float {
        get { box.volume }
        set { box.volume = newValue }
    }

    init() {
        #if os(iOS) || os(tvOS)
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.ambient, mode: .default)
        try? session.setPreferredIOBufferDuration(0.005)
        try? session.setActive(true)
        #endif
        let rate = engine.outputNode.outputFormat(forBus: 0).sampleRate
        let outputRate = rate > 0 ? rate : 48000
        guard let format = AVAudioFormat(standardFormatWithSampleRate: outputRate, channels: 2) else { return }
        let box = self.box
        // ~3 frames of latency: enough headroom for display-link jitter.
        smw_audio_ring_set_target(box.ring, UInt32(Double(SMW_AUDIO_RATE) * 0.05))
        let node = AVAudioSourceNode(format: format, renderBlock: Self.makeRenderBlock(box: box, outputRate: outputRate))
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: format)
        source = node
    }

    /// Built outside the main actor: the block runs on the real-time audio thread.
    private nonisolated static func makeRenderBlock(box: RingBox, outputRate: Double) -> AVAudioSourceNodeRenderBlock {
        { _, _, frameCount, bufferList -> OSStatus in
            let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
            guard buffers.count >= 2,
                  let l = buffers[0].mData?.assumingMemoryBound(to: Float.self),
                  let r = buffers[1].mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            smw_audio_ring_render(box.ring, l, r, Int32(frameCount), outputRate, box.volume)
            return noErr
        }
    }

    func start() {
        guard !started else { return }
        smw_audio_ring_clear(box.ring)
        do {
            try engine.start()
            started = true
        } catch {
            print("Audio start failed: \(error)")
        }
    }

    func stop() {
        guard started else { return }
        engine.pause()
        started = false
    }
}
