import Foundation
import CoreGraphics
import ApplicationServices
import Darwin

final class InputReceiver {
    private let fd: Int32
    private let displayBounds: CGRect
    private let onKeyframeRequest: () -> Void
    private let onVideoProfileRequest: (Int, Int, Int, Int) -> Void
    private let onBitrateRequest: (Int) -> Void
    private let onClientTimeout: () -> Void
    private let queue = DispatchQueue(label: "p2p.native.input", qos: .userInteractive)
    private let timeoutQueue = DispatchQueue(label: "p2p.native.input.timeout")
    private var running = true
    private var downButtons = Set<Int>()
    private var lastPacketTime: UInt64 = 0
    private var clientConnected = false
    private static let timeoutUs: UInt64 = 10_000_000

    init(
        port: UInt16,
        displayBounds: CGRect,
        onKeyframeRequest: @escaping () -> Void = {},
        onVideoProfileRequest: @escaping (Int, Int, Int, Int) -> Void = { _, _, _, _ in },
        onBitrateRequest: @escaping (Int) -> Void = { _ in },
        onClientTimeout: @escaping () -> Void = {}
    ) throws {
        self.displayBounds = displayBounds
        self.onKeyframeRequest = onKeyframeRequest
        self.onVideoProfileRequest = onVideoProfileRequest
        self.onBitrateRequest = onBitrateRequest
        self.onClientTimeout = onClientTimeout
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { throw POSIXError(.ENOTSOCK) }

        var reuse: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, socklen_t(MemoryLayout<Int32>.size))

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
    }

    deinit {
        running = false
        close(fd)
    }

    func start() {
        let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
        if !AXIsProcessTrustedWithOptions(options) {
            fputs("[input] Accessibility permission required. Approve this app/helper in System Settings.\n", stderr)
        }

        queue.async { [weak self] in
            self?.loop()
        }
        startTimeoutMonitor()
    }

    private func startTimeoutMonitor() {
        timeoutQueue.async { [weak self] in
            while true {
                Thread.sleep(forTimeInterval: 2.0)
                guard let self, self.running else { return }
                let last = self.lastPacketTime
                if last == 0 { continue }
                let now = nowUs()
                if now - last > Self.timeoutUs {
                    if self.clientConnected {
                        self.clientConnected = false
                        logLine("[input] client timeout, no packets for \(Self.timeoutUs / 1_000_000)s")
                        self.onClientTimeout()
                    }
                }
            }
        }
    }

    private func loop() {
        var buf = [UInt8](repeating: 0, count: 256)
        while running {
            let n = recv(fd, &buf, buf.count, 0)
            if n >= p2InputPacketBytes {
                lastPacketTime = nowUs()
                if !clientConnected {
                    clientConnected = true
                    logLine("[input] client connected (first packet received)")
                }
                handle(Array(buf[0..<n]))
            }
        }
    }

    private func point(x: Float, y: Float) -> CGPoint {
        let nx = CGFloat(max(0, min(1, x)))
        let ny = CGFloat(max(0, min(1, y)))
        return CGPoint(
            x: displayBounds.origin.x + nx * max(1, displayBounds.width - 1),
            y: displayBounds.origin.y + ny * max(1, displayBounds.height - 1)
        )
    }

    private func handle(_ bytes: [UInt8]) {
        guard bytes.count >= p2InputPacketBytes else { return }
        guard bytes[0] == 0x50, bytes[1] == 0x32, bytes[2] == 0x49, bytes[3] == 0x32 else { return } // P2I2
        guard bytes[4] == 1 else { return }
        let kind = bytes[5]
        let x = readFloatLE(bytes, 12)
        let y = readFloatLE(bytes, 16)
        let dx = readI32LE(bytes, 20)
        let dy = readI32LE(bytes, 24)
        let button = Int(readU16LE(bytes, 28))
        let keyCode = CGKeyCode(readU16LE(bytes, 30))
        let p = point(x: x, y: y)

        switch kind {
        case 1:
            postMouse(type: dragAwareMoveType(), point: p, button: dragButton())
        case 2:
            downButtons.insert(button)
            postMouse(type: mouseDownType(button), point: p, button: cgButton(button))
        case 3:
            postMouse(type: mouseUpType(button), point: p, button: cgButton(button))
            downButtons.remove(button)
        case 4:
            postMouse(type: dragAwareMoveType(), point: p, button: dragButton())
            postWheel(dx: dx, dy: dy)
        case 5:
            postKey(code: keyCode, down: true)
        case 6:
            postKey(code: keyCode, down: false)
        case p2InputText:
            postText(codeUnit: UInt16(keyCode), modifiers: UInt16(button))
        case p2InputRequestKeyframe:
            onKeyframeRequest()
        case p2InputSetVideoProfile:
            let width = max(640, Int(dx))
            let height = max(360, Int(dy))
            let fps = max(30, button)
            let bitrateMbps = max(0, Int(readU16LE(bytes, 30)))
            onVideoProfileRequest(width, height, fps, bitrateMbps)
        case p2InputSetVideoBitrate:
            let bitrate = max(2_000_000, Int(dx))
            onBitrateRequest(bitrate)
        default:
            return
        }
    }

    private func cgButton(_ button: Int) -> CGMouseButton {
        switch button {
        case 1: return .center
        case 2: return .right
        default: return .left
        }
    }

    private func mouseDownType(_ button: Int) -> CGEventType {
        switch button {
        case 1: return .otherMouseDown
        case 2: return .rightMouseDown
        default: return .leftMouseDown
        }
    }

    private func mouseUpType(_ button: Int) -> CGEventType {
        switch button {
        case 1: return .otherMouseUp
        case 2: return .rightMouseUp
        default: return .leftMouseUp
        }
    }

    private func dragAwareMoveType() -> CGEventType {
        if downButtons.contains(0) { return .leftMouseDragged }
        if downButtons.contains(2) { return .rightMouseDragged }
        if downButtons.contains(1) { return .otherMouseDragged }
        return .mouseMoved
    }

    private func dragButton() -> CGMouseButton {
        if downButtons.contains(0) { return .left }
        if downButtons.contains(2) { return .right }
        if downButtons.contains(1) { return .center }
        return .left
    }

    private func postMouse(type: CGEventType, point: CGPoint, button: CGMouseButton) {
        guard let event = CGEvent(mouseEventSource: nil, mouseType: type, mouseCursorPosition: point, mouseButton: button) else { return }
        event.post(tap: .cghidEventTap)
    }

    private func postWheel(dx: Int32, dy: Int32) {
        let wheelX = Int32(max(-5000, min(5000, -dx)))
        let wheelY = Int32(max(-5000, min(5000, -dy)))
        guard let event = CGEvent(scrollWheelEvent2Source: nil, units: .pixel, wheelCount: 2, wheel1: wheelY, wheel2: wheelX, wheel3: 0) else { return }
        event.post(tap: .cghidEventTap)
    }

    private func postKey(code: CGKeyCode, down: Bool) {
        guard let event = CGEvent(keyboardEventSource: nil, virtualKey: code, keyDown: down) else { return }
        event.post(tap: .cghidEventTap)
    }

    private func flags(from modifiers: UInt16) -> CGEventFlags {
        var flags = CGEventFlags()
        if (modifiers & p2ModShift) != 0 { flags.insert(.maskShift) }
        if (modifiers & p2ModControl) != 0 { flags.insert(.maskControl) }
        if (modifiers & p2ModOption) != 0 { flags.insert(.maskAlternate) }
        if (modifiers & p2ModCommand) != 0 { flags.insert(.maskCommand) }
        return flags
    }

    private func postText(codeUnit: UInt16, modifiers: UInt16) {
        guard codeUnit >= 0x20, codeUnit != 0x7f else { return }
        var chars = [UniChar(codeUnit)]
        guard let down = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: true),
              let up = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: false) else { return }
        let eventFlags = flags(from: modifiers)
        down.flags = eventFlags
        up.flags = eventFlags
        chars.withUnsafeBufferPointer { buffer in
            down.keyboardSetUnicodeString(stringLength: chars.count, unicodeString: buffer.baseAddress)
            up.keyboardSetUnicodeString(stringLength: chars.count, unicodeString: buffer.baseAddress)
        }
        down.post(tap: .cghidEventTap)
        up.post(tap: .cghidEventTap)
    }
}
