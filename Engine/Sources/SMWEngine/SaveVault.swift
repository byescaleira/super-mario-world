import Foundation

/// The cartridge save as SMW lays it out (SMWDisX rammap.asm / bank_00
/// SaveTheGame): three 143-byte files, each mirrored by a backup copy 3 files
/// later. Byte 140 is the number of exits beaten; the last two bytes make the
/// 8-bit sum of bytes 0...140 plus the checksum equal $5A5A.
enum SMWSave {
    static let fileSize = 143
    static let fileCount = 3
    static let backupOffset = 3 * fileSize
    static let exitCountOffset = 140
    static let checksumBase = 0x5A5A

    static func isValid(_ sram: [UInt8], at offset: Int) -> Bool {
        guard offset + fileSize <= sram.count else { return false }
        var sum = Int(sram[offset + 141]) | (Int(sram[offset + 142]) << 8)
        for i in 0...140 { sum += Int(sram[offset + i]) }
        return sum & 0xFFFF == checksumBase
    }

    /// The valid copy of save file `index` (primary first, then backup).
    static func file(_ sram: [UInt8], _ index: Int) -> ArraySlice<UInt8>? {
        for offset in [index * fileSize, backupOffset + index * fileSize] where isValid(sram, at: offset) {
            return sram[offset..<offset + fileSize]
        }
        return nil
    }

    static func exits(_ file: ArraySlice<UInt8>) -> Int { Int(file[file.startIndex + exitCountOffset]) }

    /// Combines save images file by file: a valid file beats a broken or empty
    /// one, more exits beat fewer, and ties go to the newest source, so no save
    /// file moves backwards. A file the player erased in-game stays erased:
    /// only copies newer than its erase date count. `sources` newest first.
    static func merge(_ sources: [(date: Date, sram: [UInt8])], erased: [Int: Date] = [:]) -> [UInt8]? {
        guard var out = sources.first?.sram else { return nil }
        for index in 0..<fileCount {
            var best: ArraySlice<UInt8>?
            for source in sources {
                if let erasedAt = erased[index], source.date < erasedAt { continue }
                guard let f = file(source.sram, index) else { continue }
                if best == nil || exits(f) > exits(best!) { best = f }
            }
            for offset in [index * fileSize, backupOffset + index * fileSize] {
                if let best {
                    out.replaceSubrange(offset..<offset + fileSize, with: best)
                } else if erased[index] != nil {
                    out.replaceSubrange(offset..<offset + fileSize, with: repeatElement(0, count: fileSize))
                }
            }
        }
        return out
    }

    static func summary(_ sram: [UInt8]) -> String {
        (0..<fileCount).map { i in
            let name = ["A", "B", "C"][i]
            guard let f = file(sram, i) else { return "\(name): vazio" }
            return "\(name): \(exits(f)) saídas"
        }.joined(separator: " · ")
    }
}

/// Keeps the battery save safe: written the frame after the game saves, to a
/// file plus rotating backups, to UserDefaults (the only storage tvOS never
/// purges) and to iCloud key-value storage, which syncs it between iPhone,
/// iPad, Mac and Apple TV. Every read merges all copies file by file.
@MainActor
final class SaveVault {
    private static let key = "smw.sram.v1"
    private static let dateKey = "smw.sram.date"
    private static let erasedKey = "smw.sram.erased"
    private static let backups = 3

    private let root: URL
    private let defaults = UserDefaults.standard
    private let cloud = NSUbiquitousKeyValueStore.default
    private var lastStored: [UInt8]?
    private var lastStoredDate = Date.distantPast
    private var observer: NSObjectProtocol?

    /// Called when iCloud delivers progress made on another device.
    var onRemoteChange: (([UInt8]) -> Void)?

