// Real-input play test: posts OS-level keyboard and mouse events to the vc3
// window, so the game sees them through macOS -> SDL -> host_input.c -> JVS
// exactly as a player's would (VC3_AUTOKEYS bypasses all of that).
//   swift scripts/real-input.swift
// Moves the real cursor. Needs Accessibility permission for the terminal.
import CoreGraphics
import Foundation

func log(_ s: String) { FileHandle.standardError.write((s + "\n").data(using: .utf8)!) }

func gameWindow() -> CGRect? {
    let opts: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
    guard let list = CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: Any]] else { return nil }
    for w in list where (w[kCGWindowOwnerName as String] as? String) == "vc3" {
        if let b = w[kCGWindowBounds as String] as? [String: CGFloat],
           let x = b["X"], let y = b["Y"], let wd = b["Width"], let h = b["Height"], wd > 200 {
            return CGRect(x: x, y: y, width: wd, height: h)
        }
    }
    return nil
}

let src = CGEventSource(stateID: .hidSystemState)

func key(_ code: CGKeyCode, _ name: String) {
    CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: true)?.post(tap: .cghidEventTap)
    usleep(300_000)
    CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: false)?.post(tap: .cghidEventTap)
    log("[REAL] key \(name)")
}

func move(_ p: CGPoint) {
    CGEvent(mouseEventSource: src, mouseType: .mouseMoved, mouseCursorPosition: p,
            mouseButton: .left)?.post(tap: .cghidEventTap)
}

func click(_ p: CGPoint, right: Bool = false) {
    move(p)
    usleep(80_000)
    let (dn, up, b): (CGEventType, CGEventType, CGMouseButton) =
        right ? (.rightMouseDown, .rightMouseUp, .right) : (.leftMouseDown, .leftMouseUp, .left)
    CGEvent(mouseEventSource: src, mouseType: dn, mouseCursorPosition: p, mouseButton: b)?.post(tap: .cghidEventTap)
    usleep(120_000)
    CGEvent(mouseEventSource: src, mouseType: up, mouseCursorPosition: p, mouseButton: b)?.post(tap: .cghidEventTap)
    log("[REAL] \(right ? "right" : "left") click at \(Int(p.x)),\(Int(p.y))")
}

// Wait for the window.
var win: CGRect? = nil
for _ in 0..<60 { if let w = gameWindow() { win = w; break }; sleep(1) }
guard let r = win else { log("[REAL] no vc3 window"); exit(1) }
log("[REAL] window \(r)")
func at(_ fx: CGFloat, _ fy: CGFloat) -> CGPoint { CGPoint(x: r.minX + r.width * fx, y: r.minY + r.height * fy) }

let t0 = Date()
func waitUntil(_ s: Double) { let d = s - Date().timeIntervalSince(t0); if d > 0 { usleep(useconds_t(d * 1e6)) } }

click(at(0.5, 0.5))                  // focus the window
waitUntil(30); key(23, "5 (coin)")   // kVK_ANSI_5
waitUntil(33); key(18, "1 (start)")  // kVK_ANSI_1
waitUntil(35); key(18, "1 (start)")
waitUntil(50); click(at(0.5, 0.55))  // Mission Select: shoot the middle file
waitUntil(60); key(18, "1 (start)")  // skip the briefing
waitUntil(62); key(18, "1 (start)")
// Combat: sweep shots across the room, reloading off-screen now and then.
var t = 66.0, i = 0
let pts: [(CGFloat, CGFloat)] = [(0.35, 0.45), (0.65, 0.45), (0.5, 0.4), (0.3, 0.55), (0.7, 0.55), (0.45, 0.5)]
while t < 92 {
    waitUntil(t)
    let p = pts[i % pts.count]
    if i % 7 == 6 { click(at(p.0, p.1), right: true) } else { click(at(p.0, p.1)) }
    t += 1.0; i += 1
}
log("[REAL] done")
