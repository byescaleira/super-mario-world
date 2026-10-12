import CryptoKit
import Foundation

public enum ROMError: LocalizedError {
    case notFound, invalid(String)
    public var errorDescription: String? {
        switch self {
        case .notFound: "Nenhuma ROM encontrada."
        case .invalid(let why): why
        }
    }
}

/// Locates and validates the game image. The preferred source is the ROM the
/// build assembled from the SMWDisX disassembly; users can also import their
/// own copy of the cartridge.
public enum ROMLibrary {
    /// SHA-1 of Super Mario World (USA) — also what SMWDisX assembles to.
    public static let usaSHA1 = "6b47bb75d16514b6a476aa0c73a683a2a4c18765"

    static var importedURL: URL {
        URL.applicationSupportDirectory.appending(path: "ROM/smw.sfc")
    }

    public static func load() throws -> Data {
        if let url = Bundle.main.url(forResource: "smw", withExtension: "sfc"),
           let data = try? Data(contentsOf: url) {
            return try validate(data)
        }
        if let data = try? Data(contentsOf: importedURL) {
            return try validate(data)
        }
        throw ROMError.notFound
    }

    public static func importROM(from url: URL) throws -> Data {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let data = try validate(Data(contentsOf: url))
        try FileManager.default.createDirectory(at: importedURL.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        try data.write(to: importedURL, options: .atomic)
        return data
    }

    static func validate(_ input: Data) throws -> Data {
        var data = input
        // Strip a 512-byte copier header if present.
        if data.count % 1024 == 512 { data = data.dropFirst(512) }
        guard data.count >= 0x80000 else { throw ROMError.invalid("Arquivo pequeno demais para ser Super Mario World.") }
        let title = String(decoding: data[0x7FC0..<0x7FD5], as: UTF8.self)
        guard title.hasPrefix("SUPER MARIOWORLD") else {
            throw ROMError.invalid("Esta ROM não é Super Mario World (título interno: \(title.trimmingCharacters(in: .whitespaces))).")
        }
        return Data(data)
    }

    public static func sha1(_ data: Data) -> String {
        Insecure.SHA1.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }
}
