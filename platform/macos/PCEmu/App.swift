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
                    .keyboardShortcut("p", modifiers: [.command, .shift]).disabled(library.active == nil || library.suspending || library.takingSnapshot)
                Button("Take Snapshot") { library.takeSnapshot() }
                    .keyboardShortcut("s", modifiers: [.command, .option])
                    .disabled(library.active == nil || library.suspending || library.takingSnapshot)
                Button("Suspend and Save") { library.suspend() }
                    .keyboardShortcut("s", modifiers: [.command, .shift]).disabled(library.active == nil || library.suspending || library.takingSnapshot)
                Button("Reboot…") { library.wantsReboot = true }
                    .disabled(library.active == nil || library.suspending || library.takingSnapshot)
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
    @State private var showingSnapshots = false
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
                                Text(library.active == vm.id ? (library.suspending || library.takingSnapshot ? "Saving state…" : library.paused ? "Paused" : "Running") : vm.snapshot.isEmpty ? "Powered off" : "Saved state")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            if library.active == vm.id { Circle().fill(library.paused ? .orange : .green).frame(width: 7, height: 7) }
                        }
                        .padding(.vertical, 5).tag(vm.id)
                        .contextMenu { vmActions(vm).disabled(library.suspending || library.takingSnapshot) }
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
                            Text(library.switchingTo.map { "Saving before opening \($0)…" } ?? (library.suspending || library.takingSnapshot ? "Saving snapshot…" : library.paused ? "Paused" : "Running"))
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
                            Button(library.active != nil ? "Save Current VM and Open \(vm.name)" : vm.restoreOnOpen && !vm.snapshot.isEmpty ? "Restore Virtual Machine" : "Start Virtual Machine") { library.start(vm) }
                                .buttonStyle(.borderedProminent)
                            Button("Settings…") { editing = vm.id }
                            if library.active != nil { Text("The current VM will be saved automatically before switching.").font(.caption).foregroundStyle(.secondary) }
                        }.frame(maxWidth: .infinity, maxHeight: .infinity)
                            .contextMenu { vmActions(vm).disabled(library.suspending || library.takingSnapshot) }
                    }
                }
                .toolbar {
                    if library.active == vm.id {
                        Button { library.togglePause() } label: { Label(library.paused ? "Resume" : "Pause", systemImage: library.paused ? "play.fill" : "pause.fill") }
                        Button { library.takeSnapshot() } label: { Label("Take Snapshot", systemImage: "camera") }
                        Button { library.suspend() } label: { Label("Suspend", systemImage: "moon.zzz") }
                        Button { library.wantsReboot = true } label: { Label("Reboot", systemImage: "arrow.clockwise") }
                    } else if library.active != vm.id {
                        Button { library.start(vm) } label: { Label(library.active == nil ? "Start" : "Switch to VM", systemImage: "play.fill") }
                    }
                    Button { showingSnapshots = true } label: { Label("Snapshots", systemImage: "clock.arrow.circlepath") }
                    Button { editing = vm.id } label: { Label("Settings", systemImage: "slider.horizontal.3") }
                }
                .disabled(library.suspending || library.takingSnapshot)
            } else {
                VStack(spacing: 16) {
                    Image(systemName: "square.stack.3d.up").font(.system(size: 60)).foregroundStyle(.secondary)
                    Text("Your Virtual Machines").font(.title)
                    Text("Add a disk or an existing PCEmu snapshot to get started.").foregroundStyle(.secondary)
                    Button("New Virtual Machine…") { library.add() }.buttonStyle(.borderedProminent)
                }.frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .onChange(of: library.selection) { _ in library.releaseInput() }
        .sheet(isPresented: Binding(get: { editing != nil }, set: { if !$0 { editing = nil } })) {
            if let id = editing, let index = library.machines.firstIndex(where: { $0.id == id }) {
                VMSettings(vm: library.machines[index], running: library.active == id) { vm in
                    library.machines[index] = vm; library.save(); editing = nil
                }
            }
        }
        .sheet(isPresented: $showingSnapshots) {
            if let vm = selected { SnapshotBrowser(library: library, vm: vm) }
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
            Button("Take Snapshot") { library.takeSnapshot() }
            Button("Suspend and Save") { library.suspend() }
            Button("Reboot…") { library.wantsReboot = true }
        } else if library.active != vm.id {
            Button(library.active == nil ? "Open" : "Save Current VM and Switch") { library.start(vm) }
            Button("Cold Boot") { library.start(vm, cold: true) }.disabled(vm.disk.isEmpty)
            Button("Settings…") { editing = vm.id }
        }
        Button("Snapshots…") { library.selection = vm.id; showingSnapshots = true }
        if library.active == vm.id { Button("Settings…") { editing = vm.id } }
        Divider()
        if !vm.disk.isEmpty { Button("Reveal Disk in Finder") { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: vm.disk)]) } }
        if !vm.snapshot.isEmpty { Button("Reveal Snapshot in Finder") { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: vm.snapshot)]) } }
        Button("Remove from Library…", role: .destructive) { deleting = vm }.disabled(library.active == vm.id)
    }
}

