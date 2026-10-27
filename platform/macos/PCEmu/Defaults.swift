import Foundation

struct VirtualMachine: Codable, Identifiable, Equatable {
    var id = UUID()
    var name = "New Virtual Machine"
    var machine = "win95"
    var disk = ""
    var snapshot = ""
    var memory = 32
    var restoreOnOpen = true
    var registrationKey: String?
    var launcher: String?
    var bootText: String?
    var snapshots: [VMSnapshot]?
    var processors: Int?
    var isARM: Bool { launcher != nil }
    var cpuCount: Int { isARM ? processors ?? 2 : 1 }
}

struct VMSnapshot: Codable, Identifiable, Equatable {
    var id: String { path }
    var path: String
    var date: Date
    var memory: Int
    var processors: Int
}

enum DefaultMachines {
    static func register(root: URL, directory: URL,
                         external: URL = URL(fileURLWithPath: "/Volumes/data/pcemu")) throws -> [VirtualMachine] {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let path = directory.appendingPathComponent("Library.json")
        let ledger = directory.appendingPathComponent("RegisteredDefaults.json")
        var machines = FileManager.default.fileExists(atPath: path.path)
            ? try JSONDecoder().decode([VirtualMachine].self, from: Data(contentsOf: path)) : []
        var registered = FileManager.default.fileExists(atPath: ledger.path)
            ? Set(try JSONDecoder().decode([String].self, from: Data(contentsOf: ledger))) : Set<String>()
        for vm in discover(root: root, external: external) {
            guard let key = vm.registrationKey, !registered.contains(key) else { continue }
            if !machines.contains(where: { $0.registrationKey == key || $0.disk == vm.disk }) { machines.append(vm) }
            registered.insert(key)
        }
        try JSONEncoder().encode(machines).write(to: path, options: .atomic)
        try JSONEncoder().encode(registered.sorted()).write(to: ledger, options: .atomic)
        return machines
    }

    static func discover(root: URL, external: URL = URL(fileURLWithPath: "/Volumes/data/pcemu")) -> [VirtualMachine] {
        func existing(_ paths: [URL], snapshot: Bool = false) -> String {
            paths.first { FileManager.default.fileExists(atPath: snapshot ? $0.appendingPathComponent("state.bin").path : $0.path) }?.path ?? ""
        }
        let media = root.appendingPathComponent("media")
        let recovered = external.appendingPathComponent("recovered")
        let win311 = existing([media.appendingPathComponent("win311.raw"), recovered.appendingPathComponent("win311.qcow2")])
        let win95 = existing(["win95-native.qcow2", "win95-mouse.qcow2", "win95-monitor.qcow2", "win95-svga.qcow2", "win95-working.qcow2", "win95.raw"].map { media.appendingPathComponent($0) } + [recovered.appendingPathComponent("win95.qcow2")])
        let win11 = existing([media.appendingPathComponent("windows11-arm64/system.qcow2")])
        var result: [VirtualMachine] = []
        for (key, name, machine, disk, ram, boot) in [
            ("win311", "Windows 3.11", "win311", win311, 16, "cd windows\nwin /s\n"),
            ("win95", "Windows 95", "win95", win95, 64, ""),
            ("win11", "Windows 11", "arm64-uefi", win11, 4096, "")] where !disk.isEmpty {
            var vm = VirtualMachine(name: name, machine: machine, disk: disk, memory: ram)
            vm.registrationKey = key
            vm.bootText = boot
            if key == "win11" { vm.launcher = "run11.sh"; vm.processors = 2 }
            else {
                vm.snapshot = existing([root.appendingPathComponent("build/snapshots/\(key)-desktop"),
                    external.appendingPathComponent("snapshots/\(key)-desktop")], snapshot: true)
            }
            result.append(vm)
        }
        return result
    }
}
