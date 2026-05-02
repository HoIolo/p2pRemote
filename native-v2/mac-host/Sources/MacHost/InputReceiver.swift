import Foundation
import CoreGraphics
import ApplicationServices
import Darwin

final class InputReceiver {
    private let fd: Int32
    private let displayBounds: CGRect
    private let queue = DispatchQueue(label: "p2p.native.input", qos: .userInteractive)
    private var running = true
    private var downButtons = Set<Int>()

    init(port: UInt16, displayBounds: CGRect) throws {
        self.displayBounds = displayBounds
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
    }

    private func loop() {
        var buf = [UInt8](repeating: 0, count: 256)
        while running {
            let n = recv(fd, &buf, buf.count, 0)
            if n >= p2InputPacketBytes {
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
}
