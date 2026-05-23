import Foundation
import VideoToolbox
import CoreMedia
import CoreVideo
import Darwin

final class H264LowLatencyEncoder {
    private var width: Int32
    private var height: Int32
    private var fps: Int
    private var bitrate: Int
    private var keyframeSeconds: Int
    private let onFrame: (Data, Bool, Bool, UInt64) -> Void
    private var session: VTCompressionSession?
    private var sps = Data()
    private var pps = Data()
    private var frameIndex: Int64 = 0
    private let controlLock = NSLock()
    private var pendingForcedKeyframe = false
    private var reportedFirstEncodedFrame = false
    private var currentBitrate: Int
    private let inflightLock = NSLock()
    private var inflightFrames = 0
    private let maxInflightFrames = 1
    private var droppedForBackpressure: UInt64 = 0
    private var lastBackpressureLog = nowUs()

    init(width: Int, height: Int, fps: Int, bitrate: Int, keyframeSeconds: Int, onFrame: @escaping (Data, Bool, Bool, UInt64) -> Void) throws {
        self.width = Int32(width)
        self.height = Int32(height)
        self.fps = fps
        self.bitrate = bitrate
        self.currentBitrate = bitrate
        self.keyframeSeconds = keyframeSeconds
        self.onFrame = onFrame
        try createSession()
    }

    deinit {
        if let session { VTCompressionSessionInvalidate(session) }
    }

