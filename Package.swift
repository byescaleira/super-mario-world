// swift-tools-version: 6.2
import PackageDescription

let package = Package(
    name: "SMWKit",
    platforms: [.iOS(.v26), .macOS(.v26), .tvOS(.v26)],
    products: [
        .library(name: "SMWEngine", targets: ["SMWEngine"]),
    ],
    targets: [
        // Native SNES runtime (C). Executes the game assembled from SMWDisX.
        .target(
            name: "SMWCore",
            path: "Core",
            sources: ["src"],
            publicHeadersPath: "include",
            cSettings: [.define("NDEBUG", .when(configuration: .release))]
        ),
        // Platform layer: Metal renderer, audio, input, haptics, saves.
        .target(
            name: "SMWEngine",
            dependencies: ["SMWCore"],
            path: "Engine/Sources/SMWEngine",
            resources: [.copy("Resources/Shaders.msl")],
            swiftSettings: [.swiftLanguageMode(.v6)],
            // Xcode runs the App Intents extractor on every Swift module and
            // warns when the framework isn't linked.
            linkerSettings: [.linkedFramework("AppIntents")]
        ),
        .testTarget(
            name: "SMWEngineTests",
            dependencies: ["SMWEngine", "SMWCore"],
            path: "Engine/Tests/SMWEngineTests"
        ),
    ]
)
