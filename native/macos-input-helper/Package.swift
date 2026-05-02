// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "macos-input-helper",
    platforms: [.macOS(.v12)],
    products: [
        .executable(name: "macos-input-helper", targets: ["macos-input-helper"])
    ],
    targets: [
        .executableTarget(name: "macos-input-helper")
    ]
)
