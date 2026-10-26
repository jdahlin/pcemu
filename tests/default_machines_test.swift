import Foundation

@main struct DefaultMachinesTest {
    static func main() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("pcemu-defaults-\(UUID())")
        defer { try? FileManager.default.removeItem(at: root) }
        let external = root.appendingPathComponent("external")
        let library = root.appendingPathComponent("library")
        for path in ["external/recovered/win311.qcow2", "media/win95-native.qcow2", "media/windows11-arm64/system.qcow2", "build/snapshots/win95-desktop/state.bin"] {
            let file = root.appendingPathComponent(path)
            try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data().write(to: file)
        }
        let first = try DefaultMachines.register(root: root, directory: library, external: external)
        precondition(first.map(\.name) == ["Windows 3.11", "Windows 95", "Windows 11"])
        precondition(first[1].snapshot.hasSuffix("win95-desktop"))
        precondition(first[0].bootText == "cd windows\nwin /s\n")
        precondition(first[2].isARM && first[2].cpuCount == 2 && first[2].memory == 4096)
        let second = try DefaultMachines.register(root: root, directory: library, external: external)
        precondition(first == second, "Registration must be idempotent")
        var edited = first
        edited[0].name = "My Windows"; edited.removeLast()
        try JSONEncoder().encode(edited).write(to: library.appendingPathComponent("Library.json"))
        let third = try DefaultMachines.register(root: root, directory: library, external: external)
        precondition(third == edited, "Preserve edits and deliberately removed defaults")
        // Earlier libraries have none of the optional registration/backend fields.
        let old = """
        {"id":"00000000-0000-0000-0000-000000000001","name":"Old VM","machine":"win95","disk":"old.raw","snapshot":"","memory":32,"restoreOnOpen":true}
        """
        let migrated = try JSONDecoder().decode(VirtualMachine.self, from: Data(old.utf8))
        precondition(!migrated.isARM && migrated.cpuCount == 1)
        print("Default discovery, registration, removal and migration passed")
    }
}
