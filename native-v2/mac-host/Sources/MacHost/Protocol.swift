import Foundation
import Darwin

let p2VideoHeaderBytes = 36
let p2TcpVideoHeaderBytes = 32
let p2InputPacketBytes = 32
let maxUdpPayloadBytes = 1440
let maxVideoFragmentPayload = maxUdpPayloadBytes - p2VideoHeaderBytes
let p2InputRequestKeyframe: UInt8 = 7
let p2InputSetVideoProfile: UInt8 = 8
let p2InputSetVideoBitrate: UInt8 = 9
let p2InputText: UInt8 = 10
let p2InputHeartbeat: UInt8 = 11
let p2InputClientStats: UInt8 = 12
let p2FlagKeyframe: UInt16 = 1 << 0
let p2FlagConfig: UInt16 = 1 << 1
let p2FlagFec: UInt16 = 1 << 2
let p2ModShift: UInt16 = 1 << 0
let p2ModControl: UInt16 = 1 << 1
let p2ModOption: UInt16 = 1 << 2
let p2ModCommand: UInt16 = 1 << 3

final class NativeStats {
    static let shared = NativeStats()
    private let lock = NSLock()
    private var encodedFrames: UInt64 = 0
    private var sentPackets: UInt64 = 0
    private var sentBytes: UInt64 = 0
    private var sendErrors: UInt64 = 0
    private var lastFrameId: UInt64 = 0
    private var started = nowUs()
    private var lastLog = nowUs()
    private var lastFrameAgeUs: UInt64 = 0
    private var maxFrameAgeUs: UInt64 = 0

    func recordFrame(frameId: UInt64, packets: UInt64, bytes: UInt64, errors: UInt64, ptsUs: UInt64) {
        lock.lock()
        encodedFrames += 1
        sentPackets += packets
        sentBytes += bytes
        sendErrors += errors
        lastFrameId = frameId
        let ageUs = nowUs() > ptsUs ? nowUs() - ptsUs : 0
        lastFrameAgeUs = ageUs
        maxFrameAgeUs = max(maxFrameAgeUs, ageUs)
        let now = nowUs()
        if now - lastLog >= 1_000_000 {
            let elapsed = Double(now - started) / 1_000_000.0
            let fps = Double(encodedFrames) / max(0.001, elapsed)
            let mbps = Double(sentBytes) * 8.0 / max(0.001, elapsed) / 1_000_000.0
            logLine(String(format: "[video] encoded=%.1f fps sent=%.1f Mbps packets=%llu errors=%llu lastFrame=%llu age=%.1fms maxAge=%.1fms",
                           fps, mbps, sentPackets, sendErrors, lastFrameId, Double(lastFrameAgeUs) / 1000.0, Double(maxFrameAgeUs) / 1000.0))
            lastLog = now
        }
        lock.unlock()
    }

    func recordTransport(frameId: UInt64, packets: UInt64, bytes: UInt64, errors: UInt64, ptsUs: UInt64) {
        lock.lock()
        sentPackets += packets
        sentBytes += bytes
        sendErrors += errors
        lastFrameId = frameId
        let ageUs = nowUs() > ptsUs ? nowUs() - ptsUs : 0
        lastFrameAgeUs = ageUs
        maxFrameAgeUs = max(maxFrameAgeUs, ageUs)
        let now = nowUs()
        if now - lastLog >= 1_000_000 {
            let elapsed = Double(now - started) / 1_000_000.0
            let fps = Double(encodedFrames) / max(0.001, elapsed)
            let mbps = Double(sentBytes) * 8.0 / max(0.001, elapsed) / 1_000_000.0
            logLine(String(format: "[video] encoded=%.1f fps sent=%.1f Mbps packets=%llu errors=%llu lastFrame=%llu age=%.1fms maxAge=%.1fms",
                           fps, mbps, sentPackets, sendErrors, lastFrameId, Double(lastFrameAgeUs) / 1000.0, Double(maxFrameAgeUs) / 1000.0))
            lastLog = now
        }
        lock.unlock()
    }
}

