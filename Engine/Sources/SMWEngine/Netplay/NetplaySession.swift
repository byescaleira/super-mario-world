import Foundation
import SMWCore

/// A two-device link (SharePlay or a Game Center match). Exactly one side is
/// the host; both sides must agree on which without talking first.
@MainActor
protocol NetplayTransport: AnyObject {
    var name: String { get }
    var peerName: String { get }
    var isHost: Bool { get }
    var onMessage: ((Data) -> Void)? { get set }
    var onDisconnect: (() -> Void)? { get set }
    func send(_ data: Data, reliable: Bool)
    func leave()
}

/// Rollback netplay for SMW's two-player mode (Mario on pad 1, Luigi on pad 2).
///
/// Both devices run the same deterministic game; only button presses travel
/// over the network. Local input is applied `inputDelay` frames late, the
/// remote input is predicted until it arrives, and when a prediction was wrong
/// the game rewinds to that frame (save state) and replays the confirmed
/// inputs in well under a millisecond per frame. Game RAM checksums of
/// confirmed frames catch any desync, which the host repairs by resending its
/// state.
@MainActor
final class NetplaySession {
    enum Phase: Equatable {
        case handshake
        case playing
        case ended(String)
    }

    private enum Kind: UInt8 {
        case helloChunk = 1, ready = 2, inputs = 3, checksum = 4, bye = 5, resync = 6
    }

    static let inputDelay = 2
    static let maxRollback = 10
    private static let ringSize = maxRollback + 6
    private static let chunkSize = 16_000
    private static let checksumInterval = 120

    let transport: NetplayTransport
    private(set) var phase: Phase = .handshake
    private(set) var rollbackFrames = 0
    private(set) var stalled = false
    var localPort: Int { transport.isHost ? 0 : 1 }
    /// Frames the peer's confirmed input lags behind the local simulation.
    var lag: Int { max(0, frame - 1 - remoteContiguous) }

    private let core: EmulatorCore
    private let romHash: Data
    private var frame = 0
    private var local: [Int: UInt16] = [:]
    private var remote: [Int: UInt16] = [:]
    private var used: [Int: UInt16] = [:]
    private var remoteContiguous = -1
    private var peerAck = -1
    private var mispredictedFrom: Int?
    private var ring: [[UInt8]]
    private var ringFrame: [Int]
    private var sentChecksums: Set<Int> = []
    private var localCRC: [Int: UInt32] = [:]
    private var remoteCRC: [Int: UInt32] = [:]
    private var helloChunks: [Int: Data] = [:]
    private var helloCount = 0
    /// The guest's own cartridge save, restored when the session ends.
    private var guestOwnSRAM: Data?

    var onAudio: ((EmulatorCore) -> Void)?
    var onPhaseChange: ((Phase) -> Void)?

    init(transport: NetplayTransport, core: EmulatorCore, romHash: Data) {
        self.transport = transport
        self.core = core
        self.romHash = romHash
        ring = Array(repeating: [UInt8](repeating: 0, count: EmulatorCore.stateSize), count: Self.ringSize)
        ringFrame = Array(repeating: -1, count: Self.ringSize)
        transport.onMessage = { [weak self] data in self?.receive(data) }
        transport.onDisconnect = { [weak self] in self?.end("O outro jogador saiu") }
        if transport.isHost { sendHello() }
    }

    func end(_ reason: String) {
        guard phase != .ended(reason) else { return }
        if case .ended = phase { return }
        transport.send(Data([Kind.bye.rawValue]), reliable: true)
        transport.leave()
        if let own = guestOwnSRAM { core.loadSRAM(own) }
        setPhase(.ended(reason))
    }

    private func setPhase(_ p: Phase) {
        phase = p
        onPhaseChange?(p)
    }

    // MARK: Handshake: the host ships its exact machine state

