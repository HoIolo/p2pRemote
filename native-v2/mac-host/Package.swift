// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "P2PNativeMacHost",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "p2p-native-mac-host", targets: ["MacHost"])
    ],
    targets: [
        .executableTarget(
            name: "MacHost",
            path: "Sources/MacHost",
            exclude: ["Info.plist"],
            swiftSettings: [
                .unsafeFlags(["-parse-as-library"])
            ],
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("ScreenCaptureKit"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreGraphics"),
                .linkedFramework("ApplicationServices")
            ]
        )
    ]
)