struct NativeHostConfig {
    var clientIP: String = "127.0.0.1"
    var videoPort: UInt16 = 45000
    var inputPort: UInt16 = 45001
    var width: Int = 1920
    var height: Int = 1080
    var fps: Int = 60
    var bitrate: Int = 30_000_000
    var keyframeSeconds: Int = 1
    var transport: String = "udp"

    static func parse() -> NativeHostConfig {
        var cfg = NativeHostConfig()
        var args = Array(CommandLine.arguments.dropFirst())
        func popValue(_ flag: String) -> String? {
            guard let i = args.firstIndex(of: flag), i + 1 < args.count else { return nil }
            let value = args[i + 1]
            args.remove(at: i + 1)
            args.remove(at: i)
            return value
        }
        if let v = popValue("--client-ip") { cfg.clientIP = v }
        if let v = popValue("--video-port"), let n = UInt16(v) { cfg.videoPort = n }
        if let v = popValue("--input-port"), let n = UInt16(v) { cfg.inputPort = n }
        if let v = popValue("--width"), let n = Int(v) { cfg.width = n }
        if let v = popValue("--height"), let n = Int(v) { cfg.height = n }
        if let v = popValue("--fps"), let n = Int(v) { cfg.fps = n }
        if let v = popValue("--bitrate"), let n = Int(v) { cfg.bitrate = n }
        if let v = popValue("--keyint"), let n = Int(v) { cfg.keyframeSeconds = n }
        if let v = popValue("--transport") { cfg.transport = v.lowercased() == "tcp" ? "tcp" : "udp" }
        return cfg
    }
}

extension Data {
    mutating func appendU8(_ value: UInt8) { append(value) }
    mutating func appendU16LE(_ value: UInt16) {
        var v = value.littleEndian
        Swift.withUnsafeBytes(of: &v) { append(contentsOf: $0) }
    }
    mutating func appendU32LE(_ value: UInt32) {
        var v = value.littleEndian
        Swift.withUnsafeBytes(of: &v) { append(contentsOf: $0) }
    }
    mutating func appendU64LE(_ value: UInt64) {
        var v = value.littleEndian
        Swift.withUnsafeBytes(of: &v) { append(contentsOf: $0) }
    }
}

func readU16LE(_ bytes: [UInt8], _ offset: Int) -> UInt16 {
    UInt16(bytes[offset]) | (UInt16(bytes[offset + 1]) << 8)
}

func readU32LE(_ bytes: [UInt8], _ offset: Int) -> UInt32 {
    UInt32(bytes[offset]) | (UInt32(bytes[offset + 1]) << 8) | (UInt32(bytes[offset + 2]) << 16) | (UInt32(bytes[offset + 3]) << 24)
}

func readU64LE(_ bytes: [UInt8], _ offset: Int) -> UInt64 {
    var value: UInt64 = 0
    for index in 0..<8 {
        value |= UInt64(bytes[offset + index]) << (index * 8)
    }
    return value
}

func readI32LE(_ bytes: [UInt8], _ offset: Int) -> Int32 {
    Int32(bitPattern: readU32LE(bytes, offset))
}

func readFloatLE(_ bytes: [UInt8], _ offset: Int) -> Float {
    Float(bitPattern: readU32LE(bytes, offset))
}

func nowUs() -> UInt64 {
    UInt64(DispatchTime.now().uptimeNanoseconds / 1_000)
}

func logLine(_ message: String) {
    print(message)
    fflush(stdout)
}

final class UdpVideoSender {
    private let fd: Int32
    private var addr = sockaddr_in()
    private var frameId: UInt64 = 1
    private let frameLock = NSLock()
    private let paceLock = NSLock()
    private let pendingCondition = NSCondition()
    private let sendQueue = DispatchQueue(label: "p2p.native.udp-video.send", qos: .userInteractive)
    private let fps: Int
    private var targetBitrateBps: Int
    private var pendingFrame: QueuedFrame?
    private var sendLoopActive = false
    private var nextPacketDueUs: UInt64 = 0

