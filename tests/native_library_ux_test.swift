import AppKit

@main struct NativeLibraryUXTest {
    @MainActor static func wait(_ library: Library, until condition: () -> Bool) async {
        for _ in 0..<300 {
            library.refreshFrame()
            if condition() { return }
            try? await Task.sleep(nanoseconds: 10_000_000)
        }
        fatalError("Session transition timed out: \(library.error ?? "no error")")
    }
    @MainActor static func main() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("pcemu-ux-\(UUID())")
        defer { try? FileManager.default.removeItem(at: directory) }
        let library = Library(root: directory, directory: directory)
        let first = VirtualMachine(name: "First", disk: "test.raw", memory: 16)
        let second = VirtualMachine(name: "Second", disk: "test2.raw", memory: 32)
        library.machines = [first, second]
        library.start(first)
        library.togglePause()
        library.takeSnapshot()
        await wait(library) { !library.takingSnapshot }
        precondition(library.active == first.id && library.paused, "Snapshot preserves paused session")
        let checkpoint = library.machines[0].snapshot
        precondition(library.machines[0].snapshots?.count == 1)
        // Editing settings while running must not mislabel a checkpoint's hardware.
        library.machines[0].memory = 48
        library.takeSnapshot()
        await wait(library) { !library.takingSnapshot }
        precondition(library.machines[0].snapshots?.first?.memory == 16)
        library.command("fail-next-snapshot")
        let previous = library.machines[0].snapshot
        library.start(second)
        await wait(library) { !library.suspending }
        precondition(library.active == first.id && library.error != nil && library.switchingTo == nil)
        precondition(library.machines[0].snapshot == previous, "Failed save preserves the selected checkpoint")
        library.error = nil
        library.start(second)
        await wait(library) { library.active == second.id }
        precondition(library.machines[0].snapshots?.count == 3)
        library.restore(library.machines[0], snapshot: checkpoint)
        await wait(library) { library.active == first.id }
        precondition(library.machines[0].snapshot == checkpoint)
        library.restore(library.machines[0], snapshot: checkpoint)
        await wait(library) { !library.suspending }
        precondition(library.active == first.id && library.machines[0].snapshot == checkpoint)
        library.takeSnapshot()
        library.suspend() // Quit while a non-stopping snapshot is still being written.
        await wait(library) { library.active == nil }
        precondition(library.error == nil)
        let reloaded = Library(root: directory, directory: directory)
        precondition(reloaded.machines == library.machines, "Snapshot history persists across launches")
        print("Snapshot, pause, switch, restore, hardware metadata and deferred suspend passed")
    }
}
