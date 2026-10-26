// Host-only stopped-machine checkpoints. Architectural device state is enumerated
// in arm_snapshot_state.hpp; host threads, RAM spans, callbacks and sockets are
// reconstructed. Native GIC restore follows Apple's fresh-VM requirement.
// https://developer.apple.com/documentation/hypervisor/hv_gic_set_state(_:_:)
#include "arm_snapshot_state.hpp"
#include <filesystem>
#include <fstream>
#include <sys/clonefile.h>
#include <unistd.h>
#include <zstd.h>
namespace pc {
namespace {
namespace fs = std::filesystem;
constexpr size_t ChunkBytes = 4 * 1024 * 1024;
const std::string SchemaIdentity = PCEMU_ARM_SNAPSHOT_SCHEMA;
// Host format policy: bounded checksummed frames; never accept unknown sizes.
std::vector<uint8_t> pack(std::span<const uint8_t> bytes) {
    auto *context = ZSTD_createCCtx();
    if (!context) throw std::runtime_error("Cannot allocate snapshot compressor");
    ZSTD_CCtx_setParameter(context,ZSTD_c_compressionLevel,1);
    ZSTD_CCtx_setParameter(context,ZSTD_c_checksumFlag,1);
    std::vector<uint8_t> output(ZSTD_compressBound(bytes.size()));
    auto size = ZSTD_compress2(context,output.data(),output.size(),bytes.data(),bytes.size());
    ZSTD_freeCCtx(context);
    if (ZSTD_isError(size)) throw std::runtime_error("Snapshot compression failed");
    output.resize(size); return output;
}
// Validate the complete zstd frame/checksum and expected uncompressed extent.
std::vector<uint8_t> unpack(std::span<const uint8_t> bytes, size_t maximum) {
    auto size = ZSTD_getFrameContentSize(bytes.data(),bytes.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > maximum)
        throw std::runtime_error("Invalid compressed snapshot extent");
    std::vector<uint8_t> result(size);
    auto n = ZSTD_decompress(result.data(),result.size(),bytes.data(),bytes.size());
    if (ZSTD_isError(n) || n != size) throw std::runtime_error("Snapshot checksum/decompression failed");
    return result;
}
// Read bounded host metadata without touching a running VM or its disk.
std::vector<uint8_t> readFile(const fs::path &path, size_t maximum) {
    auto size = fs::file_size(path);
    if (size > maximum) throw std::runtime_error("Snapshot file exceeds limit: " + path.string());
    std::vector<uint8_t> result(size);
    std::ifstream in(path,std::ios::binary);
    if (!in.read(reinterpret_cast<char *>(result.data()),size)) throw std::runtime_error("Cannot read snapshot: " + path.string());
    return result;
}
// Fail on short writes; callers publish only after every file has closed.
void writeFile(const fs::path &path, std::span<const uint8_t> bytes) {
    std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size()); out.close();
    if (!out) throw std::runtime_error("Cannot write snapshot: " + path.string());
}
// APFS clones freeze a disk's contents without allocating another large image.
// Reject unsupported/cross-volume clones rather than silently copying gigabytes.
void cloneDisk(const fs::path &source, const fs::path &destination, bool writable) {
    if (::clonefile(source.c_str(),destination.c_str(),0) != 0)
        throw std::runtime_error("Snapshot disk requires an APFS clone on the same volume: " + source.string());
    fs::permissions(destination, writable ? fs::perms::owner_read|fs::perms::owner_write : fs::perms::owner_read,fs::perm_options::replace);
}
struct Manifest {
    ArmVirtMachine::Config config;
    uint64_t backingSize = 0;
    int64_t backingTime = 0;
};
// Schema fingerprints reject incompatible state inventories before constructing
// a VM. Native GIC also performs its own host-OS compatibility validation.
Manifest readManifest(const fs::path &directory) {
    auto bytes = unpack(readFile(directory/"state.bin",1024*1024),1024*1024);
    StateArchive archive(bytes);
    std::string magic, build; unsigned version = 0;
    Manifest value;
    archive(magic,version,build,value.config,value.backingSize,value.backingTime); archive.finish();
    if (magic != "PCEMU-ARM64" || version != 1 || build != SchemaIdentity)
        throw std::runtime_error("ARM snapshot schema is incompatible; use the matching engine or cold boot");
    if (!value.config.cpus || value.config.cpus > 8 || value.config.memoryBytes < 2*1024*1024 ||
        value.config.memoryBytes > size_t(16)*1024*1024*1024 || value.config.memoryBytes % 16384)
        throw std::runtime_error("Invalid ARM snapshot topology");
    if (!value.config.disk.empty() &&
        (fs::file_size(value.config.disk) != value.backingSize || fs::last_write_time(value.config.disk).time_since_epoch().count()!=value.backingTime))
        throw std::runtime_error("ARM snapshot immutable backing disk has changed");
    const auto local=fs::absolute(directory);
    value.config.firmware=(local/"firmware.bin").string();
    value.config.dtb=(local/"platform.dtb").string();
    value.config.variableTemplate=(local/"vars-template.bin").string();
    value.config.variables=(local/"variables.bin").string();
    value.config.acpiDirectory=(local/"acpi").string();
    if (!value.config.disk.empty()) value.config.diskOverlay=(local/"disk.qcow2").string();
    return value;
}
// Stream bounded independent frames so saving 4 GiB RAM needs only a few MiB
// of compression workspace, and zero/uncommitted pages compress cheaply.
void saveRam(const fs::path &path, std::span<uint8_t> ram) {
    std::ofstream out(path,std::ios::binary);
    for (size_t at=0; at<ram.size(); at+=ChunkBytes) {
        auto compressed=pack(ram.subspan(at,std::min(ChunkBytes,ram.size()-at)));
        uint32_t size=compressed.size(); out.write(reinterpret_cast<char *>(&size),sizeof(size));
        out.write(reinterpret_cast<const char *>(compressed.data()),compressed.size());
        if (!out) throw std::runtime_error("Cannot write snapshot RAM");
    }
    out.close(); if (!out) throw std::runtime_error("Cannot close snapshot RAM");
}
// Reject truncation, trailing bytes, bad checksums and incorrect RAM chunk sizes.
void restoreRam(const fs::path &path, std::span<uint8_t> ram) {
    std::ifstream in(path,std::ios::binary);
    for (size_t at=0; at<ram.size(); at+=ChunkBytes) {
        uint32_t size=0;
        if (!in.read(reinterpret_cast<char *>(&size),sizeof(size)) || size>ZSTD_compressBound(ChunkBytes))
            throw std::runtime_error("Invalid snapshot RAM chunk");
        std::vector<uint8_t> compressed(size);
        if (!in.read(reinterpret_cast<char *>(compressed.data()),size)) throw std::runtime_error("Truncated snapshot RAM");
        auto bytes=unpack(compressed,ChunkBytes);
        if (bytes.size()!=std::min(ChunkBytes,ram.size()-at)) throw std::runtime_error("Snapshot RAM extent mismatch");
        // A fresh VM has zero RAM beyond its initial device tree. Avoid
        // committing physical host pages just to restore large zero extents.
        if (at<ChunkBytes || !std::all_of(bytes.begin(),bytes.end(),[](uint8_t b) { return b==0; }))
            std::copy(bytes.begin(),bytes.end(),ram.begin()+at);
    }
    if (in.peek()!=std::char_traits<char>::eof()) throw std::runtime_error("Trailing snapshot RAM data");
}
}
// Freeze partition and architectural counters together, between run slices.
// Host policy supporting Hyper-V Reference Counter and Reference TSC Page.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#reference-counter
void ArmVirtMachine::pauseClock(bool paused) {
    if (paused==clockPaused) return;
    if (paused) { pausedReference=ArmSnapshotAccess::sampleReference(hyperv); vm.pauseClock(true); }
    else {
        ArmSnapshotAccess::setReference(hyperv,pausedReference,true);
        vm.pauseClock(false);
        audioServiceTime=std::chrono::steady_clock::now();
    }
    clockPaused=paused;
}
// Checkpoint host policy: quiesce, flush disks, clone, compress, then atomically
// publish a new directory. Failure never overwrites a prior checkpoint.
void ArmVirtMachine::saveSnapshot(const std::string &directory) {
    if (nvme || extraDisk || agentSocket || config.spiceAgent)
        throw std::runtime_error("ARM checkpoints currently require primary VirtIO storage and no external agent socket");
    bool wasPaused=clockPaused; pauseClock(true);
    const auto target=fs::absolute(directory);
    const auto temporary=fs::path(target.string()+".saving-"+std::to_string(getpid()));
    if (fs::exists(target) || fs::exists(temporary)) { if (!wasPaused) pauseClock(false); throw std::runtime_error("Snapshot destination already exists"); }
    try {
        fs::create_directories(temporary);
        fs::permissions(temporary,fs::perms::owner_all,fs::perm_options::replace);
        saveVariables();
        Manifest manifest{config};
        auto &saved=manifest.config;
        saved.initializationProgress={}; saved.initializationStage={}; saved.audioSink={};
        for (auto [source,name] : {std::pair{config.firmware,"firmware.bin"}, {config.dtb,"platform.dtb"}, {config.variableTemplate,"vars-template.bin"}})
            fs::copy_file(source,temporary/name);
        fs::create_directories(temporary/"acpi");
        for (auto name : {"tables","rsdp","table-loader"}) fs::copy_file(fs::path(config.acpiDirectory)/name,temporary/"acpi"/name);
        saved.firmware=(target/"firmware.bin").string(); saved.dtb=(target/"platform.dtb").string();
        saved.variableTemplate=(target/"vars-template.bin").string(); saved.acpiDirectory=(target/"acpi").string();
        saved.variables=(target/"variables.bin").string();
        writeFile(temporary/"variables.bin",nor.storage());
        if (disk) {
            // Relative QCOW backing names would change meaning after moving the
            // clone; require the absolute backing already used by run11.sh.
            auto parent=fs::canonical(config.disk);
            manifest.backingSize=fs::file_size(parent);
            manifest.backingTime=fs::last_write_time(parent).time_since_epoch().count();
            saved.disk=parent.string(); saved.diskOverlay=(target/"disk.qcow2").string();
            cloneDisk(disk->path,temporary/"disk.qcow2",false);
            Disk checkDisk((temporary/"disk.qcow2").string(),false,true);
            if (fs::canonical(checkDisk.backingPath())!=parent) throw std::runtime_error("Snapshot disk backing mismatch");
        }
        StateArchive devices;
        ArmSnapshotAccess::setReference(hyperv,pausedReference,false);
        devices(nor,fwCfg,uart,rtc,osLocks,hyperv,rtcInterruptLevel,powerAction);
        auto device=[&](auto &pointer) { bool exists=bool(pointer); devices(exists); if (exists) devices(*pointer); };
        device(block); device(pci); device(net); device(netPci); device(rng); device(rngPci);
        device(serial); device(serialPci); device(balloon); device(balloonPci); device(gpu); device(gpuPci); device(usb); device(hda);
        writeFile(temporary/"devices.zst",pack(devices.take()));
        writeFile(temporary/"cpu.zst",pack(vm.saveState()));
        saveRam(temporary/"ram.zst",vm.ram());
        StateArchive metadata; std::string magic="PCEMU-ARM64", build=SchemaIdentity; unsigned version=1;
        metadata(magic,version,build,saved,manifest.backingSize,manifest.backingTime);
        writeFile(temporary/"state.bin",pack(metadata.take()));
        fs::rename(temporary,target);
        if (!wasPaused) pauseClock(false);
    } catch (...) {
        std::error_code ignored; fs::remove_all(temporary,ignored);
        if (!wasPaused) pauseClock(false);
        throw;
    }
}
// Prepare a fresh private writable fork. Its owner removes the run directory
// after VM destruction. The saved checkpoint remains immutable and reusable.
ArmVirtMachine::Config ArmVirtMachine::snapshotConfig(const std::string &directory,const std::string &runDirectory) {
    auto manifest=readManifest(directory);
    auto config=manifest.config;
    const auto run=fs::absolute(runDirectory);
    if (fs::exists(run)) throw std::runtime_error("Snapshot run directory already exists");
    fs::create_directories(run); fs::permissions(run,fs::perms::owner_all,fs::perm_options::replace);
    try {
        fs::copy_file(config.variables,run/"variables.bin"); config.variables=(run/"variables.bin").string();
        if (!config.disk.empty()) { cloneDisk(config.diskOverlay,run/"disk.qcow2",true); config.diskOverlay=(run/"disk.qcow2").string(); }
        config.restoringSnapshot=true; return config;
    } catch (...) { std::error_code ignored; fs::remove_all(run,ignored); throw; }
}
// Restore only into a fresh machine, before any guest VP can execute. Host
// services reconnect with fresh callbacks/sockets, preserving all guest queues.
void ArmVirtMachine::restoreSnapshot(const std::string &directory) {
    if (!config.restoringSnapshot) throw std::runtime_error("ARM restore requires a fresh machine");
    auto manifest=readManifest(directory);
    if (manifest.config.cpus!=config.cpus || manifest.config.memoryBytes!=config.memoryBytes)
        throw std::runtime_error("ARM snapshot configuration mismatch");
    auto bytes=unpack(readFile(fs::path(directory)/"devices.zst",size_t(512)*1024*1024),size_t(512)*1024*1024);
    StateArchive devices(bytes);
    devices(nor,fwCfg,uart,rtc,osLocks,hyperv,rtcInterruptLevel,powerAction);
    auto device=[&](auto &pointer) { bool exists=false; devices(exists); if (exists!=bool(pointer)) throw std::runtime_error("Snapshot device inventory mismatch"); if (exists) devices(*pointer); };
    device(block); device(pci); device(net); device(netPci); device(rng); device(rngPci);
    device(serial); device(serialPci); device(balloon); device(balloonPci); device(gpu); device(gpuPci); device(usb); device(hda);
    devices.finish();
    restoreRam(fs::path(directory)/"ram.zst",vm.ram());
    vm.writeMemory(0x04000000,nor.bytes());
    auto cpu=unpack(readFile(fs::path(directory)/"cpu.zst",16*1024*1024),16*1024*1024);
    vm.restoreState(cpu);
    pausedReference=ArmSnapshotAccess::savedReference(hyperv);
    ArmSnapshotAccess::setReference(hyperv,pausedReference,true);
    clockPaused=true;
    ArmSnapshotAccess::invalidateFramebuffer(ramfb); displayWasGpu=false;
    audioServiceTime=std::chrono::steady_clock::now();
}
}