    private var nextFrameIdToQueue: UInt64 = 1

    private struct QueuedFrame {
        let id: UInt64
        let frame: Data
        let keyframe: Bool
        let configIncluded: Bool
        let ptsUs: UInt64
    }

    init(clientIP: String, port: UInt16, fps: Int, bitrate: Int) throws {
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { throw POSIXError(.ENOTSOCK) }
        self.fps = max(30, fps)
        self.targetBitrateBps = max(2_000_000, bitrate)

        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        // Sunshine found that large enough UDP send buffers plus bounded batches
        // avoid NIC/router TX stalls better than trying to sleep before every
        // tiny packet. Keep a moderate cushion here so packet bursts do not
        // overflow the kernel queue while still bounding stale video.
        var sndbuf: Int32 = 1024 * 1024
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, socklen_t(MemoryLayout<Int32>.size))
        var tos: Int32 = 0x10 // IPTOS_LOWDELAY
        var sndLowat: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_SNDLOWAT, &sndLowat, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, socklen_t(MemoryLayout<Int32>.size))

        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = in_port_t(port).bigEndian
        let ok = clientIP.withCString { inet_pton(AF_INET, $0, &addr.sin_addr) }
        guard ok == 1 else { throw POSIXError(.EDESTADDRREQ) }
        logLine("[udp] sending video to \(clientIP):\(port)")
    }

    deinit { close(fd) }

    func updateTargetBitrate(_ bitrate: Int) {
        paceLock.lock()
        targetBitrateBps = max(2_000_000, min(80_000_000, bitrate))
        paceLock.unlock()
    }

    func hasQueuedFrameForSend() -> Bool {
        pendingCondition.lock()
        let queued = pendingFrame != nil
        pendingCondition.unlock()
        return queued
    }

    func sendFrame(_ frame: Data, keyframe: Bool, configIncluded: Bool, ptsUs: UInt64) {
        if frame.isEmpty { return }
        frameLock.lock()
        let id = frameId
        frameId &+= 1
        frameLock.unlock()

        let item = QueuedFrame(id: id, frame: frame, keyframe: keyframe, configIncluded: configIncluded, ptsUs: ptsUs)
        var shouldStart = false
        pendingCondition.lock()
        // Encoded H.264 P-frames may reference prior encoded frames. Dropping or
        // reordering them here can corrupt the decoder until the next IDR, so
        // sender backpressure waits for the single pending slot and preserves the
        // encoder callback order by frame id.
        while pendingFrame != nil || id != nextFrameIdToQueue {
            pendingCondition.wait()
        }

        pendingFrame = item
        nextFrameIdToQueue &+= 1
        pendingCondition.broadcast()
        if !sendLoopActive {
            sendLoopActive = true
            shouldStart = true
        }
        pendingCondition.unlock()

        if shouldStart {
            sendQueue.async { [weak self] in
                self?.sendLoop()
            }
        }
    }

    private func takePendingFrame() -> QueuedFrame? {
        pendingCondition.lock()
        let item = pendingFrame
        pendingFrame = nil
        pendingCondition.broadcast()
        if item == nil {
            sendLoopActive = false
        }
        pendingCondition.unlock()
        return item
    }

    private func currentTargetBitrate() -> Int {
        paceLock.lock()
        let bitrate = targetBitrateBps
        paceLock.unlock()
        return max(2_000_000, bitrate)
    }

    private func sendLoop() {
        while let item = takePendingFrame() {
            sendFrameNow(item)
        }
    }

    private func waitForPacketSlot(packetBytes: Int) {
        let bitrate = currentTargetBitrate()
        let spacingUs = max(50, UInt64(packetBytes * 8 * 1_000_000 / bitrate))
        let now = nowUs()
        if nextPacketDueUs == 0 || nextPacketDueUs + 100_000 < now {
            nextPacketDueUs = now
        }
        if nextPacketDueUs > now {
            let waitUs = nextPacketDueUs - now
            if waitUs >= 2_000 {
                usleep(useconds_t(waitUs - 500))
            }
            while nextPacketDueUs > nowUs() {
                sched_yield()
            }
        }
        nextPacketDueUs = max(nextPacketDueUs &+ spacingUs, nowUs())
    }

    private func sendFrameNow(_ item: QueuedFrame) {
        let frame = item.frame
        let id = item.id
        let keyframe = item.keyframe
        let configIncluded = item.configIncluded
        let ptsUs = item.ptsUs

        let fragCount = UInt16((frame.count + maxVideoFragmentPayload - 1) / maxVideoFragmentPayload)
        let useFec = fragCount > 1
        let totalFragCount = fragCount + (useFec ? 1 : 0)
        var parity = Data(repeating: 0, count: maxVideoFragmentPayload)
        var parityLen = 0
        var sentPackets: UInt64 = 0
        var sentBytes: UInt64 = 0
        var sendErrors: UInt64 = 0
        var offset = 0
        var fragIndex: UInt16 = 0

        frame.withUnsafeBytes { frameRaw in
            guard frameRaw.baseAddress != nil else { return }

            func sendPacket(fragIndex: UInt16, totalFragCount: UInt16, payload: UnsafeRawBufferPointer, flags: UInt16) {
                var header = Data(capacity: p2VideoHeaderBytes)
                header.append(contentsOf: [0x50, 0x32, 0x56, 0x32]) // P2V2
                header.appendU8(1)
                header.appendU8(1)
                header.appendU16LE(UInt16(p2VideoHeaderBytes))
                header.appendU64LE(id)
                header.appendU64LE(ptsUs)
                header.appendU32LE(UInt32(frame.count))
                header.appendU16LE(fragIndex)
                header.appendU16LE(totalFragCount)
                header.appendU16LE(UInt16(payload.count))
                header.appendU16LE(flags)
                waitForPacketSlot(packetBytes: p2VideoHeaderBytes + payload.count)
                header.withUnsafeBytes { headerRaw in
                    guard let headerBase = headerRaw.baseAddress, let payloadBase = payload.baseAddress else { return }
                    var iov = [
                        iovec(iov_base: UnsafeMutableRawPointer(mutating: headerBase), iov_len: headerRaw.count),
                        iovec(iov_base: UnsafeMutableRawPointer(mutating: payloadBase), iov_len: payload.count),
                    ]
                    iov.withUnsafeMutableBufferPointer { iovPtr in
                        var message = msghdr()
                        withUnsafePointer(to: &addr) { ptr in
                            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                                message.msg_name = UnsafeMutableRawPointer(mutating: sa)
                                message.msg_namelen = socklen_t(MemoryLayout<sockaddr_in>.size)
                                message.msg_iov = iovPtr.baseAddress
                                message.msg_iovlen = numericCast(iovPtr.count)
                                let rc = sendmsg(fd, &message, 0)
                                if rc >= 0 {
                                    sentPackets += 1
                                    sentBytes += UInt64(rc)
                                } else {
                                    sendErrors += 1
                                }
                            }
                        }
                    }
                }
            }

            while offset < frame.count {
                let n = min(maxVideoFragmentPayload, frame.count - offset)
                if useFec {
                    parityLen = max(parityLen, n)
                    guard let src = frameRaw.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
                    parity.withUnsafeMutableBytes { parityRaw in
                        guard let dst = parityRaw.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
                        for i in 0..<n { dst[i] ^= src[offset + i] }
                    }
                }
                var flags: UInt16 = 0
                if keyframe { flags |= p2FlagKeyframe }
                if configIncluded { flags |= p2FlagConfig }
                sendPacket(fragIndex: fragIndex, totalFragCount: totalFragCount, payload: UnsafeRawBufferPointer(rebasing: frameRaw[offset..<(offset + n)]), flags: flags)
                offset += n
                fragIndex &+= 1
            }

            if useFec && parityLen > 0 {
                var flags: UInt16 = p2FlagFec
                if keyframe { flags |= p2FlagKeyframe }
                if configIncluded { flags |= p2FlagConfig }
                parity.withUnsafeBytes { parityRaw in
                    sendPacket(fragIndex: fragIndex, totalFragCount: totalFragCount, payload: UnsafeRawBufferPointer(rebasing: parityRaw[0..<parityLen]), flags: flags)
                }
            }
        }

        NativeStats.shared.recordFrame(frameId: id, packets: sentPackets, bytes: sentBytes, errors: sendErrors, ptsUs: ptsUs)
    }
}

