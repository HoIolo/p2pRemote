import Foundation
import CoreGraphics
import ApplicationServices

struct Bounds: Decodable {
    let x: Double
    let y: Double
    let width: Double
    let height: Double
}

struct RemoteEvent: Decodable {
    let kind: String
    let x: Double?
    let y: Double?
    let button: Int?
    let dx: Double?
    let dy: Double?
    let code: String?
    let key: String?
    let t: Double?
}

struct InputMessage: Decodable {
    let event: RemoteEvent
    let bounds: Bounds
}

let trustOptions = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
if !AXIsProcessTrustedWithOptions(trustOptions) {
    fputs("Accessibility permission is required for remote input. Approve this helper/app in System Settings.\n", stderr)
}

var downButtons = Set<Int>()

func clamp01(_ value: Double?) -> Double {
    guard let value = value, value.isFinite else { return 0.0 }
    return min(1.0, max(0.0, value))
}

func point(from event: RemoteEvent, in bounds: Bounds) -> CGPoint {
    let width = max(1.0, bounds.width - 1.0)
    let height = max(1.0, bounds.height - 1.0)
    return CGPoint(
        x: bounds.x + clamp01(event.x) * width,
        y: bounds.y + clamp01(event.y) * height
    )
}

func cgButton(_ button: Int?) -> CGMouseButton {
    switch button ?? 0 {
    case 1: return .center
    case 2: return .right
    default: return .left
    }
}

func mouseDownType(_ button: Int?) -> CGEventType {
    switch button ?? 0 {
    case 1: return .otherMouseDown
    case 2: return .rightMouseDown
    default: return .leftMouseDown
    }
}

func mouseUpType(_ button: Int?) -> CGEventType {
    switch button ?? 0 {
    case 1: return .otherMouseUp
    case 2: return .rightMouseUp
    default: return .leftMouseUp
    }
}

func mouseMoveType() -> CGEventType {
    if downButtons.contains(0) { return .leftMouseDragged }
    if downButtons.contains(2) { return .rightMouseDragged }
    if downButtons.contains(1) { return .otherMouseDragged }
    return .mouseMoved
}

func postMouse(_ type: CGEventType, _ location: CGPoint, _ button: CGMouseButton = .left) {
    guard let event = CGEvent(mouseEventSource: nil, mouseType: type, mouseCursorPosition: location, mouseButton: button) else { return }
    event.post(tap: .cghidEventTap)
}

func postScroll(dx: Double, dy: Double) {
    let wheelX = Int32(max(-5000.0, min(5000.0, -dx)))
    let wheelY = Int32(max(-5000.0, min(5000.0, -dy)))
    guard let event = CGEvent(scrollWheelEvent2Source: nil, units: .pixel, wheelCount: 2, wheel1: wheelY, wheel2: wheelX, wheel3: 0) else { return }
    event.post(tap: .cghidEventTap)
}

let keyMap: [String: CGKeyCode] = [
    "KeyA": 0, "KeyS": 1, "KeyD": 2, "KeyF": 3, "KeyH": 4, "KeyG": 5,
    "KeyZ": 6, "KeyX": 7, "KeyC": 8, "KeyV": 9, "KeyB": 11, "KeyQ": 12,
    "KeyW": 13, "KeyE": 14, "KeyR": 15, "KeyY": 16, "KeyT": 17,
    "Digit1": 18, "Digit2": 19, "Digit3": 20, "Digit4": 21, "Digit6": 22,
    "Digit5": 23, "Equal": 24, "Digit9": 25, "Digit7": 26, "Minus": 27,
    "Digit8": 28, "Digit0": 29, "BracketRight": 30, "KeyO": 31, "KeyU": 32,
    "BracketLeft": 33, "KeyI": 34, "KeyP": 35, "Enter": 36, "KeyL": 37,
    "KeyJ": 38, "Quote": 39, "KeyK": 40, "Semicolon": 41, "Backslash": 42,
    "Comma": 43, "Slash": 44, "KeyN": 45, "KeyM": 46, "Period": 47,
    "Tab": 48, "Space": 49, "Backquote": 50, "Backspace": 51, "Escape": 53,
    "MetaRight": 54, "MetaLeft": 55, "ShiftLeft": 56, "CapsLock": 57, "AltLeft": 58,
    "ControlLeft": 59, "ShiftRight": 60, "AltRight": 61, "ControlRight": 62,
    "NumpadDecimal": 65, "NumpadMultiply": 67, "NumpadAdd": 69, "NumLock": 71,
    "NumpadDivide": 75, "NumpadEnter": 76, "NumpadSubtract": 78, "NumpadEqual": 81,
    "Numpad0": 82, "Numpad1": 83, "Numpad2": 84, "Numpad3": 85, "Numpad4": 86,
    "Numpad5": 87, "Numpad6": 88, "Numpad7": 89, "Numpad8": 91, "Numpad9": 92,
    "F5": 96, "F6": 97, "F7": 98, "F3": 99, "F8": 100, "F9": 101,
    "F11": 103, "F13": 105, "F14": 107, "F10": 109, "F12": 111, "F15": 113,
    "Help": 114, "Home": 115, "PageUp": 116, "Delete": 117, "F4": 118, "End": 119,
    "F2": 120, "PageDown": 121, "F1": 122, "ArrowLeft": 123, "ArrowRight": 124,
    "ArrowDown": 125, "ArrowUp": 126
]

func postKey(code: String?, down: Bool) {
    guard let code = code, let keyCode = keyMap[code] else { return }
    guard let event = CGEvent(keyboardEventSource: nil, virtualKey: keyCode, keyDown: down) else { return }
    event.post(tap: .cghidEventTap)
}

func handle(_ message: InputMessage) {
    let event = message.event
    switch event.kind {
    case "pointerMove":
        postMouse(mouseMoveType(), point(from: event, in: message.bounds), .left)
    case "pointerDown":
        let button = event.button ?? 0
        downButtons.insert(button)
        postMouse(mouseDownType(button), point(from: event, in: message.bounds), cgButton(button))
    case "pointerUp":
        let button = event.button ?? 0
        downButtons.remove(button)
        postMouse(mouseUpType(button), point(from: event, in: message.bounds), cgButton(button))
    case "wheel":
        postMouse(mouseMoveType(), point(from: event, in: message.bounds), .left)
        postScroll(dx: event.dx ?? 0.0, dy: event.dy ?? 0.0)
    case "keyDown":
        postKey(code: event.code, down: true)
    case "keyUp":
        postKey(code: event.code, down: false)
    default:
        return
    }
}

let decoder = JSONDecoder()
while let line = readLine(strippingNewline: true) {
    guard let data = line.data(using: .utf8) else { continue }
    do {
        let message = try decoder.decode(InputMessage.self, from: data)
        handle(message)
    } catch {
        fputs("bad input json: \(error)\n", stderr)
    }
}
