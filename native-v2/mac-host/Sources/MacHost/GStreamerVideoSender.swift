import Foundation
import Darwin

final class GStreamerVideoSender {
    private var cfg: NativeHostConfig
    private var process: Process?

    init(cfg: NativeHostConfig) {
        self.cfg = cfg
    }

    deinit {
        stop()
    }

    func start() throws {
        let gstLaunch = resolveGstLaunch()
        let args = makeArgs()
        logLine("[gst] launching \(gstLaunch) \(args.joined(separator: " "))")

        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: gstLaunch)
        proc.arguments = args
        proc.environment = makeEnvironment()
        let stderrPipe = Pipe()
        let stdoutPipe = Pipe()
        proc.standardError = stderrPipe
        proc.standardOutput = stdoutPipe
        stderrPipe.fileHandleForReading.readabilityHandler = { handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
                logLine("[gst] \(line)")
            }
        }
        stdoutPipe.fileHandleForReading.readabilityHandler = { handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
                logLine("[gst] \(line)")
            }
        }
        proc.terminationHandler = { p in
            logLine("[gst] sender exited status=\(p.terminationStatus)")
            stderrPipe.fileHandleForReading.readabilityHandler = nil
            stdoutPipe.fileHandleForReading.readabilityHandler = nil
        }
        try proc.run()
        process = proc
    }

    func stop() {
        guard let proc = process else { return }
        if proc.isRunning {
            proc.terminate()
        }
        process = nil
    }

    func requestKeyframe() {
        // gst-launch pipelines cannot receive a force-key-unit event from this
        // helper process. Keep this as an explicit no-op so the control path is
        // visible in logs; regular IDR frames are kept frequent via keyint.
        logLine("[gst] keyframe request noted; gst-launch backend uses fixed keyint")
    }

    func updateBitrate(_ bitrate: Int) throws {
        var next = cfg
        next.bitrate = max(2_000_000, min(200_000_000, bitrate))
        try restart(with: next)
    }

    func reconfigure(width: Int, height: Int, fps: Int, bitrateMbps: Int) throws {
        var next = cfg
        next.width = max(640, width)
        next.height = max(360, height)
        next.fps = max(30, min(240, fps))
        if bitrateMbps > 0 {
            next.bitrate = max(2_000_000, min(200_000_000, bitrateMbps * 1_000_000))
        }
        try restart(with: next)
    }

    private func restart(with next: NativeHostConfig) throws {
        logLine("[gst] restarting video sender \(next.width)x\(next.height)@\(next.fps) bitrate=\(next.bitrate)")
        stop()
        cfg = next
        try start()
    }

    private func makeArgs() -> [String] {
        let bitrateKbps = max(500, cfg.bitrate / 1000)
        let keyintFrames = max(1, cfg.fps * max(1, cfg.keyframeSeconds))
        let caps = "video/x-raw,width=\(cfg.width),height=\(cfg.height),framerate=\(cfg.fps)/1"
        return [
            "-q",
            "avfvideosrc", "capture-screen=true", "capture-screen-cursor=false", "!",
            "queue", "max-size-buffers=1", "max-size-time=0", "max-size-bytes=0", "leaky=downstream", "!",
            "videoconvert", "!",
            "videoscale", "!",
            caps, "!",
            "queue", "max-size-buffers=1", "max-size-time=0", "max-size-bytes=0", "leaky=downstream", "!",
            "vtenc_h264_hw", "realtime=true", "allow-frame-reordering=false", "max-keyframe-interval=\(keyintFrames)", "bitrate=\(bitrateKbps)", "!",
            "h264parse", "config-interval=-1", "!",
            "rtph264pay", "pt=96", "mtu=1200", "config-interval=-1", "aggregate-mode=zero-latency", "!",
            "udpsink", "host=\(cfg.clientIP)", "port=\(cfg.videoPort)", "sync=false", "async=false"
        ]
    }

    private func makeEnvironment() -> [String: String] {
        var env = ProcessInfo.processInfo.environment
        let prefixes = [
            "/opt/homebrew",
            "/usr/local",
            "/Library/Frameworks/GStreamer.framework/Versions/Current",
            "/Library/Frameworks/GStreamer.framework/Commands"
        ]
        var pathParts = [String]()
        for prefix in prefixes {
            pathParts.append("\(prefix)/bin")
        }
        pathParts.append(env["PATH"] ?? "/usr/bin:/bin:/usr/sbin:/sbin")
        env["PATH"] = pathParts.joined(separator: ":")
        return env
    }

    private func resolveGstLaunch() -> String {
        let candidates = [
            ProcessInfo.processInfo.environment["GST_LAUNCH_1_0"],
            "/opt/homebrew/bin/gst-launch-1.0",
            "/usr/local/bin/gst-launch-1.0",
            "/Library/Frameworks/GStreamer.framework/Versions/Current/bin/gst-launch-1.0",
            "/usr/bin/gst-launch-1.0"
        ].compactMap { $0 }
        for candidate in candidates where access(candidate, X_OK) == 0 {
            return candidate
        }
        return "gst-launch-1.0"
    }
}
