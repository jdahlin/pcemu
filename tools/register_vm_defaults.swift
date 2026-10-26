import Foundation

@main struct RegisterDefaults {
    static func main() throws {
        let root = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : FileManager.default.currentDirectoryPath)
        let directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("PCEmu")
        let machines = try DefaultMachines.register(root: root, directory: directory)
        for vm in machines { print("\(vm.name): \(vm.disk)" + (vm.isARM ? " (ARM64 window)" : "")) }
    }
}