    private func sendHello() {
        setPhase(.handshake)
        resetTimeline()
        let state = StateCodec.encode(core.saveState())
        let sram = core.sram
        var payload = Data()
        payload.append(romHash)
        payload.appendUInt32(UInt32(sram.count))
        payload.append(sram)
        payload.append(state)
        let chunks = stride(from: 0, to: payload.count, by: Self.chunkSize).map {
            payload.subdata(in: $0..<min($0 + Self.chunkSize, payload.count))
        }
        for (i, chunk) in chunks.enumerated() {
            var msg = Data([Kind.helloChunk.rawValue])
            msg.appendUInt16(UInt16(i))
            msg.appendUInt16(UInt16(chunks.count))
            msg.append(chunk)
            transport.send(msg, reliable: true)
        }
    }

    private func receiveHelloChunk(_ data: Data) {
        guard !transport.isHost, data.count >= 5 else { return }
        let index = Int(data.readUInt16(at: 1)), count = Int(data.readUInt16(at: 3))
        if index == 0 { helloChunks.removeAll() }
        helloCount = count
        helloChunks[index] = data.subdata(in: 5..<data.count)
        guard helloChunks.count == helloCount else { return }
        var payload = Data()
        for i in 0..<helloCount { payload.append(helloChunks[i] ?? Data()) }
        helloChunks.removeAll()

        guard payload.count > 24, payload.prefix(romHash.count) == romHash else {
            end("O outro aparelho está com uma ROM diferente")
            return
        }
        var offset = romHash.count
        let sramSize = Int(payload.readUInt32(at: offset))
        offset += 4
        let sram = payload.subdata(in: offset..<offset + sramSize)
        offset += sramSize
        guard let state = StateCodec.decode(payload.subdata(in: offset..<payload.count),
                                            expectedSize: EmulatorCore.stateSize) else {
            end("Versões diferentes do app")
            return
        }
        if guestOwnSRAM == nil { guestOwnSRAM = core.sram }
        core.loadSRAM(sram)
        core.loadState(state)
        resetTimeline()
        transport.send(Data([Kind.ready.rawValue]), reliable: true)
        setPhase(.playing)
    }

    private func resetTimeline() {
        frame = 0
        local.removeAll()
        remote.removeAll()
        used.removeAll()
        remoteContiguous = -1
        peerAck = -1
        mispredictedFrom = nil
        ringFrame = Array(repeating: -1, count: Self.ringSize)
        sentChecksums.removeAll()
        localCRC.removeAll()
        remoteCRC.removeAll()
    }

    // MARK: Per-frame

    /// Advances the shared game by one frame (or waits for the peer).
    /// Returns false while stalled so the caller keeps showing the last frame.
    func advance(localButtons: UInt16, display: UnsafeMutablePointer<UInt32>) -> Bool {
        guard phase == .playing else { return false }
        if local[frame + Self.inputDelay] == nil { local[frame + Self.inputDelay] = localButtons }
        sendInputs()

        stalled = frame - remoteContiguous > Self.maxRollback
        if stalled { return false }

        if let from = mispredictedFrom, from < frame, ringFrame[from % Self.ringSize] == from {
            core.loadState(from: ring[from % Self.ringSize])
            for f in from..<frame { simulate(f, display: nil, audio: false) }
            rollbackFrames += frame - from
        }
        mispredictedFrom = nil
        simulate(frame, display: display, audio: true)
        frame += 1
        verifyChecksums()
        prune()
        return true
    }

    private func simulate(_ f: Int, display: UnsafeMutablePointer<UInt32>?, audio: Bool) {
        let slot = f % Self.ringSize
        ring[slot].withUnsafeMutableBytes { core.saveState(into: $0.baseAddress!) }
        ringFrame[slot] = f
        let theirs = remote[f] ?? predictedRemote()
        used[f] = theirs
        let mine = local[f] ?? 0
        core.setButtons(localPort == 0 ? mine : theirs, port: 0)
        core.setButtons(localPort == 0 ? theirs : mine, port: 1)
        core.setFramebuffer(display)
        core.runFrame()
        if audio { onAudio?(core) } else { core.discardAudio() }
    }

    private func predictedRemote() -> UInt16 {
        remoteContiguous >= 0 ? (remote[remoteContiguous] ?? 0) : 0
    }

