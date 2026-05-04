import Foundation
import CoreGraphics
import AppKit
import ScreenCaptureKit
import Darwin

@available(macOS 13.0, *)
private func mainDisplayBounds() async throws -> CGRect {
    let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
    let displays = content.displays
    guard !displays.isEmpty else {
        throw NSError(domain: "P2PNative", code: 1, userInfo: [NSLocalizedDescriptionKey: "No capturable display found"])
    }
    let mainId = CGMainDisplayID()
    let display = displays.first(where: { CGDirectDisplayID($0.displayID) == mainId })
      ?? displays.max(by: {
          let lhs = CGDisplayBounds(CGDirectDisplayID($0.displayID))
          let rhs = CGDisplayBounds(CGDirectDisplayID($1.displayID))
          return lhs.width * lhs.height < rhs.width * rhs.height
      })!
    let bounds = CGDisplayBounds(CGDirectDisplayID(display.displayID))
    logLine("[capture] selected display=\(display.displayID) bounds=\(Int(bounds.width))x\(Int(bounds.height))")
    return bounds
}

@available(macOS 13.0, *)
final actor NativeHostRuntime {
    let video: GStreamerVideoSender
    let input: InputReceiver
    private var cfg: NativeHostConfig

    init(cfg: NativeHostConfig, video: GStreamerVideoSender, input: InputReceiver) {
        self.cfg = cfg
        self.video = video
        self.input = input
    }

    func requestKeyframe(reason: String) {
        logLine("[control] keyframe request ignored by gst backend: \(reason)")
    }

    func updateBitrate(_ bitrate: Int, reason: String) {
        logLine("[control] bitrate update requested (restart required for gst backend): \(bitrate) reason=\(reason)")
    }

    func reconfigureVideo(width: Int, height: Int, fps: Int, bitrateMbps: Int) async {
        logLine("[control] video profile request ignored by gst backend: \(width)x\(height)@\(fps) bitrateMbps=\(bitrateMbps)")
    }
}

@available(macOS 13.0, *)
private var gRuntime: NativeHostRuntime?

@main
struct MacHostMain {
    static func main() {
        guard #available(macOS 13.0, *) else {
            fputs("P2P Native v2 host requires macOS 13+ for ScreenCaptureKit.\n", stderr)
            exit(2)
        }

        setbuf(__stdoutp, nil)
        setbuf(__stderrp, nil)
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)

        Task {
            await run()
        }
        RunLoop.main.run()
    }

    @available(macOS 13.0, *)
    private static func run() async {
        let cfg = NativeHostConfig.parse()
        logLine("P2P Native v2 Mac Host")
        logLine("client=\(cfg.clientIP):\(cfg.videoPort), input=0.0.0.0:\(cfg.inputPort), video=\(cfg.width)x\(cfg.height)@\(cfg.fps), bitrate=\(cfg.bitrate), transport=\(cfg.transport)")

        do {
            let displayBounds = try await mainDisplayBounds()
            let video = GStreamerVideoSender(cfg: cfg)
            try video.start()

            let input = try InputReceiver(
                port: cfg.inputPort,
                displayBounds: displayBounds,
                onKeyframeRequest: {
                    Task {
                        await gRuntime?.requestKeyframe(reason: "windows client loss recovery")
                    }
                },
                onVideoProfileRequest: { width, height, fps, bitrateMbps in
                    Task {
                        await gRuntime?.reconfigureVideo(width: width, height: height, fps: fps, bitrateMbps: bitrateMbps)
                    }
                },
                onBitrateRequest: { bitrate in
                    Task {
                        await gRuntime?.updateBitrate(bitrate, reason: "windows adaptive control")
                    }
                }
            )
            gRuntime = NativeHostRuntime(cfg: cfg, video: video, input: input)
            input.start()
            logLine("[ready] gst video sender and input receiver running")
            logLine("[ready] host streaming. Press Ctrl+C to stop.")
        } catch {
            fputs("[fatal] \(error)\n", stderr)
            fflush(stderr)
            exit(1)
        }
    }
}

final class RefBox<T> {
    var value: T
    init(_ value: T) {
        self.value = value
    }
}
