import SwiftUI
import AppKit

@MainActor final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var library: Library?
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let library, library.active != nil else { return .terminateNow }
        library.termination = { NSApp.reply(toApplicationShouldTerminate: true) }
        library.suspend()
        return .terminateLater
    }
}

@main struct PCEmuApp: App {
    @StateObject private var library = Library()
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    var body: some Scene {
        Window("PCEmu", id: "library") {
            LibraryView(library: library)
                .onAppear { delegate.library = library }
        }
        .defaultSize(width: 1150, height: 760)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("New Virtual Machine…") { library.add() }.keyboardShortcut("n")
            }
            CommandMenu("Virtual Machine") {
                Button(library.paused ? "Resume" : "Pause") { library.togglePause() }
                    .keyboardShortcut("p", modifiers: [.command, .shift]).disabled(library.active == nil || library.suspending)
                Button("Suspend and Save") { library.suspend() }
                    .keyboardShortcut("s", modifiers: [.command, .shift]).disabled(library.active == nil || library.suspending)
                Button("Reboot…") { library.wantsReboot = true }
                    .disabled(library.active == nil || library.suspending)
                Divider()
                Button("Release Keyboard and Mouse") { library.releaseInput() }
                    .keyboardShortcut(.escape, modifiers: [.command])
            }
        }
    }
}

struct LibraryView: View {
    @ObservedObject var library: Library
    @State private var editing: UUID?
    @State private var deleting: VirtualMachine?
    var selected: VirtualMachine? { library.machines.first { $0.id == library.selection } }
    var body: some View {
        NavigationSplitView {
            List(selection: $library.selection) {
                Section("Virtual Machines") {
                    ForEach(library.machines) { vm in
                        HStack {
                            Image(systemName: "desktopcomputer").font(.title2).foregroundStyle(.tint)
                            VStack(alignment: .leading) {
                                Text(vm.name)
                                Text(library.active == vm.id ? (library.suspending ? "Saving state…" : library.paused ? "Paused" : "Running") : vm.snapshot.isEmpty ? "Powered off" : "Saved state")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            if library.active == vm.id { Circle().fill(library.paused ? .orange : .green).frame(width: 7, height: 7) }
                        }
                        .padding(.vertical, 5).tag(vm.id)
                        .contextMenu { vmActions(vm) }
                        .onTapGesture(count: 2) { library.start(vm) }
                    }
                }
            }
            .navigationSplitViewColumnWidth(min: 200, ideal: 240)
            .safeAreaInset(edge: .bottom) {
                Button { library.add() } label: { Label("New Virtual Machine", systemImage: "plus") }
                    .buttonStyle(.borderless).padding()
            }
        } detail: {
            if let vm = selected {
                VStack(spacing: 0) {
                    HStack {
                        VStack(alignment: .leading) {
                            Text(vm.name).font(.headline)
                            Text("\(vm.machine) · \(vm.memory) MB · \(vm.cpuCount) vCPU").font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        if library.active == vm.id {
                            Text(library.suspending ? "Saving snapshot…" : library.paused ? "Paused" : "Running")
                                .foregroundStyle(.secondary)
                        }
                    }.padding()
                    Divider()
                    if library.active == vm.id {
                        GuestDisplay(library: library)
                            .overlay(alignment: .top) {
                                if library.paused { Text("Paused").padding(8).background(.regularMaterial, in: Capsule()).padding() }
                            }
                        HStack {
                            Text("Click to capture input · Control–Option to release").font(.caption)
                            Spacer()
                            Button("Release Input") { library.releaseInput() }.buttonStyle(.borderless)
                        }.padding(10).foregroundStyle(.secondary)
                    } else {
                        VStack(spacing: 18) {
                            Image(systemName: "desktopcomputer").font(.system(size: 72, weight: .ultraLight)).foregroundStyle(.secondary)
                            Text(vm.snapshot.isEmpty ? "Ready to boot" : "Ready to restore").font(.title2)
                            Text(vm.restoreOnOpen && !vm.snapshot.isEmpty ? "Continue from the saved snapshot." : "Start from the configured boot disk.")
                                .foregroundStyle(.secondary)
                            Button(vm.restoreOnOpen && !vm.snapshot.isEmpty ? "Restore Virtual Machine" : "Start Virtual Machine") { library.start(vm) }
                                .buttonStyle(.borderedProminent).disabled(library.active != nil)
                            Button("Settings…") { editing = vm.id }
                            if library.active != nil { Text("Suspend the running VM before starting another.").font(.caption).foregroundStyle(.secondary) }
                        }.frame(maxWidth: .infinity, maxHeight: .infinity)
                            .contextMenu { vmActions(vm) }
                    }
                }
                .toolbar {
                    if library.active == vm.id {
                        Button { library.togglePause() } label: { Label(library.paused ? "Resume" : "Pause", systemImage: library.paused ? "play.fill" : "pause.fill") }
                        Button { library.suspend() } label: { Label("Suspend", systemImage: "moon.zzz") }
                        Button { library.wantsReboot = true } label: { Label("Reboot", systemImage: "arrow.clockwise") }
                    } else if library.active != vm.id {
                        Button { library.start(vm) } label: { Label("Start", systemImage: "play.fill") }.disabled(library.active != nil)
                    }
                    Button { editing = vm.id } label: { Label("Settings", systemImage: "slider.horizontal.3") }
                        .disabled(library.active == vm.id)
                }
                .disabled(library.suspending)
            } else {
                VStack(spacing: 16) {
                    Image(systemName: "square.stack.3d.up").font(.system(size: 60)).foregroundStyle(.secondary)
                    Text("Your Virtual Machines").font(.title)
                    Text("Add a disk or an existing PCEmu snapshot to get started.").foregroundStyle(.secondary)
                    Button("New Virtual Machine…") { library.add() }.buttonStyle(.borderedProminent)
                }.frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .sheet(isPresented: Binding(get: { editing != nil }, set: { if !$0 { editing = nil } })) {
            if let id = editing, let index = library.machines.firstIndex(where: { $0.id == id }) {
                VMSettings(vm: library.machines[index]) { vm in
                    library.machines[index] = vm; library.save(); editing = nil
                }
            }
        }
        .alert("PCEmu", isPresented: Binding(get: { library.error != nil }, set: { if !$0 { library.error = nil } })) {
            Button("OK") { library.error = nil }
        } message: { Text(library.error ?? "") }
        .confirmationDialog("Remove \(deleting?.name ?? "VM") from the library?", isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } })) {
            Button("Remove from Library", role: .destructive) { if let vm = deleting { library.remove(vm) }; deleting = nil }
        } message: { Text("Disk images and snapshots will remain on disk.") }
        .confirmationDialog("Reboot the running VM?", isPresented: $library.wantsReboot) {
            Button("Reboot", role: .destructive) { library.command("reboot"); library.paused = false }
        } message: { Text("Unsaved guest work will be lost.") }
    }
    @ViewBuilder func vmActions(_ vm: VirtualMachine) -> some View {
        if library.active == vm.id {
            Button(library.paused ? "Resume" : "Pause") { library.togglePause() }
            Button("Suspend and Save") { library.suspend() }
            Button("Reboot…") { library.wantsReboot = true }
        } else if library.active != vm.id {
            Button("Open") { library.start(vm) }.disabled(library.active != nil)
            Button("Cold Boot") { library.start(vm, cold: true) }.disabled(library.active != nil || vm.disk.isEmpty)
            Button("Settings…") { editing = vm.id }
        }
        Divider()
        if !vm.disk.isEmpty { Button("Reveal Disk in Finder") { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: vm.disk)]) } }
        if !vm.snapshot.isEmpty { Button("Reveal Snapshot in Finder") { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: vm.snapshot)]) } }
        Button("Remove from Library…", role: .destructive) { deleting = vm }.disabled(library.active == vm.id)
    }
}

