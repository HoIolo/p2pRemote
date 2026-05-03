import Foundation
import CoreGraphics
import Darwin

@available(macOS 13.0, *)
final class NativeHostRuntime {
    let tcpServer: TcpVideoServer?
    let udpSender: UdpVideoSender?
    let encoder: H264LowLatencyEncoder
    let output: ScreenCaptureOutput
    let capturer: ScreenCapturer
    let input: InputReceiver

    init(tcpServer: TcpVideoServer?, udpSender: UdpVideoSender?, encoder: H264LowLatencyEncoder, output: ScreenCaptureOutput, capturer: ScreenCapturer, input: InputReceiver) {
        self.tcpServer = tcpServer
        self.udpSender = udpSender
        self.encoder = encoder
        self.output = output
        self.capturer = capturer
        self.input = input
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

        Task {
            await run()
        }
        RunLoop.main.run()
    }

    @available(macOS 13.0, *)
    private static func run() async {
        let cfg = NativeHostConfig.parse()
        print("P2P Native v2 Mac Host")
        print("client=\(cfg.clientIP):\(cfg.videoPort), input=0.0.0.0:\(cfg.inputPort), video=\(cfg.width)x\(cfg.height)@\(cfg.fps), bitrate=\(cfg.bitrate), transport=\(cfg.transport)")

        do {
            let tcpServer: TcpVideoServer?
            let udpSender: UdpVideoSender?
            if cfg.transport == "udp" {
                udpSender = try UdpVideoSender(clientIP: cfg.clientIP, port: cfg.videoPort)
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
            let output = ScreenCaptureOutput(encoder: encoder) {
                capturerRef.value??.markFirstFrame()
            }
            let capturer = ScreenCapturer(cfg: cfg, output: output)
            capturerRef.value = capturer
            let displayBounds = try await capturer.start()

            let input = try InputReceiver(port: cfg.inputPort, displayBounds: displayBounds) {
                encoder.requestKeyframe(reason: "windows client loss recovery")
            }
            input.start()
            gRuntime = NativeHostRuntime(
                tcpServer: tcpServer,
                udpSender: udpSender,
                encoder: encoder,
                output: output,
                capturer: capturer,
                input: input
            )
            print("[ready] runtime retained encoder/output/capturer/input")
            print("[ready] host streaming. Press Ctrl+C to stop.")
        } catch {
            fputs("[fatal] \(error)\n", stderr)
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