    private func sendInputs() {
        let last = frame + Self.inputDelay
        let first = max(peerAck + 1, last - 40, 0)
        guard first <= last else { return }
        var msg = Data([Kind.inputs.rawValue])
        msg.appendUInt32(UInt32(bitPattern: Int32(remoteContiguous)))
        msg.appendUInt32(UInt32(first))
        msg.append(UInt8(last - first + 1))
        for f in first...last { msg.appendUInt16(local[f] ?? 0) }
        transport.send(msg, reliable: false)
    }

    private func receiveInputs(_ data: Data) {
        guard data.count >= 10 else { return }
        let ack = Int(Int32(bitPattern: data.readUInt32(at: 1)))
        let first = Int(data.readUInt32(at: 5))
        let n = Int(data[data.startIndex + 9])
        guard data.count >= 10 + n * 2 else { return }
        peerAck = max(peerAck, ack)
        for i in 0..<n {
            let f = first + i
            guard remote[f] == nil, f > remoteContiguous else { continue }
            let b = data.readUInt16(at: 10 + i * 2)
            remote[f] = b
            if let guess = used[f], guess != b { mispredictedFrom = min(mispredictedFrom ?? f, f) }
        }
        while remote[remoteContiguous + 1] != nil { remoteContiguous += 1 }
    }

    // MARK: Desync detection

    private func verifyChecksums() {
        // Frames up to remoteContiguous are final on both sides.
        let confirmed = min(remoteContiguous, frame - 1)
        for f in Swift.stride(from: confirmed - confirmed % Self.checksumInterval, through: max(0, confirmed - Self.maxRollback),
                              by: -Self.checksumInterval) where f > 0 && !sentChecksums.contains(f) {
            let slot = f % Self.ringSize
            guard ringFrame[slot] == f else { continue }
            let crc = ring[slot].withUnsafeBytes { smw_state_wram_crc($0.baseAddress!) }
            sentChecksums.insert(f)
            localCRC[f] = crc
            var msg = Data([Kind.checksum.rawValue])
            msg.appendUInt32(UInt32(f))
            msg.appendUInt32(crc)
            transport.send(msg, reliable: true)
            compare(f)
        }
    }

    private func receiveChecksum(_ data: Data) {
        guard data.count >= 9 else { return }
        let f = Int(data.readUInt32(at: 1))
        remoteCRC[f] = data.readUInt32(at: 5)
        compare(f)
    }

    private func compare(_ f: Int) {
        guard let mine = localCRC[f], let theirs = remoteCRC[f] else { return }
        localCRC[f] = nil
        remoteCRC[f] = nil
        guard mine != theirs else { return }
        if transport.isHost { sendHello() } else { transport.send(Data([Kind.resync.rawValue]), reliable: true) }
    }

    private func prune() {
        let keepFrom = min(remoteContiguous, frame - Self.maxRollback) - 2
        guard keepFrom > 0 else { return }
        for key in local.keys where key < keepFrom { local[key] = nil }
        for key in remote.keys where key < keepFrom { remote[key] = nil }
        for key in used.keys where key < keepFrom { used[key] = nil }
    }

    // MARK: Messages

    private func receive(_ data: Data) {
        guard let first = data.first, let kind = Kind(rawValue: first) else { return }
        switch kind {
        case .helloChunk: receiveHelloChunk(data)
        case .ready:
            if transport.isHost, phase == .handshake { setPhase(.playing) }
        case .inputs: receiveInputs(data)
        case .checksum: receiveChecksum(data)
        case .resync: if transport.isHost { sendHello() }
        case .bye: end("O outro jogador saiu")
        }
    }
}

extension Data {
    mutating func appendUInt16(_ v: UInt16) { Swift.withUnsafeBytes(of: v.littleEndian) { append(contentsOf: $0) } }
    mutating func appendUInt32(_ v: UInt32) { Swift.withUnsafeBytes(of: v.littleEndian) { append(contentsOf: $0) } }

    func readUInt16(at offset: Int) -> UInt16 {
        let i = startIndex + offset
        return UInt16(self[i]) | (UInt16(self[i + 1]) << 8)
    }

    func readUInt32(at offset: Int) -> UInt32 {
        let i = startIndex + offset
        return UInt32(self[i]) | (UInt32(self[i + 1]) << 8) | (UInt32(self[i + 2]) << 16) | (UInt32(self[i + 3]) << 24)
    }
}
