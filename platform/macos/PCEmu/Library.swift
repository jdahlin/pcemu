import AppKit
import SwiftUI

struct GuestFrame {
    let pixels: Data
    let width: Int
    let height: Int
}

@MainActor final class Library: ObservableObject {
    @Published var machines: [VirtualMachine] = []
    @Published var selection: UUID?
    @Published var active: UUID?
    @Published var paused = false
    @Published var suspending = false
    @Published var takingSnapshot = false
    @Published var switchingTo: String?
    @Published var wantsReboot = false
    @Published var error: String?
    @Published var frame: GuestFrame?
    @Published var classic = false
    @Published var guestCursor: NSCursor?
    let root: URL
    let directory: URL
    private var pendingSnapshot: URL?
    private var queuedStart: (VirtualMachine, Bool)?
    private var suspendAfterSnapshot = false
    private var snapshotHardware: (Int, Int)?
    private var runningConfiguration: VirtualMachine?
    private var timer: Timer?
    private var pixels = [UInt32](repeating: 0, count: 2048 * 2048)
    var termination: (() -> Void)?

    init(root: URL? = nil, directory: URL? = nil) {
        self.root = root ?? URL(fileURLWithPath: Bundle.main.object(forInfoDictionaryKey: "PCEmuCheckout") as? String
                   ?? FileManager.default.currentDirectoryPath)
        self.directory = directory ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("PCEmu", isDirectory: true)
        do {
            machines = try DefaultMachines.register(root: self.root, directory: self.directory)
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
        guard !suspending && !takingSnapshot else { return }
        if active == vm.id { selection = vm.id; return }
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
            if active != nil {
                queuedStart = (vm, cold); switchingTo = vm.name
                suspend()
                if !suspending { queuedStart = nil; switchingTo = nil }
                return
            }
            if let index = machines.firstIndex(where: { $0.id == vm.id }) {
                machines[index].snapshot = vm.snapshot; machines[index].restoreOnOpen = vm.restoreOnOpen; save()
            }
            pcemu_native_prepare()
            error = nil
            var configuration = vm
            if !cold && vm.restoreOnOpen, let checkpoint = vm.snapshots?.first(where: { $0.path == vm.snapshot }) {
                configuration.memory = checkpoint.memory; configuration.processors = checkpoint.processors
            }
            runningConfiguration = configuration
            active = vm.id; selection = vm.id; paused = false; frame = nil
            let snapshot = (!cold && vm.restoreOnOpen) ? vm.snapshot : ""
            let checkout = root
            let run = directory.appendingPathComponent("Runs/\(UUID().uuidString)")
            let rom = "" // Use the embedded standalone VGA option ROM.
            timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30, repeats: true) { [weak self] _ in
                Task { @MainActor in self?.refreshFrame() }
            }
            DispatchQueue.global(qos: .userInitiated).async {
                do {
                    let result: Int32
                    if vm.isARM {
                        if snapshot.isEmpty { try Self.prepareARM(root: checkout, memory: vm.memory, cpus: vm.cpuCount) }
                        result = pcemu_native_run_arm(checkout.path, vm.disk, snapshot, UInt32(vm.memory), UInt32(vm.cpuCount), run.path)
                    } else {
                        result = pcemu_native_run(vm.machine, vm.disk, snapshot, rom, UInt32(vm.memory), vm.bootText ?? "", 1)
                    }
                    Task { @MainActor in self.finished(result) }
                } catch {
                    Task { @MainActor in self.error = error.localizedDescription; self.finished(1) }
                }
            }
        } catch { self.error = error.localizedDescription }
    }
    // Firmware tables must describe exactly the selected memory and VP count.
    nonisolated private static func prepareARM(root: URL, memory: Int, cpus: Int) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        process.arguments = ["python3", root.appendingPathComponent("tools/build_arm_platform.py").path,
                             "--ram-mb", String(memory), "--cpus", String(cpus), "--hyperv", "--vmbus", "--msi"]
        process.currentDirectoryURL = root
        try process.run(); process.waitUntilExit()
        if process.terminationStatus != 0 {
            throw NSError(domain: "PCEmu", code: Int(process.terminationStatus), userInfo: [NSLocalizedDescriptionKey: "Cannot prepare matching ARM64 firmware tables."])
        }
    }
    private func finished(_ status: Int32) {
        var message = [CChar](repeating: 0, count: 4096)
        if pcemu_native_error(&message, Int32(message.count)) != 0 { error = String(cString: message) }
        timer?.invalidate(); timer = nil
        if let snapshot = pendingSnapshot {
            if status == 0 && FileManager.default.fileExists(atPath: snapshot.appendingPathComponent("state.bin").path) {
                recordSnapshot(snapshot)
            } else { error = "Snapshot did not complete. The previous snapshot is still selected." }
        } else if status != 0 && error == nil { error = "The VM exited with an error (\(status)). See the launching terminal for emulator diagnostics." }
        let next = error == nil ? queuedStart : nil
        active = nil; paused = false; suspending = false; takingSnapshot = false; pendingSnapshot = nil
        runningConfiguration = nil; queuedStart = nil; switchingTo = nil
        suspendAfterSnapshot = false
        if termination == nil, let (vm, cold) = next {
            if let index = machines.firstIndex(where: { $0.id == vm.id }) {
                machines[index].snapshot = vm.snapshot; machines[index].restoreOnOpen = vm.restoreOnOpen; save()
            }
            start(vm, cold: cold)
        }
        if error == nil { termination?() }
        else if termination != nil { NSApp.reply(toApplicationShouldTerminate: false) }
        termination = nil
    }
    func togglePause() {
        guard active != nil && !suspending && !takingSnapshot else { return }
        paused.toggle(); command(paused ? "pause" : "resume")
    }
    private func recordSnapshot(_ path: URL) {
        guard let index = machines.firstIndex(where: { $0.id == active }) else { return }
        let hardware = snapshotHardware ?? (machines[index].memory, machines[index].cpuCount)
        var snapshots = machines[index].snapshots ?? []
        if !snapshots.contains(where: { $0.path == path.path }) {
            snapshots.insert(VMSnapshot(path: path.path, date: Date(), memory: hardware.0, processors: hardware.1), at: 0)
        }
        machines[index].snapshots = snapshots
        machines[index].snapshot = path.path
        save()
    }
    func restore(_ vm: VirtualMachine, snapshot: String) {
        guard !suspending, !takingSnapshot,
              let index = machines.firstIndex(where: { $0.id == vm.id }) else { return }
        guard FileManager.default.fileExists(atPath: snapshot + "/state.bin") else {
            error = "This snapshot is missing. Reveal it in Finder to check its location."; return
        }
        var target = machines[index]
        target.snapshot = snapshot; target.restoreOnOpen = true
        if active == vm.id {
            queuedStart = (target, false); switchingTo = vm.name; suspend()
            if !suspending { queuedStart = nil; switchingTo = nil }
        } else { start(target) }
    }
    func suspend() {
        if takingSnapshot { suspendAfterSnapshot = true; return }
        saveSnapshot(suspend: true)
    }
    func takeSnapshot() { saveSnapshot(suspend: false) }
    private func saveSnapshot(suspend: Bool) {
        guard let id = active, !suspending, !takingSnapshot else { return }
        let path = directory.appendingPathComponent("Snapshots/\(id)/\(UUID().uuidString)")
        do { try FileManager.default.createDirectory(at: path.deletingLastPathComponent(), withIntermediateDirectories: true) }
        catch {
            self.error = error.localizedDescription
            if termination != nil { NSApp.reply(toApplicationShouldTerminate: false); termination = nil }
            return
        }
        releaseInput(); suspending = suspend; takingSnapshot = !suspend; pendingSnapshot = path
        let configuration = runningConfiguration ?? machines.first { $0.id == id }
        snapshotHardware = configuration.map { ($0.memory, $0.cpuCount) }
        command("\(suspend ? "suspend" : "save-snapshot") \(path.path)")
    }
    func releaseInput() { pcemu_native_event(0, 0, 0, 0, 0, 0) }
    func refreshFrame() {
        var message = [CChar](repeating: 0, count: 4096)
        if pcemu_native_error(&message, Int32(message.count)) != 0 {
            error = String(cString: message)
            suspending = false; takingSnapshot = false; pendingSnapshot = nil
            queuedStart = nil; switchingTo = nil; suspendAfterSnapshot = false
            if termination != nil { NSApp.reply(toApplicationShouldTerminate: false); termination = nil }
        }
        // Both engines publish complete checkpoints with an atomic directory rename.
        if takingSnapshot, let path = pendingSnapshot,
           FileManager.default.fileExists(atPath: path.appendingPathComponent("state.bin").path) {
            recordSnapshot(path); pendingSnapshot = nil; takingSnapshot = false
            if suspendAfterSnapshot { suspendAfterSnapshot = false; suspend() }
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
        frame = GuestFrame(pixels: data, width: Int(w), height: Int(h)); classic = c != 0
    }
}
