import Foundation
import CoreGraphics

@main
struct MacHostMain {
    static func main() async {
        guard #available(macOS 13.0, *) else {
            fputs("P2P Native v2 host requires macOS 13+ for ScreenCaptureKit.\n", stderr)
            exit(2)
        }

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
            let output = ScreenCaptureOutput(encoder: encoder)
            let capturer = ScreenCapturer(cfg: cfg, output: output)
            let displayBounds = try await capturer.start()

            let input = try InputReceiver(port: cfg.inputPort, displayBounds: displayBounds)
            input.start()
            print("[ready] host streaming. Press Ctrl+C to stop.")
            dispatchMain()
        } catch {
            fputs("[fatal] \(error)\n", stderr)
            exit(1)
        }
    }
}
