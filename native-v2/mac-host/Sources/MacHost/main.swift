import Foundation
import CoreGraphics
import AppKit
import Darwin

@available(macOS 13.0, *)
private struct CapturePipeline {
    let output: ScreenCaptureOutput
    let capturer: ScreenCapturer
}

@available(macOS 13.0, *)
final actor NativeHostRuntime {
    let tcpServer: TcpVideoServer?
    let udpSender: UdpVideoSender?
    let input: InputReceiver
    private var cfg: NativeHostConfig
    private var encoder: H264LowLatencyEncoder
    private var output: ScreenCaptureOutput
    private var capturer: ScreenCapturer
    private var reconfiguring = false

    init(
        cfg: NativeHostConfig,
        tcpServer: TcpVideoServer?,
        udpSender: UdpVideoSender?,
        encoder: H264LowLatencyEncoder,
        output: ScreenCaptureOutput,
        capturer: ScreenCapturer,
        input: InputReceiver
    ) {
        self.cfg = cfg
        self.tcpServer = tcpServer
        self.udpSender = udpSender
        self.encoder = encoder
        self.output = output
        self.capturer = capturer
        self.input = input
    }

    func requestKeyframe(reason: String) {
        encoder.requestKeyframe(reason: reason)
    }

    func updateBitrate(_ bitrate: Int, reason: String) {
        encoder.updateBitrate(bitrate, reason: reason)
        udpSender?.updateTargetBitrate(bitrate)
    }

    func reconfigureVideo(width: Int, height: Int, fps: Int, bitrateMbps: Int) async {
        if reconfiguring {
            logLine("[control] video profile change ignored: reconfiguration already in progress")
            return
        }

        let nextCfg = resolvedConfig(width: width, height: height, fps: fps, bitrateMbps: bitrateMbps)
        if nextCfg.width == cfg.width && nextCfg.height == cfg.height && nextCfg.fps == cfg.fps && nextCfg.bitrate == cfg.bitrate {
            encoder.requestKeyframe(reason: "video profile unchanged")
            return
        }

        reconfiguring = true
        defer { reconfiguring = false }

        let previousCfg = cfg
        let previousEncoder = encoder
        logLine("[control] reconfiguring video \(previousCfg.width)x\(previousCfg.height)@\(previousCfg.fps) -> \(nextCfg.width)x\(nextCfg.height)@\(nextCfg.fps) bitrate=\(nextCfg.bitrate)")

        do {
            let newEncoder = try makeEncoder(cfg: nextCfg)
            await capturer.stop()
            let newPipeline = try await makePipeline(cfg: nextCfg, encoder: newEncoder)
            cfg = nextCfg
            encoder = newEncoder
            udpSender?.updateTargetBitrate(nextCfg.bitrate)
            output = newPipeline.output
            capturer = newPipeline.capturer
            logLine("[control] video profile applied \(nextCfg.width)x\(nextCfg.height)@\(nextCfg.fps) bitrate=\(nextCfg.bitrate)")
        } catch {
            logLine("[control] video profile apply failed: \(error.localizedDescription)")
            do {
                let restorePipeline = try await makePipeline(cfg: previousCfg, encoder: previousEncoder)
                cfg = previousCfg
                encoder = previousEncoder
                udpSender?.updateTargetBitrate(previousCfg.bitrate)
                output = restorePipeline.output
                capturer = restorePipeline.capturer
                logLine("[control] restored previous video profile \(previousCfg.width)x\(previousCfg.height)@\(previousCfg.fps) bitrate=\(previousCfg.bitrate)")
            } catch {
                logLine("[control] restore previous video profile failed: \(error.localizedDescription)")
            }
        }
    }

    private func resolvedConfig(width: Int, height: Int, fps: Int, bitrateMbps: Int) -> NativeHostConfig {
        var next = cfg
        next.width = max(640, width - (width % 2))
        next.height = max(360, height - (height % 2))
        next.fps = max(30, fps)
        if bitrateMbps > 0 {
            next.bitrate = max(1, min(200, bitrateMbps)) * 1_000_000
        } else {
            next.bitrate = autoBitrate(width: next.width, height: next.height, fallback: cfg.bitrate)
        }
        return next
    }

    private func autoBitrate(width: Int, height: Int, fallback: Int) -> Int {
        let pixels = width * height
        var bitrate = max(8_000_000, fallback)
        if pixels <= 1280 * 720 {
            bitrate = max(bitrate, 8_000_000)
        } else if pixels <= 1600 * 900 {
            bitrate = max(bitrate, 10_000_000)
        } else if pixels <= 1920 * 1080 {
            bitrate = max(bitrate, 12_000_000)
        } else if pixels <= 1920 * 1200 {
            bitrate = max(bitrate, 14_000_000)
        } else if pixels <= 2560 * 1440 {
            bitrate = max(bitrate, 18_000_000)
        } else {
            bitrate = max(bitrate, 24_000_000)
        }
        return bitrate
    }

    private func makeEncoder(cfg: NativeHostConfig) throws -> H264LowLatencyEncoder {
        try H264LowLatencyEncoder(
            width: cfg.width,
            height: cfg.height,
            fps: cfg.fps,
            bitrate: cfg.bitrate,
            keyframeSeconds: cfg.keyframeSeconds
        ) { [tcpServer, udpSender] frame, keyframe, configIncluded, ptsUs in
            if let udpSender {
                udpSender.sendFrame(frame, keyframe: keyframe, configIncluded: configIncluded, ptsUs: ptsUs)
            } else {
                tcpServer?.sendFrame(frame, keyframe: keyframe, configIncluded: configIncluded, ptsUs: ptsUs)
            }
        }
    }

    private func makePipeline(cfg: NativeHostConfig, encoder: H264LowLatencyEncoder) async throws -> CapturePipeline {
        let capturerRef = RefBox<ScreenCapturer?>(nil)
        let output = ScreenCaptureOutput(encoder: encoder, keyframeSeconds: cfg.keyframeSeconds) {
            capturerRef.value?.markFirstFrame()
        }
        let capturer = ScreenCapturer(cfg: cfg, output: output)
        capturerRef.value = capturer
        _ = try await capturer.start()
        return CapturePipeline(output: output, capturer: capturer)
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
            let tcpServer: TcpVideoServer?
            let udpSender: UdpVideoSender?
            if cfg.transport == "udp" {
                udpSender = try UdpVideoSender(clientIP: cfg.clientIP, port: cfg.videoPort, fps: cfg.fps, bitrate: cfg.bitrate)
                tcpServer = nil
            } else {
                tcpServer = try TcpVideoServer(port: cfg.videoPort)
                tcpServer?.start()
                udpSender = nil
            }
            let encoder = try H264LowLatencyEncoder(width: cfg.width, height: cfg.height, fps: cfg.fps, bitrate: cfg.bitrate, keyframeSeconds: cfg.keyframeSeconds) { frame, keyframe, configIncluded, ptsUs in
                if let udpSender {
                    udpSender.sendFrame(frame, keyframe: keyframe, configIncluded: configIncluded, ptsUs: ptsUs)
                } else {
                    tcpServer?.sendFrame(frame, keyframe: keyframe, configIncluded: configIncluded, ptsUs: ptsUs)
                }
            }
            let capturerRef = RefBox<ScreenCapturer?>(nil)
            let output = ScreenCaptureOutput(encoder: encoder, keyframeSeconds: cfg.keyframeSeconds) {
                capturerRef.value?.markFirstFrame()
            }
            let capturer = ScreenCapturer(cfg: cfg, output: output)
            capturerRef.value = capturer
            let displayBounds = try await capturer.start()

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
            gRuntime = NativeHostRuntime(
                cfg: cfg,
                tcpServer: tcpServer,
                udpSender: udpSender,
                encoder: encoder,
                output: output,
                capturer: capturer,
                input: input
            )
            input.start()
            logLine("[ready] runtime retained encoder/output/capturer/input")
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
