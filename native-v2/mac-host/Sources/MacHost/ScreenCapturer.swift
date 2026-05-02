import Foundation
import ScreenCaptureKit
import CoreMedia
import CoreVideo
import CoreGraphics

@available(macOS 13.0, *)
final class ScreenCaptureOutput: NSObject, SCStreamOutput {
    private let encoder: H264LowLatencyEncoder
    private var frames = 0
    private let started = nowUs()

    init(encoder: H264LowLatencyEncoder) {
        self.encoder = encoder
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen else { return }
        guard sampleBuffer.isValid, let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        encoder.encode(pixelBuffer, pts: pts)
        frames += 1
        if frames % 300 == 0 {
            let elapsed = Double(nowUs() - started) / 1_000_000.0
            let fps = Double(frames) / max(0.001, elapsed)
            print(String(format: "[capture] %.1f fps", fps))
        }
    }
}

@available(macOS 13.0, *)
final class ScreenCapturer {
    private let cfg: NativeHostConfig
    private let output: ScreenCaptureOutput
    private var stream: SCStream?

    init(cfg: NativeHostConfig, output: ScreenCaptureOutput) {
        self.cfg = cfg
        self.output = output
    }

    func start() async throws -> CGRect {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
        guard let display = content.displays.first else {
            throw NSError(domain: "P2PNative", code: 1, userInfo: [NSLocalizedDescriptionKey: "No capturable display found"])
        }

        let filter = SCContentFilter(display: display, excludingApplications: [], exceptingWindows: [])
        let streamCfg = SCStreamConfiguration()
        streamCfg.width = cfg.width
        streamCfg.height = cfg.height
        streamCfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(cfg.fps))
        streamCfg.queueDepth = 2
        streamCfg.showsCursor = true
        streamCfg.capturesAudio = false
        streamCfg.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        streamCfg.colorSpaceName = CGColorSpace.sRGB

        let newStream = SCStream(filter: filter, configuration: streamCfg, delegate: nil)
        try newStream.addStreamOutput(output, type: .screen, sampleHandlerQueue: DispatchQueue(label: "p2p.native.capture", qos: .userInteractive))
        try await newStream.startCapture()
        stream = newStream

        let bounds = CGDisplayBounds(CGDirectDisplayID(display.displayID))
        print("[capture] display=\(display.displayID) bounds=\(Int(bounds.width))x\(Int(bounds.height)) stream=\(cfg.width)x\(cfg.height)@\(cfg.fps)")
        return bounds
    }
}
