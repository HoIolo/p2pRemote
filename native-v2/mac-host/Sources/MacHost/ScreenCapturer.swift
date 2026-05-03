import Foundation
import ScreenCaptureKit
import CoreMedia
import CoreVideo
import CoreGraphics
import AppKit

@available(macOS 13.0, *)
final class ScreenCaptureOutput: NSObject, SCStreamOutput {
    private let encoder: H264LowLatencyEncoder
    private var frames = 0
    private let started = nowUs()
    private var lastKeyframe = nowUs()
    private let firstFrameCallback: () -> Void
    private var reportedFirstFrame = false
    private var reportedMissingImageBuffer = false

    init(encoder: H264LowLatencyEncoder, firstFrameCallback: @escaping () -> Void = {}) {
        self.encoder = encoder
        self.firstFrameCallback = firstFrameCallback
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen else { return }
        guard sampleBuffer.isValid else { return }
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
            if !reportedMissingImageBuffer {
                reportedMissingImageBuffer = true
                logLine("[capture] sample buffer arrived without image buffer")
            }
            return
        }
        if !reportedFirstFrame {
            reportedFirstFrame = true
            let width = CVPixelBufferGetWidth(pixelBuffer)
            let height = CVPixelBufferGetHeight(pixelBuffer)
            logLine("[capture] first frame \(width)x\(height)")
            firstFrameCallback()
        }
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let now = nowUs()
        let forceKeyframe = frames == 0 || now - lastKeyframe >= 1_000_000
        if forceKeyframe { lastKeyframe = now }
        encoder.encode(pixelBuffer, pts: pts, forceKeyframe: forceKeyframe)
        frames += 1
        if frames % 300 == 0 {
            let elapsed = Double(nowUs() - started) / 1_000_000.0
            let fps = Double(frames) / max(0.001, elapsed)
            logLine(String(format: "[capture] %.1f fps", fps))
        }
    }
}

@available(macOS 13.0, *)
final class ScreenCapturer: NSObject, SCStreamDelegate {
    private let cfg: NativeHostConfig
    private let output: ScreenCaptureOutput
    private let sampleQueue = DispatchQueue(label: "p2p.native.capture", qos: .userInteractive)
    private var stream: SCStream?
    private var firstFrameWatchdog: DispatchWorkItem?
    private var selectedDisplayId: CGDirectDisplayID = 0
    private var selectedBounds: CGRect = .zero
    private var sawFirstFrame = false

    init(cfg: NativeHostConfig, output: ScreenCaptureOutput) {
        self.cfg = cfg
        self.output = output
    }

    func start() async throws -> CGRect {
        try await ensureScreenCapturePermission()
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
        let available = content.displays
        guard !available.isEmpty else {
            throw NSError(domain: "P2PNative", code: 1, userInfo: [NSLocalizedDescriptionKey: "No capturable display found"])
        }
        let mainId = CGMainDisplayID()
        for item in available {
            let bounds = CGDisplayBounds(CGDirectDisplayID(item.displayID))
            logLine("[capture] candidate display=\(item.displayID) \(Int(bounds.width))x\(Int(bounds.height))")
        }
        let display = available.first(where: { CGDirectDisplayID($0.displayID) == mainId })
          ?? available.max(by: {
              let lhs = CGDisplayBounds(CGDirectDisplayID($0.displayID))
              let rhs = CGDisplayBounds(CGDirectDisplayID($1.displayID))
              return lhs.width * lhs.height < rhs.width * rhs.height
          })!

        let filter = SCContentFilter(display: display, excludingApplications: [], exceptingWindows: [])
        let streamCfg = SCStreamConfiguration()
        streamCfg.width = cfg.width
        streamCfg.height = cfg.height
        streamCfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(cfg.fps))
        streamCfg.queueDepth = 1
        streamCfg.showsCursor = false
        streamCfg.capturesAudio = false
        streamCfg.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        streamCfg.colorSpaceName = CGColorSpace.sRGB

        let newStream = SCStream(filter: filter, configuration: streamCfg, delegate: self)
        try newStream.addStreamOutput(output, type: .screen, sampleHandlerQueue: sampleQueue)
        try await newStream.startCapture()
        stream = newStream

        let bounds = CGDisplayBounds(CGDirectDisplayID(display.displayID))
        logLine("[capture] selected display=\(display.displayID) main=\(mainId) bounds=\(Int(bounds.width))x\(Int(bounds.height)) stream=\(cfg.width)x\(cfg.height)@\(cfg.fps)")
        selectedDisplayId = CGDirectDisplayID(display.displayID)
        selectedBounds = bounds
        armFirstFrameWatchdog()
        return bounds
    }

    private func armFirstFrameWatchdog() {
        firstFrameWatchdog?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            if self.sawFirstFrame { return }
            let permission = CGPreflightScreenCaptureAccess() ? "granted" : "missing"
            logLine("[capture] warning: no first frame within 3000 ms; permission=\(permission) selected=\(self.selectedDisplayId) bounds=\(Int(self.selectedBounds.width))x\(Int(self.selectedBounds.height)) stream=\(self.cfg.width)x\(self.cfg.height)@\(self.cfg.fps)")
        }
        firstFrameWatchdog = work
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 3.0, execute: work)
    }

    private func ensureScreenCapturePermission() async throws {
        let ready = await MainActor.run {
            CGPreflightScreenCaptureAccess()
        }
        if ready {
            logLine("[capture] screen recording permission ready for native host")
            return
        }

        logLine("[capture] screen recording permission missing for native host; requesting access")
        let granted = await MainActor.run {
            NSApp.activate(ignoringOtherApps: true)
            return CGRequestScreenCaptureAccess()
        }
        guard granted else {
            throw NSError(
                domain: "P2PNative",
                code: 2,
                userInfo: [
                    NSLocalizedDescriptionKey: "Screen Recording permission denied for p2p-native-mac-host. Enable it in System Settings -> Privacy & Security -> Screen Recording, then fully restart the Mac app."
                ]
            )
        }

        logLine("[capture] screen recording permission granted for native host; if macOS still returns no frames, fully restart the Mac app once and reconnect")
        try? await Task.sleep(nanoseconds: 300_000_000)
    }

    func markFirstFrame() {
        if sawFirstFrame { return }
        sawFirstFrame = true
        firstFrameWatchdog?.cancel()
        firstFrameWatchdog = nil
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        logLine("[capture] stream stopped with error: \(error.localizedDescription)")
    }
}