final class TcpVideoServer {
    private let fd: Int32
    private let queue = DispatchQueue(label: "p2p.native.tcp-video", qos: .userInteractive)
    private let lock = NSLock()
    private let sendLock = NSLock()
    private var clients = [Int32]()
    private var frameId: UInt64 = 1
    private var running = true

    init(port: UInt16) throws {
        fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
        guard fd >= 0 else { throw POSIXError(.ENOTSOCK) }

        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = in_port_t(port).bigEndian
        addr.sin_addr = in_addr(s_addr: INADDR_ANY.bigEndian)
        let bindResult = withUnsafePointer(to: &addr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                Darwin.bind(fd, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bindResult == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        guard listen(fd, 4) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        logLine("[tcp] listening on 0.0.0.0:\(port)")
    }

    deinit {
        running = false
        close(fd)
        lock.lock()
        let old = clients
        clients.removeAll()
        lock.unlock()
        for c in old { close(c) }
    }

    func start() {
        queue.async { [weak self] in self?.acceptLoop() }
    }

    private func acceptLoop() {
        while running {
            var addr = sockaddr_in()
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            let client = withUnsafeMutablePointer(to: &addr) { ptr in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    accept(fd, sa, &len)
                }
            }
            if client < 0 { continue }
            var one: Int32 = 1
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
            setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
            var sndbuf: Int32 = 4 * 1024 * 1024
            setsockopt(client, SOL_SOCKET, SO_SNDBUF, &sndbuf, socklen_t(MemoryLayout<Int32>.size))
            lock.lock()
            clients.append(client)
            lock.unlock()
            logLine("[tcp] Windows video client connected")
        }
    }

    private func writeAll(_ fd: Int32, _ data: Data) -> Bool {
        var offset = 0
        return data.withUnsafeBytes { raw in
            guard let base = raw.baseAddress else { return false }
            while offset < raw.count {
                let rc = Darwin.send(fd, base.advanced(by: offset), raw.count - offset, 0)
                if rc <= 0 { return false }
                offset += rc
            }
            return true
        }
    }

    func sendFrame(_ frame: Data, keyframe: Bool, configIncluded: Bool, ptsUs: UInt64) {
        sendLock.lock()
        defer { sendLock.unlock() }
        if frame.isEmpty { return }
        let id = frameId
        frameId &+= 1

        var packet = Data(capacity: p2TcpVideoHeaderBytes + frame.count)
        packet.append(contentsOf: [0x50, 0x32, 0x54, 0x32]) // P2T2
        packet.appendU8(1)
        packet.appendU8(1)
        packet.appendU16LE(UInt16(p2TcpVideoHeaderBytes))
        packet.appendU64LE(id)
        packet.appendU64LE(ptsUs)
        packet.appendU32LE(UInt32(frame.count))
        var flags: UInt16 = 0
        if keyframe { flags |= 1 }
        if configIncluded { flags |= 2 }
        packet.appendU16LE(flags)
        packet.appendU16LE(0)
        packet.append(frame)

        lock.lock()
        var next = [Int32]()
        var sentClients: UInt64 = 0
        for c in clients {
            if writeAll(c, packet) {
                next.append(c)
                sentClients += 1
            } else {
                close(c)
                logLine("[tcp] Windows video client disconnected")
            }
        }
        clients = next
        lock.unlock()
        NativeStats.shared.recordFrame(frameId: id, packets: sentClients, bytes: UInt64(packet.count) * sentClients, errors: 0, ptsUs: ptsUs)
    }
}