    private func createSession() throws {
        let refcon = UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())
        var newSession: VTCompressionSession?
        let encoderSpec = [
            kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder as String: true
        ] as CFDictionary
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: width,
            height: height,
            codecType: kCMVideoCodecType_H264,
            encoderSpecification: encoderSpec,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: compressionCallback,
            refcon: refcon,
            compressionSessionOut: &newSession
        )
        guard status == noErr, let created = newSession else { throw NSError(domain: NSOSStatusErrorDomain, code: Int(status)) }
        session = created

        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel)
        set(kVTCompressionPropertyKey_AverageBitRate, bitrate as CFTypeRef)
        setDataRateLimits(bitrate: bitrate)
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, (fps * keyframeSeconds) as CFTypeRef)
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, keyframeSeconds as CFTypeRef)
        if #available(macOS 11.0, *) {
            set(kVTCompressionPropertyKey_MaximizePowerEfficiency, kCFBooleanFalse)
        }
        if #available(macOS 10.13, *) {
            set(kVTCompressionPropertyKey_ExpectedFrameRate, fps as CFTypeRef)
        }
        VTCompressionSessionPrepareToEncodeFrames(created)
    }

    private func set(_ key: CFString, _ value: CFTypeRef) {
        guard let session else { return }
        let status = VTSessionSetProperty(session, key: key, value: value)
        if status != noErr {
            fputs("[encoder] property \(key) failed: \(status)\n", stderr)
            fflush(stderr)
        }
    }

    private func setDataRateLimits(bitrate: Int) {
        guard bitrate > 0 else { return }
        let bytesPerSecond = max(1, bitrate * 2 / 8)
        let limits: [Any] = [bytesPerSecond as NSNumber, 1.0 as NSNumber]
        set(kVTCompressionPropertyKey_DataRateLimits, limits as CFArray)
    }

    func requestKeyframe(reason: String = "client recovery") {
        controlLock.lock()
        pendingForcedKeyframe = true
        controlLock.unlock()
        logLine("[encoder] keyframe requested: \(reason)")
    }

    func updateBitrate(_ newBitrate: Int, reason: String = "adaptive") {
        let clamped = max(2_000_000, min(80_000_000, newBitrate))
        controlLock.lock()
        if clamped == currentBitrate {
            controlLock.unlock()
            return
        }
        currentBitrate = clamped
        bitrate = clamped
        controlLock.unlock()
        set(kVTCompressionPropertyKey_AverageBitRate, clamped as CFTypeRef)
        setDataRateLimits(bitrate: clamped)
        logLine("[encoder] bitrate updated: \(clamped)")
    }

    func canAcceptFrame() -> Bool {
        inflightLock.lock()
        let available = inflightFrames < maxInflightFrames
        inflightLock.unlock()
        return available
    }

    private func beginEncodeSlot() -> Bool {
        inflightLock.lock()
        defer { inflightLock.unlock() }
        guard inflightFrames < maxInflightFrames else { return false }
        inflightFrames += 1
        return true
    }

    fileprivate func finishEncodeSlot() {
        inflightLock.lock()
        if inflightFrames > 0 {
            inflightFrames -= 1
        }
        inflightLock.unlock()
    }

    private func recordBackpressureDrop() {
        droppedForBackpressure &+= 1
        let now = nowUs()
        if now - lastBackpressureLog >= 1_000_000 {
            logLine("[encoder] skipped stale frames while encoder busy: \(droppedForBackpressure)")
            droppedForBackpressure = 0
            lastBackpressureLog = now
        }
    }

    func reconfigure(width newWidth: Int, height newHeight: Int, fps newFps: Int, bitrate newBitrate: Int, keyframeSeconds newKeyframeSeconds: Int, reason: String = "profile changed") throws {
        let clampedWidth = Int32(max(640, newWidth))
        let clampedHeight = Int32(max(360, newHeight))
        let clampedFps = max(30, min(240, newFps))
        let clampedBitrate = max(2_000_000, min(80_000_000, newBitrate))
        let clampedKeyframeSeconds = max(1, newKeyframeSeconds)

        controlLock.lock()
        if let oldSession = session {
            VTCompressionSessionInvalidate(oldSession)
        }
        session = nil
        width = clampedWidth
        height = clampedHeight
        fps = clampedFps
        bitrate = clampedBitrate
        currentBitrate = clampedBitrate
        keyframeSeconds = clampedKeyframeSeconds
        sps.removeAll(keepingCapacity: true)
        pps.removeAll(keepingCapacity: true)
        frameIndex = 0
        pendingForcedKeyframe = true
        reportedFirstEncodedFrame = false
        controlLock.unlock()

        inflightLock.lock()
        inflightFrames = 0
        inflightLock.unlock()

        try createSession()
        logLine("[encoder] reconfigured: \(Int(clampedWidth))x\(Int(clampedHeight))@\(clampedFps) bitrate=\(clampedBitrate) reason=\(reason)")
    }

    func encode(_ pixelBuffer: CVPixelBuffer, pts: CMTime, forceKeyframe: Bool = false) -> Bool {
        guard let session else { return false }
        guard beginEncodeSlot() else {
            recordBackpressureDrop()
            return false
        }
        var flags = VTEncodeInfoFlags()
        let index = frameIndex
        frameIndex += 1
        controlLock.lock()
        let forcedByControl = pendingForcedKeyframe
        pendingForcedKeyframe = false
        controlLock.unlock()
        var props: CFDictionary?
        if forceKeyframe || forcedByControl || index == 0 {
            props = [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary
        }
        let status = VTCompressionSessionEncodeFrame(
            session,
            imageBuffer: pixelBuffer,
            presentationTimeStamp: pts,
            duration: CMTime(value: 1, timescale: CMTimeScale(fps)),
            frameProperties: props,
            sourceFrameRefcon: nil,
            infoFlagsOut: &flags
        )
        if status != noErr {
            finishEncodeSlot()
            fputs("[encoder] encode failed: \(status)\n", stderr)
            fflush(stderr)
            return false
        }
        if flags.contains(.frameDropped) {
            finishEncodeSlot()
            recordBackpressureDrop()
            return false
        }
        return true
    }

    fileprivate func handle(sampleBuffer: CMSampleBuffer) {
        guard CMSampleBufferDataIsReady(sampleBuffer) else { return }
        let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[CFString: Any]]
        let notSync = attachments?.first?[kCMSampleAttachmentKey_NotSync] as? Bool ?? false
        let keyframe = !notSync
        var configIncluded = false

        if keyframe, let format = CMSampleBufferGetFormatDescription(sampleBuffer) {
            updateParameterSets(format)
        }

        guard let block = CMSampleBufferGetDataBuffer(sampleBuffer) else { return }
        let totalLength = CMBlockBufferGetDataLength(block)
        guard totalLength > 0 else { return }
        var elementaryStream = Data(count: totalLength)
        let copyStatus = elementaryStream.withUnsafeMutableBytes { rawBuffer in
            guard let baseAddress = rawBuffer.baseAddress else { return kCMBlockBufferBadCustomBlockSourceErr }
            return CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: totalLength, destination: baseAddress)
        }
        guard copyStatus == kCMBlockBufferNoErr else {
            fputs("[encoder] copy bitstream failed: \(copyStatus)\n", stderr)
            fflush(stderr)
            return
        }

        var out = Data(capacity: totalLength + 128)
        if keyframe, !sps.isEmpty, !pps.isEmpty {
            out.append(contentsOf: [0, 0, 0, 1]); out.append(sps)
            out.append(contentsOf: [0, 0, 0, 1]); out.append(pps)
            configIncluded = true
        }

        var offset = 0
        elementaryStream.withUnsafeBytes { rawBuffer in
            guard let baseAddress = rawBuffer.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
            while offset + 4 <= totalLength {
                let b0 = UInt32(baseAddress[offset])
                let b1 = UInt32(baseAddress[offset + 1])
                let b2 = UInt32(baseAddress[offset + 2])
                let b3 = UInt32(baseAddress[offset + 3])
                let nalLength = Int((b0 << 24) | (b1 << 16) | (b2 << 8) | b3)
                offset += 4
                if nalLength <= 0 || offset + nalLength > totalLength { break }
                out.append(contentsOf: [0, 0, 0, 1])
                out.append(baseAddress + offset, count: nalLength)
                offset += nalLength
            }
        }
        if !reportedFirstEncodedFrame {
            reportedFirstEncodedFrame = true
            logLine("[encoder] first h264 frame bytes=\(out.count) keyframe=\(keyframe) config=\(configIncluded)")
        }

        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let ptsMicros: UInt64
        if pts.isValid && pts.timescale != 0 {
            ptsMicros = UInt64(max(0, (Double(pts.value) / Double(pts.timescale)) * 1_000_000.0))
        } else {
            ptsMicros = nowUs()
        }
        onFrame(out, keyframe, configIncluded, ptsMicros)
    }

    private func updateParameterSets(_ format: CMFormatDescription) {
        var spsPtr: UnsafePointer<UInt8>?
        var spsSize = 0
        var spsCount = 0
        var nalHeaderLength: Int32 = 0
        var status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: 0, parameterSetPointerOut: &spsPtr, parameterSetSizeOut: &spsSize, parameterSetCountOut: &spsCount, nalUnitHeaderLengthOut: &nalHeaderLength)
        if status == noErr, let spsPtr { sps = Data(bytes: spsPtr, count: spsSize) }

        var ppsPtr: UnsafePointer<UInt8>?
        var ppsSize = 0
        status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: 1, parameterSetPointerOut: &ppsPtr, parameterSetSizeOut: &ppsSize, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
        if status == noErr, let ppsPtr { pps = Data(bytes: ppsPtr, count: ppsSize) }
    }
}

private let compressionCallback: VTCompressionOutputCallback = { refcon, _, status, _, sampleBuffer in
    guard let refcon else { return }
    let encoder = Unmanaged<H264LowLatencyEncoder>.fromOpaque(refcon).takeUnretainedValue()
    defer { encoder.finishEncodeSlot() }
    guard status == noErr, let sampleBuffer else { return }
    encoder.handle(sampleBuffer: sampleBuffer)
}