struct VMSettings: View {
    @Environment(\.dismiss) private var dismiss
    @State var vm: VirtualMachine
    let save: (VirtualMachine) -> Void
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Virtual Machine Settings").font(.title2.bold())
            Form {
                TextField("Name", text: $vm.name)
                Picker("Machine", selection: $vm.machine) {
                    Text("Legacy PC").tag("pc-legacy")
                    Text("Windows 3.11").tag("win311")
                    Text("Windows 95 / Office 97").tag("win95")
                    if vm.isARM { Text("Windows 11 ARM64").tag("arm64-uefi") }
                }.disabled(vm.isARM)
                Stepper("Memory: \(vm.memory) MB", value: $vm.memory, in: vm.isARM ? 256...16384 : 2...64, step: vm.isARM ? 256 : 1)
                if vm.isARM {
                    Stepper("Virtual CPUs: \(vm.cpuCount)", value: Binding(get: { vm.cpuCount }, set: { vm.processors = $0 }), in: 1...8)
                } else { LabeledContent("Virtual CPUs", value: "1 (x86 engine limit)") }
                Text("Memory applies on cold boot. A restored snapshot includes its original RAM configuration.")
                    .font(.caption).foregroundStyle(.secondary)
                HStack { TextField("Boot disk", text: $vm.disk); Button("Choose…") { choose(directory: false) { vm.disk = $0 } } }
                HStack { TextField("Snapshot", text: $vm.snapshot); Button("Choose…") { choose(directory: true) { vm.snapshot = $0 } } }
                Toggle("Restore saved snapshot by default", isOn: $vm.restoreOnOpen)
                Text("Cold boot writes to the selected disk. Snapshot restore creates a private writable disk clone. Suspend saves a new checkpoint; previous checkpoints remain available in Finder.")
                    .font(.caption).foregroundStyle(.secondary)
            }.formStyle(.grouped)
            HStack {
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Save") { save(vm) }.keyboardShortcut(.defaultAction)
                    .disabled(vm.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || vm.disk.contains("\n") || vm.snapshot.contains("\n"))
            }
        }.padding(24).frame(width: 590)
    }
    func choose(directory: Bool, completion: @escaping (String) -> Void) {
        let panel = NSOpenPanel(); panel.canChooseDirectories = directory; panel.canChooseFiles = !directory
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url { completion(url.path) }
    }
}
