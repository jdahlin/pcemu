import AppKit
import SwiftUI

@MainActor final class Library: ObservableObject {
    @Published var machines: [VirtualMachine] = []
    @Published var selection: UUID?
    @Published var active: UUID?
    @Published var paused = false
    @Published var suspending = false
    @Published var wantsReboot = false
    @Published var error: String?
    @Published var image: NSImage?
    @Published var classic = false
    @Published var guestCursor: NSCursor?
    let root: URL
    let directory: URL
    private var pendingSnapshot: URL?
    private var externalProcess: Process?
    var activeIsARM: Bool { machines.first { $0.id == active }?.isARM == true }
    private var timer: Timer?
    private var pixels = [UInt32](repeating: 0, count: 2048 * 2048)
    var termination: (() -> Void)?

    init() {
        root = URL(fileURLWithPath: Bundle.main.object(forInfoDictionaryKey: "PCEmuCheckout") as? String
                   ?? FileManager.default.currentDirectoryPath)
        directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("PCEmu", isDirectory: true)
        do {
            machines = try DefaultMachines.register(root: root, directory: directory)
            selection = machines.first?.id
        } catch { self.error = "Cannot read the VM library: \(error.localizedDescription)" }
    }
    func save() {
        do { try JSONEncoder().encode(machines).write(to: directory.appendingPathComponent("Library.json"), options: .atomic) }
        catch { self.error = "Cannot save the VM library: \(error.localizedDescription)" }
    }
    func add() {
        let vm = VirtualMachine()
        machines.append(vm); selection = vm.id; save()
    }
    func remove(_ vm: VirtualMachine) {
        guard active != vm.id else { return }
        machines.removeAll { $0.id == vm.id }; selection = machines.first?.id; save()
    }
    func command(_ text: String) {
        pcemu_native_command(text)
    }
    func start(_ vm: VirtualMachine, cold: Bool = false) {
        guard active == nil else { return }
        if vm.isARM { startARM(vm); return }
        do {
            if !cold && vm.restoreOnOpen && !vm.snapshot.isEmpty {
                guard FileManager.default.fileExists(atPath: vm.snapshot + "/state.bin") else {
                    throw NSError(domain: "PCEmu", code: 1, userInfo: [NSLocalizedDescriptionKey: "The saved snapshot is missing. Choose a snapshot or use Cold Boot."])
                }
            } else {
                guard !vm.disk.isEmpty else {
                    throw NSError(domain: "PCEmu", code: 2, userInfo: [NSLocalizedDescriptionKey: "Choose a boot disk in VM settings before cold booting."])
                }
            }
            pcemu_native_prepare()
            active = vm.id; selection = vm.id; paused = false; image = nil
            let snapshot = (!cold && vm.restoreOnOpen) ? vm.snapshot : ""
            let rom = root.appendingPathComponent("roms/vgabios.bin").path
            timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30, repeats: true) { [weak self] _ in
                Task { @MainActor in self?.refreshFrame() }
            }
            DispatchQueue.global(qos: .userInitiated).async {
                let result = pcemu_native_run(vm.machine, vm.disk, snapshot, rom, UInt32(vm.memory), vm.bootText ?? "", vm.machine == "win95" ? 1 : 0)
                Task { @MainActor in self.finished(result) }
            }
        } catch { self.error = error.localizedDescription }
    }
    private func startARM(_ vm: VirtualMachine) {
        if let app = NSRunningApplication.runningApplications(withBundleIdentifier: "local.pcemu11").first {
            app.activate(options: [])
            return
        }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/sh")
        process.arguments = [root.appendingPathComponent("run11.sh").path]
        process.currentDirectoryURL = root
        var environment = ProcessInfo.processInfo.environment
        environment["PCEMU_RAM_MB"] = String(vm.memory)
        environment["PCEMU_CPUS"] = String(vm.cpuCount)
        process.environment = environment
        process.terminationHandler = { child in
            Task { @MainActor in
                self.externalProcess = nil
                self.finished(child.terminationStatus)
            }
        }
        do { try process.run(); externalProcess = process; active = vm.id; selection = vm.id }
        catch { self.error = error.localizedDescription }
    }
    private func finished(_ status: Int32) {
        var message = [CChar](repeating: 0, count: 4096)
        if !activeIsARM && pcemu_native_error(&message, Int32(message.count)) != 0 { error = String(cString: message) }
        timer?.invalidate(); timer = nil
        if let snapshot = pendingSnapshot {
            if status == 0 && FileManager.default.fileExists(atPath: snapshot.appendingPathComponent("state.bin").path),
               let index = machines.firstIndex(where: { $0.id == active }) {
                machines[index].snapshot = snapshot.path; save()
            } else { error = "Suspend did not complete. The previous snapshot is still selected." }
        } else if status != 0 && error == nil { error = "The VM exited with an error (\(status)). See the launching terminal for emulator diagnostics." }
        active = nil; paused = false; suspending = false; pendingSnapshot = nil
        if error == nil { termination?() }
        else if termination != nil { NSApp.reply(toApplicationShouldTerminate: false) }
        termination = nil
    }
    func togglePause() {
        guard active != nil && !activeIsARM && !suspending else { return }
        paused.toggle(); command(paused ? "pause" : "resume")
    }
    func suspend() {
        guard let id = active, !activeIsARM, !suspending else { return }
        let path = directory.appendingPathComponent("Snapshots/\(id)/\(UUID().uuidString)")
        do { try FileManager.default.createDirectory(at: path.deletingLastPathComponent(), withIntermediateDirectories: true) }
        catch {
            self.error = error.localizedDescription
            if termination != nil { NSApp.reply(toApplicationShouldTerminate: false); termination = nil }
            return
        }
        releaseInput(); suspending = true; pendingSnapshot = path
        command("suspend \(path.path)")
    }
    func releaseInput() { pcemu_native_event(0, 0, 0, 0, 0, 0) }
    func refreshFrame() {
        var message = [CChar](repeating: 0, count: 4096)
        if pcemu_native_error(&message, Int32(message.count)) != 0 {
            error = String(cString: message)
            suspending = false; pendingSnapshot = nil
            if termination != nil { NSApp.reply(toApplicationShouldTerminate: false); termination = nil }
        }
        var cursorPixels = [UInt32](repeating: 0, count: 64 * 64)
        var cw: Int32 = 0, ch: Int32 = 0, hx: Int32 = 0, hy: Int32 = 0, visible: Int32 = 0
        if pcemu_native_cursor(&cursorPixels, &cw, &ch, &hx, &hy, &visible) != 0 {
            let data = cursorPixels.withUnsafeBytes { Data($0.prefix(Int(cw * ch) * 4)) }
            if let provider = CGDataProvider(data: data as CFData),
               let cg = CGImage(width: Int(cw), height: Int(ch), bitsPerComponent: 8, bitsPerPixel: 32,
                   bytesPerRow: Int(cw) * 4, space: CGColorSpaceCreateDeviceRGB(),
                   bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.first.rawValue).union(.byteOrder32Little),
                   provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent) {
                guestCursor = NSCursor(image: NSImage(cgImage: cg, size: NSSize(width: Int(cw), height: Int(ch))),
                    hotSpot: NSPoint(x: Int(hx), y: Int(hy)))
            }
        }
        var w: Int32 = 0, h: Int32 = 0, c: Int32 = 0
        let count = pixels.withUnsafeMutableBufferPointer { pcemu_native_frame($0.baseAddress!, Int32($0.count), &w, &h, &c) }
        guard count > 0 else { return }
        let data = pixels.withUnsafeBytes { Data($0.prefix(Int(count) * 4)) }
        guard let provider = CGDataProvider(data: data as CFData),
              let cg = CGImage(width: Int(w), height: Int(h), bitsPerComponent: 8, bitsPerPixel: 32,
                bytesPerRow: Int(w) * 4, space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue).union(.byteOrder32Little),
                provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent) else { return }
        image = NSImage(cgImage: cg, size: NSSize(width: Int(w), height: Int(h))); classic = c != 0
    }
}
