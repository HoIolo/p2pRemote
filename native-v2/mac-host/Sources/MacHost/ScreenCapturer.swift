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
            CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
            var nonZeroBytes = 0
            if let base = CVPixelBufferGetBaseAddress(pixelBuffer) {
                let totalBytes = CVPixelBufferGetDataSize(pixelBuffer)
                let ptr = base.assumingMemoryBound(to: UInt8.self)
                let checkCount = min(totalBytes, 4096)
                for i in 0..<checkCount {
                    if ptr[i] != 0 { nonZeroBytes += 1 }
                }
            }
            CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly)
            logLine("[capture] first frame \(width)x\(height) nonZeroInFirst4K=\(nonZeroBytes)")
            firstFrameCallback()
        }
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let now = nowUs()
        let forceKeyframe = frames == 0 || now - lastKeyframe >= 2_000_000
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
    private let stateQueue = DispatchQueue(label: "p2p.native.capture.state")
    private let preferredPixelFormats: [OSType] = [
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        kCVPixelFormatType_32BGRA,
    ]
    private var stream: SCStream?
    private var firstFrameWatchdog: DispatchWorkItem?
    private var selectedDisplayId: CGDirectDisplayID = 0
    private var selectedBounds: CGRect = .zero
    private var currentPixelFormat: OSType = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
    private var sawFirstFrame = false
    private var firstFrameContinuation: CheckedContinuation<Bool, Never>?

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

        let bounds = CGDisplayBounds(CGDirectDisplayID(display.displayID))
        selectedDisplayId = CGDirectDisplayID(display.displayID)
        selectedBounds = bounds
        try await startStream(display: display, mainId: mainId, pixelFormat: preferredPixelFormats[0])
        let gotFirstFrame = await waitForFirstFrame(timeoutMs: 1_500)
        if preferredPixelFormats.count > 1 && !gotFirstFrame {
            let fallback = preferredPixelFormats[1]
            logLine("[capture] no first frame within 1500 ms using pixelFormat=\(pixelFormatName(currentPixelFormat)); retrying with pixelFormat=\(pixelFormatName(fallback))")
            try await startStream(display: display, mainId: mainId, pixelFormat: fallback)
            _ = await waitForFirstFrame(timeoutMs: 1_500)
        }
        armFirstFrameWatchdog()
        return bounds
    }

    func stop() async {
        firstFrameWatchdog?.cancel()
        firstFrameWatchdog = nil
        resetFirstFrameState()
        if let stream {
            try? await stream.stopCapture()
            self.stream = nil
        }
    }

    private func startStream(display: SCDisplay, mainId: CGDirectDisplayID, pixelFormat: OSType) async throws {
        resetFirstFrameState()
        if let oldStream = stream {
            try? await oldStream.stopCapture()
        }

        let filter = SCContentFilter(display: display, excludingApplications: [], exceptingWindows: [])
        let streamCfg = SCStreamConfiguration()
        streamCfg.width = cfg.width
        streamCfg.height = cfg.height
        streamCfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(cfg.fps))
        streamCfg.queueDepth = 3
        streamCfg.showsCursor = true
        streamCfg.capturesAudio = false
        streamCfg.pixelFormat = pixelFormat
        streamCfg.colorSpaceName = CGColorSpace.sRGB

        let newStream = SCStream(filter: filter, configuration: streamCfg, delegate: self)
        try newStream.addStreamOutput(output, type: .screen, sampleHandlerQueue: sampleQueue)
        try await newStream.startCapture()
        stream = newStream
        currentPixelFormat = pixelFormat
        logLine("[capture] selected display=\(display.displayID) main=\(mainId) bounds=\(Int(selectedBounds.width))x\(Int(selectedBounds.height)) stream=\(cfg.width)x\(cfg.height)@\(cfg.fps) pixelFormat=\(pixelFormatName(pixelFormat))")
    }

    private func armFirstFrameWatchdog() {
        firstFrameWatchdog?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            if self.hasSeenFirstFrame() { return }
            let permission = CGPreflightScreenCaptureAccess() ? "granted" : "missing"
            logLine("[capture] warning: no first frame within 3000 ms; permission=\(permission) selected=\(self.selectedDisplayId) bounds=\(Int(self.selectedBounds.width))x\(Int(self.selectedBounds.height)) stream=\(self.cfg.width)x\(self.cfg.height)@\(self.cfg.fps) pixelFormat=\(self.pixelFormatName(self.currentPixelFormat))")
        }
        firstFrameWatchdog = work
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 3.0, execute: work)
    }

    private func waitForFirstFrame(timeoutMs: UInt64) async -> Bool {
        if hasSeenFirstFrame() {
            return true
        }

        return await withCheckedContinuation { continuation in
            var resolvedImmediately = false
            stateQueue.sync {
                if sawFirstFrame {
                    resolvedImmediately = true
                } else {
                    firstFrameContinuation = continuation
                }
            }

            if resolvedImmediately {
                continuation.resume(returning: true)
                return
            }

            Task {
                try? await Task.sleep(nanoseconds: timeoutMs * 1_000_000)
                let pending = self.stateQueue.sync { () -> CheckedContinuation<Bool, Never>? in
                    let pending = self.firstFrameContinuation
                    self.firstFrameContinuation = nil
                    return pending
                }
                pending?.resume(returning: false)
            }
        }
    }

    private func resetFirstFrameState() {
        firstFrameWatchdog?.cancel()
        firstFrameWatchdog = nil
        let pending = stateQueue.sync { () -> CheckedContinuation<Bool, Never>? in
            sawFirstFrame = false
            let pending = firstFrameContinuation
            firstFrameContinuation = nil
            return pending
        }
        pending?.resume(returning: false)
    }

    private func hasSeenFirstFrame() -> Bool {
        stateQueue.sync { sawFirstFrame }
    }

    private func pixelFormatName(_ pixelFormat: OSType) -> String {
        switch pixelFormat {
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
            return "420v"
        case kCVPixelFormatType_32BGRA:
            return "BGRA"
        default:
            return String(format: "0x%08X", pixelFormat)
        }
    }

    private func ensureScreenCapturePermission() async throws {
        if CGPreflightScreenCaptureAccess() {
            logLine("[capture] screen recording permission ready")
            return
        }

        logLine("[capture] screen recording permission missing; requesting access...")
        _ = CGRequestScreenCaptureAccess()

        // Give macOS a moment to register the permission change
        try await Task.sleep(nanoseconds: 1_000_000_000)

        if CGPreflightScreenCaptureAccess() {
            logLine("[capture] screen recording permission granted")
            return
        }

        // When launched as a child of Electron, permission is inherited from the parent.
        // If still not granted, the parent app needs screen recording permission.
        let parentName = ProcessInfo.processInfo.environment["__CFBundleIdentifier"] ?? "unknown"
        throw NSError(
            domain: "P2PNative",
            code: 2,
            userInfo: [
                NSLocalizedDescriptionKey: "Screen Recording permission not granted. The parent app (\(parentName)) needs screen recording permission in System Settings → Privacy & Security → Screen Recording. Restart the app after granting."
            ]
        )
    }

    func markFirstFrame() {
        let pending = stateQueue.sync { () -> CheckedContinuation<Bool, Never>? in
            if sawFirstFrame {
                return nil
            }
            sawFirstFrame = true
            let pending = firstFrameContinuation
            firstFrameContinuation = nil
            return pending
        }
        firstFrameWatchdog?.cancel()
        firstFrameWatchdog = nil
        pending?.resume(returning: true)
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        logLine("[capture] stream stopped with error: \(error.localizedDescription)")
    }
}
