import Foundation
import SMWCore
import Testing
@testable import SMWEngine

/// Builds a save image with file `index` holding `exits`, using the game's own
/// checksum rule (bytes 0...140 plus the 16-bit checksum sum to $5A5A).
private func saveImage(files: [Int: UInt8], backupsOnly: Set<Int> = [], size: Int = 2048) -> [UInt8] {
    var sram = [UInt8](repeating: 0, count: size)
    for (index, exits) in files {
        var file = [UInt8](repeating: 0, count: SMWSave.fileSize)
        for i in 0..<140 { file[i] = UInt8((i * 7 + Int(exits)) & 0xFF) }
        file[SMWSave.exitCountOffset] = exits
        let sum = file[0...140].reduce(0) { $0 + Int($1) }
        let checksum = (SMWSave.checksumBase - sum) & 0xFFFF
        file[141] = UInt8(checksum & 0xFF)
        file[142] = UInt8(checksum >> 8)
        var offsets = [SMWSave.backupOffset + index * SMWSave.fileSize]
        if !backupsOnly.contains(index) { offsets.append(index * SMWSave.fileSize) }
        for o in offsets { sram.replaceSubrange(o..<o + SMWSave.fileSize, with: file) }
    }
    return sram
}

@Suite("Cartridge save")
struct SaveTests {
    @Test func checksumMatchesGameRule() {
        let sram = saveImage(files: [0: 11])
        #expect(SMWSave.isValid(sram, at: 0))
        #expect(!SMWSave.isValid(sram, at: SMWSave.fileSize)) // empty file B
        var broken = sram
        broken[10] ^= 0xFF
        #expect(!SMWSave.isValid(broken, at: 0))
    }

    @Test func fallsBackToBackupCopy() throws {
        var sram = saveImage(files: [1: 20])
        sram[SMWSave.fileSize + 3] ^= 0x55 // corrupt primary of file B
        let file = try #require(SMWSave.file(sram, 1))
        #expect(SMWSave.exits(file) == 20)
    }

    @Test func mergeNeverLosesProgress() throws {
        let now = Date()
        let newer = saveImage(files: [0: 5, 1: 30])          // file A regressed?
        let older = saveImage(files: [0: 12, 2: 3])           // more exits in A, only copy of C
        let merged = try #require(SMWSave.merge([(now, newer), (now.addingTimeInterval(-60), older)]))
        #expect(SMWSave.exits(try #require(SMWSave.file(merged, 0))) == 12)
        #expect(SMWSave.exits(try #require(SMWSave.file(merged, 1))) == 30)
        #expect(SMWSave.exits(try #require(SMWSave.file(merged, 2))) == 3)
    }

    @Test func erasedFileStaysErased() throws {
        let now = Date()
        let current = saveImage(files: [1: 7])                // player erased file A just now
        let backup = saveImage(files: [0: 40, 1: 7])
        let merged = try #require(SMWSave.merge([(now, current), (now.addingTimeInterval(-300), backup)],
                                                erased: [0: now.addingTimeInterval(-1)]))
        #expect(SMWSave.file(merged, 0) == nil)
        #expect(SMWSave.exits(try #require(SMWSave.file(merged, 1))) == 7)
    }

    @Test func stateCodecRoundTripAndVersionCheck() throws {
        var raw = Data(count: 4096)
        raw[100] = 42
        let encoded = StateCodec.encode(raw)
        #expect(encoded.count < raw.count)
        #expect(StateCodec.decode(encoded, expectedSize: 4096) == raw)
        #expect(StateCodec.decode(encoded, expectedSize: 4097) == nil) // other build
        #expect(StateCodec.decode(Data("junk".utf8), expectedSize: 4096) == nil)
    }
}

@Suite("Runtime")
struct RuntimeTests {
    static let rom: Data? = {
        let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .appending(path: "../../../Generated/smw.sfc").standardized
        return try? Data(contentsOf: url)
    }()

    private func makeCore() throws -> OpaquePointer {
        let rom = try #require(Self.rom, "Run scripts/build_rom.sh first")
        return try #require(rom.withUnsafeBytes { smw_create($0.baseAddress!.assumingMemoryBound(to: UInt8.self), $0.count) })
    }

    @Test func loadingAStateNeverRollsBackTheCartridgeSave() throws {
        let core = try makeCore()
        defer { smw_destroy(core) }
        for _ in 0..<30 { smw_run_frame(core) }
        var state = [UInt8](repeating: 0, count: smw_state_size())
        smw_save_state(core, &state)
        var length = 0
        let sram = try #require(smw_sram(core, &length))
        sram[0] = 0xAB                       // the game "saves" after the state was taken
        #expect(smw_load_state(core, state, state.count))
        #expect(sram[0] == 0xAB)
    }

    @Test func emulationIsDeterministic() throws {
        let a = try makeCore(), b = try makeCore()
        defer { smw_destroy(a); smw_destroy(b) }
        for frame in 0..<900 {
            let buttons: UInt16 = frame % 90 < 4 ? UInt16(SMW_BTN_START) : 0
            smw_set_buttons(a, 0, buttons)
            smw_set_buttons(b, 0, buttons)
            smw_run_frame(a)
            smw_run_frame(b)
        }
        let wa = smw_wram(a)!, wb = smw_wram(b)!
        #expect(memcmp(wa, wb, 0x20000) == 0)
    }

    @Test func statesReplayExactly() throws {
        let core = try makeCore()
        defer { smw_destroy(core) }
        for _ in 0..<600 { smw_run_frame(core) }
        var state = [UInt8](repeating: 0, count: smw_state_size())
        smw_save_state(core, &state)
        for _ in 0..<120 { smw_run_frame(core) }
        let first = Data(bytes: smw_wram(core)!, count: 0x20000)
        #expect(smw_load_state(core, state, state.count))
        for _ in 0..<120 { smw_run_frame(core) }
        #expect(Data(bytes: smw_wram(core)!, count: 0x20000) == first)
    }
}
