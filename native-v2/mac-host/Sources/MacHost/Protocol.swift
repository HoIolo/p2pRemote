import Foundation
import Darwin

let p2VideoHeaderBytes = 36
let p2InputPacketBytes = 32
let maxUdpPayloadBytes = 1200
let maxVideoFragmentPayload = maxUdpPayloadBytes - p2VideoHeaderBytes

struct NativeHostConfig {
    var clientIP: String = "127.0.0.1"
    var videoPort: UInt16 = 45000
    var inputPort: UInt16 = 45001
    var width: Int = 1920
    var height: Int = 1080
    var fps: Int = 60
    var bitrate: Int = 25_000_000
    var keyframeSeconds: Int = 1

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

func readI32LE(_ bytes: [UInt8], _ offset: Int) -> Int32 {
    Int32(bitPattern: readU32LE(bytes, offset))
}

func readFloatLE(_ bytes: [UInt8], _ offset: Int) -> Float {
    Float(bitPattern: readU32LE(bytes, offset))
}

func nowUs() -> UInt64 {
    UInt64(DispatchTime.now().uptimeNanoseconds / 1_000)
}

final class UdpVideoSender {
    private let fd: Int32
    private var addr = sockaddr_in()
    private var frameId: UInt64 = 1

    init(clientIP: String, port: UInt16) throws {
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { throw POSIXError(.ENOTSOCK) }

        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        var sndbuf: Int32 = 4 * 1024 * 1024
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, socklen_t(MemoryLayout<Int32>.size))
        var tos: Int32 = 0x10 // IPTOS_LOWDELAY
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, socklen_t(MemoryLayout<Int32>.size))

        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = in_port_t(port).bigEndian
        let ok = clientIP.withCString { inet_pton(AF_INET, $0, &addr.sin_addr) }
        guard ok == 1 else { throw POSIXError(.EDESTADDRREQ) }
    }

    deinit { close(fd) }

    func sendFrame(_ frame: Data, keyframe: Bool, configIncluded: Bool, ptsUs: UInt64) {
        if frame.isEmpty { return }
        let id = frameId
        frameId &+= 1
        let fragCount = UInt16((frame.count + maxVideoFragmentPayload - 1) / maxVideoFragmentPayload)
        var offset = 0
        var fragIndex: UInt16 = 0
        while offset < frame.count {
            let n = min(maxVideoFragmentPayload, frame.count - offset)
            var packet = Data(capacity: p2VideoHeaderBytes + n)
            packet.append(contentsOf: [0x50, 0x32, 0x56, 0x32]) // P2V2
            packet.appendU8(1)
            packet.appendU8(1)
            packet.appendU16LE(UInt16(p2VideoHeaderBytes))
            packet.appendU64LE(id)
            packet.appendU64LE(ptsUs)
            packet.appendU32LE(UInt32(frame.count))
            packet.appendU16LE(fragIndex)
            packet.appendU16LE(fragCount)
            packet.appendU16LE(UInt16(n))
            var flags: UInt16 = 0
            if keyframe { flags |= 1 }
            if configIncluded { flags |= 2 }
            packet.appendU16LE(flags)
            packet.append(frame.subdata(in: offset..<(offset + n)))

            packet.withUnsafeBytes { raw in
                withUnsafePointer(to: &addr) { ptr in
                    ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                        _ = sendto(fd, raw.baseAddress!, raw.count, 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }
            }
            offset += n
            fragIndex &+= 1
        }
    }
}