struct VMSettings: View {
    @Environment(\.dismiss) private var dismiss
    @State var vm: VirtualMachine
    var running = false
    let save: (VirtualMachine) -> Void
    @State private var original: VirtualMachine?
    var hardwareChanged: Bool { original.map { $0.memory != vm.memory || $0.cpuCount != vm.cpuCount || $0.machine != vm.machine || $0.disk != vm.disk } ?? false }
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
                LabeledContent("Memory (MB)") {
                    TextField("MB", value: $vm.memory, format: .number.grouping(.never)).frame(width: 90).multilineTextAlignment(.trailing)
                }
                Stepper("Memory: \(vm.memory) MB", value: $vm.memory, in: vm.isARM ? 256...16384 : 2...64, step: vm.isARM ? 256 : 1)
                if vm.isARM {
                    Stepper("Virtual CPUs: \(vm.cpuCount)", value: Binding(get: { vm.cpuCount }, set: { vm.processors = $0 }), in: 1...8)
                } else { LabeledContent("Virtual CPUs", value: "1 (x86 engine limit)") }
                Text(hardwareChanged ? "These changes require a cold boot. Saving will turn off automatic snapshot restore; your snapshots remain available." : running ? "Changes apply on the next cold boot. The running VM keeps its current configuration." : "Restoring a snapshot uses the memory and CPUs saved in that snapshot.")
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
                Button(hardwareChanged ? "Save for Cold Boot" : "Save") {
                    var updated = vm
                    if hardwareChanged { updated.restoreOnOpen = false }
                    save(updated)
                }.keyboardShortcut(.defaultAction)
                    .disabled(!(vm.isARM ? 256...16384 : 2...64).contains(vm.memory) || !(1...8).contains(vm.cpuCount) || vm.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || vm.disk.contains("\n") || vm.snapshot.contains("\n"))
            }
        }.padding(24).frame(width: 590)
            .onAppear { original = vm }
    }
    func choose(directory: Bool, completion: @escaping (String) -> Void) {
        let panel = NSOpenPanel(); panel.canChooseDirectories = directory; panel.canChooseFiles = !directory
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url { completion(url.path) }
    }
}


struct SnapshotBrowser: View {
    @ObservedObject var library: Library
    let vm: VirtualMachine
    @Environment(\.dismiss) private var dismiss
    var snapshots: [VMSnapshot] { library.machines.first { $0.id == vm.id }?.snapshots ?? [] }
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Snapshots · \(vm.name)").font(.title2.bold())
            Text("Restore a saved moment, or take a snapshot while the VM keeps running.")
                .foregroundStyle(.secondary)
            List {
                if !vm.snapshot.isEmpty && !snapshots.contains(where: { $0.path == vm.snapshot }) {
                    snapshotRow(path: vm.snapshot, title: "Previously selected snapshot", detail: "Imported checkpoint")
                }
                ForEach(snapshots) { snapshot in
                    snapshotRow(path: snapshot.path,
                                title: snapshot.date.formatted(date: .abbreviated, time: .standard),
                                detail: "\(snapshot.memory) MB · \(snapshot.processors) vCPU")
                }
            }.overlay {
                if snapshots.isEmpty && vm.snapshot.isEmpty { Text("No snapshots yet").foregroundStyle(.secondary) }
            }
            if library.active == vm.id { Text("Restoring saves the current state first, then opens the selected snapshot.").font(.caption).foregroundStyle(.secondary) }
            HStack {
                Button("Take Snapshot") { library.takeSnapshot() }
                    .disabled(library.active != vm.id || library.suspending || library.takingSnapshot)
                if library.takingSnapshot { ProgressView().controlSize(.small); Text("Saving…").font(.caption) }
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.cancelAction)
            }
        }.padding(24).frame(width: 620, height: 430)
    }
    func snapshotRow(path: String, title: String, detail: String) -> some View {
        HStack {
            Image(systemName: "clock.arrow.circlepath")
            VStack(alignment: .leading) { Text(title); Text(detail).font(.caption).foregroundStyle(.secondary) }
            Spacer()
            Button("Restore") { library.restore(vm, snapshot: path); dismiss() }
                .disabled(library.suspending || library.takingSnapshot)
        }.padding(.vertical, 4).contextMenu {
            Button("Reveal in Finder") { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)]) }
        }
    }
}