    init() {
        root = URL.applicationSupportDirectory.appending(path: "Saves", directoryHint: .isDirectory)
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        observer = NotificationCenter.default.addObserver(
            forName: NSUbiquitousKeyValueStore.didChangeExternallyNotification, object: cloud, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.cloudChanged() }
        }
        cloud.synchronize()
    }

    private var fileURL: URL { root.appending(path: "smw.srm") }
    private func backupURL(_ n: Int) -> URL { root.appending(path: "smw.srm.bak\(n)") }

    /// The best save available across every copy, or nil on a fresh install.
    func load() -> [UInt8]? {
        var sources: [(Date, [UInt8])] = []
        func add(_ data: Data?, _ date: Date?) {
            if let data, !data.isEmpty { sources.append((date ?? .distantPast, [UInt8](data))) }
        }
        func fileDate(_ url: URL) -> Date? {
            (try? FileManager.default.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date
        }
        add(try? Data(contentsOf: fileURL), fileDate(fileURL))
        for n in 1...Self.backups { add(try? Data(contentsOf: backupURL(n)), fileDate(backupURL(n))) }
        add(defaults.data(forKey: Self.key), defaults.object(forKey: Self.dateKey) as? Date)
        add(cloud.data(forKey: Self.key), cloud.object(forKey: Self.dateKey) as? Date)
        let ordered = sources.sorted { $0.0 > $1.0 }
        let size = ordered.first?.1.count ?? 0
        let merged = SMWSave.merge(ordered.filter { $0.1.count == size }.map { (date: $0.0, sram: $0.1) },
                                   erased: erasedDates())
        lastStored = merged
        lastStoredDate = ordered.first?.0 ?? .distantPast
        return merged
    }

    /// Erase dates per save file, union of local and iCloud records.
    private func erasedDates() -> [Int: Date] {
        var out: [Int: Date] = [:]
        for store in [defaults.dictionary(forKey: Self.erasedKey), cloud.dictionary(forKey: Self.erasedKey)] {
            for (k, v) in store ?? [:] {
                guard let i = Int(k), let d = v as? Date else { continue }
                out[i] = max(out[i] ?? .distantPast, d)
            }
        }
        return out
    }

    private func recordErases(from old: [UInt8]?, to new: [UInt8], at date: Date) {
        guard let old, old.count == new.count else { return }
        var erased = erasedDates()
        var changed = false
        for i in 0..<SMWSave.fileCount where SMWSave.file(old, i) != nil && SMWSave.file(new, i) == nil {
            erased[i] = date
            changed = true
        }
        guard changed else { return }
        let plist = Dictionary(uniqueKeysWithValues: erased.map { (String($0.key), $0.value) })
        defaults.set(plist, forKey: Self.erasedKey)
        cloud.set(plist, forKey: Self.erasedKey)
    }

    func store(_ sram: [UInt8]) {
        guard sram != lastStored else { return }
        let data = Data(sram)
        let now = Date()
        recordErases(from: lastStored, to: sram, at: now)
        // Rotate backups so the previous few saves are always recoverable.
        let fm = FileManager.default
        if fm.fileExists(atPath: fileURL.path) {
            try? fm.removeItem(at: backupURL(Self.backups))
            for n in stride(from: Self.backups - 1, through: 1, by: -1) {
                try? fm.moveItem(at: backupURL(n), to: backupURL(n + 1))
            }
            try? fm.copyItem(at: fileURL, to: backupURL(1))
        }
        try? data.write(to: fileURL, options: .atomic)
        defaults.set(data, forKey: Self.key)
        defaults.set(now, forKey: Self.dateKey)
        cloud.set(data, forKey: Self.key)
        cloud.set(now, forKey: Self.dateKey)
        lastStored = sram
        lastStoredDate = now
    }

    private func cloudChanged() {
        guard let remote = cloud.data(forKey: Self.key).map({ [UInt8]($0) }) else { return }
        let remoteDate = cloud.object(forKey: Self.dateKey) as? Date ?? .distantPast
        let local = lastStored ?? remote
        let sources = [(date: remoteDate, sram: remote), (date: lastStoredDate, sram: local)].sorted { $0.date > $1.date }
        guard remote.count == local.count, let merged = SMWSave.merge(sources, erased: erasedDates()),
              merged != local else { return }
        store(merged)
        onRemoteChange?(merged)
    }
}

/// Save-state container: magic, format version, raw size, LZFSE payload.
enum StateCodec {
    private static let magic: [UInt8] = Array("SMWS".utf8)
    private static let version: UInt32 = 1

    static func encode(_ raw: Data) -> Data {
        var out = Data(magic)
        withUnsafeBytes(of: version.littleEndian) { out.append(contentsOf: $0) }
        withUnsafeBytes(of: UInt32(raw.count).littleEndian) { out.append(contentsOf: $0) }
        let packed = (try? (raw as NSData).compressed(using: .lzfse)) as Data? ?? raw
        out.append(packed)
        return out
    }

    /// nil when the data is damaged or was made by an incompatible build.
    static func decode(_ data: Data, expectedSize: Int) -> Data? {
        guard data.count > 12, Array(data.prefix(4)) == magic else { return nil }
        let header = data.subdata(in: 4..<12)
        let fileVersion = header.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt32.self) }
        let size = header.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 4, as: UInt32.self) }
        guard UInt32(littleEndian: fileVersion) == version, Int(UInt32(littleEndian: size)) == expectedSize,
              let raw = try? (data.subdata(in: 12..<data.count) as NSData).decompressed(using: .lzfse) as Data,
              raw.count == expectedSize else { return nil }
        return raw
    }
}
