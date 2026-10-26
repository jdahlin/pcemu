import AppKit
import SwiftUI
import CoreGraphics

struct GuestDisplay: NSViewRepresentable {
    @ObservedObject var library: Library
    func makeNSView(context: Context) -> GuestView { GuestView(library: library) }
    static func dismantleNSView(_ view: GuestView, coordinator: ()) { view.releaseCapture() }
    func updateNSView(_ view: GuestView, context: Context) {
        view.image = library.image; view.classic = library.classic; view.needsDisplay = true
        view.updateCapture()
        view.window?.invalidateCursorRects(for: view)
    }
}

final class GuestView: NSView {
    let library: Library
    var image: NSImage?
    var classic = false
    private var captured = false
    private var keys = Set<UInt16>()
    private var tracking: NSTrackingArea?
    private var observers: [NSObjectProtocol] = []
    override var acceptsFirstResponder: Bool { true }
    override var isFlipped: Bool { true }
    init(library: Library) {
        self.library = library
        super.init(frame: .zero)
        for name in [NSWindow.didResignKeyNotification, NSApplication.didResignActiveNotification] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.releaseCapture() }
            })
        }
    }
    required init?(coder: NSCoder) { fatalError() }
    deinit { for observer in observers { NotificationCenter.default.removeObserver(observer) } }
    var guestRect: NSRect {
        guard let image else { return bounds }
        let aspect = classic ? 4.0 / 3 : image.size.width / image.size.height
        let w = min(bounds.width, bounds.height * aspect), h = min(bounds.height, bounds.width / aspect)
        return NSRect(x: (bounds.width-w)/2, y: (bounds.height-h)/2, width: w, height: h)
    }
    override func draw(_ dirtyRect: NSRect) {
        NSColor.black.setFill(); bounds.fill()
        image?.draw(in: guestRect, from: .zero, operation: .copy, fraction: 1, respectFlipped: true, hints: nil)
    }
    override func resetCursorRects() {
        if pcemu_native_absolute() != 0, let cursor = library.guestCursor, acceptsInput {
            addCursorRect(guestRect, cursor: cursor)
        }
    }
    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking { removeTrackingArea(tracking) }
        tracking = NSTrackingArea(rect: .zero, options: [.activeInKeyWindow, .inVisibleRect, .mouseMoved, .mouseEnteredAndExited], owner: self)
        addTrackingArea(tracking!)
    }
    func updateCapture() {
        let next = library.active != nil && !library.paused && !library.suspending && pcemu_native_captured() != 0
        if captured != next {
            captured = next
            CGAssociateMouseAndMouseCursorPosition(next ? 0 : 1)
            if next { NSCursor.hide() } else { NSCursor.unhide() }
        }
    }
    func releaseCapture() {
        library.releaseInput(); keys.removeAll()
        if captured { captured = false; CGAssociateMouseAndMouseCursorPosition(1); NSCursor.unhide() }
    }
    override func resignFirstResponder() -> Bool { releaseCapture(); return true }
    private var acceptsInput: Bool { library.active != nil && !library.paused && !library.suspending }
    private func send(_ kind: Int32, key: UInt16 = 0, up: Bool = false, button: Int32 = 0, x: Int32 = 0, y: Int32 = 0) {
        guard acceptsInput else { return }
        pcemu_native_event(kind, key, up ? 1 : 0, button, x, y)
    }
    override func keyDown(with event: NSEvent) {
        if event.modifierFlags.contains([.control, .option]) { releaseCapture(); return }
        guard let usage = Self.usages[event.keyCode] else { return }
        keys.insert(usage); send(1, key: usage)
    }
    override func keyUp(with event: NSEvent) {
        guard let usage = Self.usages[event.keyCode] else { return }
        keys.remove(usage); send(1, key: usage, up: true)
    }
    override func flagsChanged(with event: NSEvent) {
        if event.keyCode == 57 { send(1, key: 57); send(1, key: 57, up: true); return }
        if event.modifierFlags.contains([.control, .option]) { releaseCapture(); return }
        let modifiers: [UInt16: (UInt16, NSEvent.ModifierFlags)] = [
            56:(225,.shift),60:(229,.shift),59:(224,.control),62:(228,.control),
            58:(226,.option),61:(230,.option),55:(227,.command),54:(231,.command)]
        guard let (usage, flag) = modifiers[event.keyCode] else { return }
        let up = keys.contains(usage) || !event.modifierFlags.contains(flag)
        if up { keys.remove(usage) } else { keys.insert(usage) }
        send(1, key: usage, up: up)
    }
    private func pointer(_ event: NSEvent) {
        guard guestRect.width > 0, guestRect.height > 0 else { return }
        let p = convert(event.locationInWindow, from: nil)
        if pcemu_native_absolute() != 0 {
            send(5, x: Int32(max(0,min(65535,(p.x-guestRect.minX)/guestRect.width*65535))),
                 y: Int32(max(0,min(65535,(p.y-guestRect.minY)/guestRect.height*65535))))
        } else { send(4, x: Int32(event.deltaX), y: Int32(event.deltaY)) }
    }
    private func button(_ event: NSEvent, up: Bool) {
        guard acceptsInput else { return }
        window?.makeFirstResponder(self)
        pointer(event)
        send(3, up: up, button: event.buttonNumber == 0 ? 1 : event.buttonNumber == 1 ? 2 : 4)
    }
    override func mouseDown(with e: NSEvent) { button(e, up: false) }
    override func mouseUp(with e: NSEvent) { button(e, up: true) }
    override func rightMouseDown(with e: NSEvent) {
        if captured || (acceptsInput && pcemu_native_absolute() != 0 && window?.firstResponder === self) { button(e, up: false) }
        else { NSMenu.popUpContextMenu(contextMenu(), with: e, for: self) }
    }
    override func rightMouseUp(with e: NSEvent) { button(e, up: true) }
    override func otherMouseDown(with e: NSEvent) { button(e, up: false) }
    override func otherMouseUp(with e: NSEvent) { button(e, up: true) }
    override func mouseMoved(with e: NSEvent) { pointer(e) }
    override func mouseDragged(with e: NSEvent) { pointer(e) }
    override func rightMouseDragged(with e: NSEvent) { pointer(e) }
    override func otherMouseDragged(with e: NSEvent) { pointer(e) }
    override func scrollWheel(with e: NSEvent) { send(7, y: e.scrollingDeltaY > 0 ? 1 : -1) }
    override func mouseExited(with e: NSEvent) { send(6) }
    private func contextMenu() -> NSMenu {
        let menu = NSMenu()
        for (title, action) in [("Capture Keyboard and Mouse", #selector(captureInput)), ("Release Input (⌃⌥)", #selector(releaseInput)),
                                (library.paused ? "Resume" : "Pause", #selector(pauseVM)), ("Suspend", #selector(suspendVM))] {
            let item = NSMenuItem(title: title, action: action, keyEquivalent: ""); item.target = self; menu.addItem(item)
        }
        return menu
    }
    @objc func captureInput() { window?.makeFirstResponder(self); send(2) }
    @objc func releaseInput() { releaseCapture() }
    @objc func pauseVM() { library.togglePause() }
    @objc func suspendVM() { library.suspend() }
    // macOS virtual keycodes -> USB HID usages. Guest layout handles characters.
    static let usages: [UInt16: UInt16] = [
        0:4,1:22,2:7,3:9,4:11,5:10,6:29,7:27,8:6,9:25,11:5,12:20,13:26,14:8,15:21,
        16:28,17:23,18:30,19:31,20:32,21:33,22:35,23:34,24:46,25:38,26:36,27:45,28:37,29:39,
        30:48,31:18,32:24,33:47,34:12,35:19,36:40,37:15,38:13,39:52,40:14,41:51,42:49,43:54,
        44:56,45:17,46:16,47:55,48:43,49:44,50:53,51:42,53:41,57:57,65:99,67:85,69:87,
        71:83,75:84,76:88,78:86,81:103,82:98,83:89,84:90,85:91,86:92,87:93,88:94,89:95,
        91:96,92:97,96:62,97:63,98:64,99:60,100:65,101:66,103:68,105:104,106:107,107:105,
        109:67,111:69,113:106,114:73,115:74,116:75,117:76,118:61,119:77,120:59,121:78,122:58,
        123:80,124:79,125:81,126:82]
}
