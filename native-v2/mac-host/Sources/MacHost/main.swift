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
    let capturer: ScreenCapturer
    let encoder: H264LowLatencyEncoder
    let udpVideo: UdpVideoSender?
    let tcpVideo: TcpVideoServer?
    let gstVideo: GStreamerVideoSender?
    let input: InputReceiver
    private var cfg: NativeHostConfig
    private var lastStatsLogUs: UInt64 = 0
    private var lastAdaptiveChangeUs: UInt64 = 0
    private var stableStatsCount: UInt32 = 0

    init(
        cfg: NativeHostConfig,
        capturer: ScreenCapturer,
        encoder: H264LowLatencyEncoder,
        udpVideo: UdpVideoSender?,
        tcpVideo: TcpVideoServer?,
        gstVideo: GStreamerVideoSender?,
        input: InputReceiver
    ) {
        self.cfg = cfg
        self.capturer = capturer
        self.encoder = encoder
        self.udpVideo = udpVideo
        self.tcpVideo = tcpVideo
        self.gstVideo = gstVideo
        self.input = input
    }

    func requestKeyframe(reason: String) {
        logLine("[control] keyframe request: \(reason)")
        if let gstVideo {
            gstVideo.requestKeyframe()
        } else {
            encoder.requestKeyframe(reason: reason)
        }
    }

    func updateBitrate(_ bitrate: Int, reason: String) {
        let clamped = max(2_000_000, min(200_000_000, bitrate))
        if let gstVideo {
            do {
                try gstVideo.updateBitrate(clamped)
            } catch {
                logLine("[gst] bitrate update failed: \(error)")
            }
        } else {
            encoder.updateBitrate(clamped, reason: reason)
            udpVideo?.updateTargetBitrate(clamped)
        }
        cfg.bitrate = clamped
    }

    func handleClientStats(networkDropped: UInt64, clientDropped: UInt64, jitterMs: Int, latencyMs: Int, trouble: Bool) {
        let now = nowUs()
        let fps = max(30, cfg.fps)
        let frameMs = max(1, 1000 / fps)
        let overloaded = trouble || jitterMs > frameMs || latencyMs > frameMs * 4
        if now - lastStatsLogUs >= 1_000_000 {
            logLine("[feedback] drops net=\(networkDropped) client=\(clientDropped) jitter=\(jitterMs)ms latency=\(latencyMs)ms trouble=\(trouble) bitrate=\(cfg.bitrate)")
            lastStatsLogUs = now
        }
        if overloaded {
            stableStatsCount = 0
            if now - lastAdaptiveChangeUs >= 1_500_000 {
                let next = max(8_000_000, cfg.bitrate * 85 / 100)
                if next < cfg.bitrate {
                    updateBitrate(next, reason: "client feedback congestion")
                    lastAdaptiveChangeUs = now
                }
            }
        } else if jitterMs <= max(2, frameMs / 2) && latencyMs <= frameMs * 3 {
            stableStatsCount &+= 1
            if stableStatsCount >= 12 && now - lastAdaptiveChangeUs >= 4_000_000 {
                let next = min(80_000_000, cfg.bitrate * 105 / 100)
                if next > cfg.bitrate {
                    updateBitrate(next, reason: "client feedback stable")
                    lastAdaptiveChangeUs = now
                }
                stableStatsCount = 0
            }
        } else {
            stableStatsCount = 0
        }
    }

    func reconfigureVideo(width: Int, height: Int, fps: Int, bitrateMbps: Int) async {
        let nextWidth = max(640, width)
        let nextHeight = max(360, height)
        let nextFps = max(30, min(240, fps))
        let nextBitrate = bitrateMbps > 0 ? bitrateMbps * 1_000_000 : cfg.bitrate

        let shapeChanged = nextWidth != cfg.width || nextHeight != cfg.height || nextFps != cfg.fps
        if !shapeChanged {
            updateBitrate(nextBitrate, reason: "manual profile")
            requestKeyframe(reason: "bitrate changed")
            logLine("[control] bitrate-only profile applied live: bitrate=\(nextBitrate)")
            return
        }

        var nextCfg = cfg
        nextCfg.width = nextWidth
        nextCfg.height = nextHeight
        nextCfg.fps = nextFps
        nextCfg.bitrate = max(2_000_000, min(80_000_000, nextBitrate))

        let started = nowUs()
        logLine("[control] live profile reconfigure start: \(cfg.width)x\(cfg.height)@\(cfg.fps) -> \(nextCfg.width)x\(nextCfg.height)@\(nextCfg.fps) bitrate=\(nextCfg.bitrate)")
        do {
            if let gstVideo {
                try gstVideo.reconfigure(width: nextCfg.width, height: nextCfg.height, fps: nextCfg.fps, bitrateMbps: max(1, nextCfg.bitrate / 1_000_000))
                cfg = nextCfg
                requestKeyframe(reason: "profile changed")
                let elapsedMs = Double(nowUs() - started) / 1000.0
                logLine(String(format: "[control] gst profile reconfigure done: %dx%d@%d bitrate=%d elapsed=%.0f ms",
                               nextCfg.width, nextCfg.height, nextCfg.fps, nextCfg.bitrate, elapsedMs))
                return
            }
            await capturer.stop()
            try encoder.reconfigure(
                width: nextCfg.width,
                height: nextCfg.height,
                fps: nextCfg.fps,
                bitrate: nextCfg.bitrate,
                keyframeSeconds: nextCfg.keyframeSeconds,
                reason: "manual profile"
            )
            cfg = nextCfg
            udpVideo?.updateTargetBitrate(nextCfg.bitrate)
            capturer.updateConfig(nextCfg)
            _ = try await capturer.start()
            requestKeyframe(reason: "profile changed")
            let elapsedMs = Double(nowUs() - started) / 1000.0
            logLine(String(format: "[control] live profile reconfigure done: %dx%d@%d bitrate=%d elapsed=%.0f ms",
                           nextCfg.width, nextCfg.height, nextCfg.fps, nextCfg.bitrate, elapsedMs))
        } catch {
            logLine("[control] live profile reconfigure failed: \(error)")
        }
    }

    func stopForExit() async {
        gstVideo?.stop()
        await capturer.stop()
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

        signal(SIGINT) { _ in
            logLine("[signal] SIGINT received, exiting")
            exit(0)
        }
        signal(SIGTERM) { _ in
            logLine("[signal] SIGTERM received, exiting")
            exit(0)
        }

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
        logLine("client=\(cfg.clientIP):\(cfg.videoPort), input=0.0.0.0:\(cfg.inputPort), video=\(cfg.width)x\(cfg.height)@\(cfg.fps), bitrate=\(cfg.bitrate), transport=\(cfg.transport), captureMode=\(cfg.captureMode), hideHostCursor=\(cfg.hideHostCursor)")

        do {
            let gstVideo: GStreamerVideoSender? = cfg.transport == "gst" ? GStreamerVideoSender(cfg: cfg) : nil
            let udpVideo = cfg.transport == "udp" ? try UdpVideoSender(clientIP: cfg.clientIP, port: cfg.videoPort, fps: cfg.fps, bitrate: cfg.bitrate) : nil
            let tcpVideo = cfg.transport == "tcp" ? try TcpVideoServer(port: cfg.videoPort) : nil
            tcpVideo?.start()
            if let gstVideo {
                try gstVideo.start()
            }
            let encoder = try H264LowLatencyEncoder(
                width: cfg.width,
                height: cfg.height,
                fps: cfg.fps,
                bitrate: cfg.bitrate,
                keyframeSeconds: cfg.keyframeSeconds
            ) { frame, keyframe, configIncluded, ptsUs in
                if let udpVideo {
                    udpVideo.sendFrame(frame, keyframe: keyframe, configIncluded: configIncluded, ptsUs: ptsUs)
                } else if let tcpVideo {
                    tcpVideo.sendFrame(frame, keyframe: keyframe, configIncluded: configIncluded, ptsUs: ptsUs)
                }
            }
            let capturerRef = RefBox<ScreenCapturer?>(nil)
            let output = ScreenCaptureOutput(
                encoder: encoder,
                shouldEncodeFrame: {
                    if gstVideo != nil {
                        return false
                    }
                    // UDP path is latency sensitive: do not feed VideoToolbox
                    // while a frame is already encoded or waiting to be sent.
                    // This prefers a fresh capture sample over completing stale
                    // work queued in the pipeline. TCP keeps the old behavior.
                    if let udpVideo {
                        return encoder.canAcceptFrame() && !udpVideo.hasQueuedFrameForSend()
                    }
                    return true
                },
                firstFrameCallback: {
                    capturerRef.value?.markFirstFrame()
                }
            )
            let capturer = ScreenCapturer(cfg: cfg, output: output)
            capturerRef.value = capturer
            let displayBounds = gstVideo == nil ? try await capturer.start() : try await mainDisplayBounds()

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
                },
                onClientStats: { networkDropped, clientDropped, jitterMs, latencyMs, trouble in
                    Task {
                        await gRuntime?.handleClientStats(networkDropped: networkDropped, clientDropped: clientDropped, jitterMs: jitterMs, latencyMs: latencyMs, trouble: trouble)
                    }
                },
                onClientTimeout: {
                    logLine("[control] client disconnected (timeout), stopping host")
                    exit(0)
                }
            )
            gRuntime = NativeHostRuntime(
                cfg: cfg,
                capturer: capturer,
                encoder: encoder,
                udpVideo: udpVideo,
                tcpVideo: tcpVideo,
                gstVideo: gstVideo,
                input: input
            )
            input.start()
            logLine(gstVideo == nil ? "[ready] native VideoToolbox sender and input receiver running" : "[ready] GStreamer RTP sender and input receiver running")
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
