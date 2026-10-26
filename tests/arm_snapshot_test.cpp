#include "arm_virt.hpp"
#include "arm_session.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <unistd.h>
using namespace pc;
static void check(bool value,const char *message) { if (!value) throw std::runtime_error(message); }
// Headless machine validation. --windows reuses the installed immutable base
// with one temporary compressed overlay; it never opens the user's live disk.
int main(int argc,char **argv) {
    namespace fs=std::filesystem;
    auto root=fs::current_path();
    bool windows=argc>1 && std::string(argv[1])=="--windows";
    auto work=root/"build"/("arm-snapshot-test-"+std::to_string(getpid()));
    try {
        fs::create_directories(work);
        ArmVirtMachine::Config config;
        config.cpus=2; config.memoryBytes=size_t(windows ? 4096 : 256)*1024*1024;
        const auto platform=root/("build/arm64-fw/cpus2-ram"+std::to_string(windows ? 4096 : 256)+"-hyperv-vmbus-msi");
        config.hyperv=config.hypervSynic=config.hypervVmbus=config.virtioMsix=true;
        config.network=config.virtioGpu=config.virtioSerial=config.virtioBalloon=config.balloonLegacyPfn64=config.intelHda=true;
        config.firmware=(root/"roms/edk2-arm64.bin").string();
        config.variableTemplate=(root/"roms/edk2-arm64-vars.bin").string();
        config.dtb=(platform/"pcemu-virt.dtb").string(); config.acpiDirectory=(platform/"acpi").string();
        config.variables=(work/"variables.bin").string();
        if (windows) {
            auto installed=root/"media/windows11-arm64/system-tools-live.qcow2";
            if (!fs::exists(installed)) installed=root/"media/windows11-arm64/system.qcow2";
            config.disk=installed.string();
            fs::copy_file(root/"build/windows11-qcow2/installed-vars.bin",config.variables);
        } else {
            const std::array<uint32_t,2> loop{0x91000421,0x17ffffff}; // ADD X1,X1,#1; B back.
            std::ofstream out(work/"firmware.bin",std::ios::binary);
            out.write(reinterpret_cast<const char *>(loop.data()),sizeof(loop)); out.close();
            config.firmware=(work/"firmware.bin").string();
            std::ofstream disk(work/"base.raw",std::ios::binary); std::string zero(4096,0); disk.write(zero.data(),zero.size());
            config.disk=(work/"base.raw").string();
        }
        config.diskOverlay=(work/"working.pcd").string();
        const auto snapshot=work/"checkpoint";
        std::string cpuBefore, hypervBefore;
        std::vector<uint32_t> before;
        unsigned width=0,height=0;
        uint64_t requests=0;
        {
            ArmVirtMachine vm(config);
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(windows ? 90 : 0);
            unsigned runs=0;
            const auto bootStart=std::chrono::steady_clock::now();
            auto nextDisplay=bootStart;
            do {
                vm.run(std::chrono::milliseconds(100)); ++runs;
                if (std::chrono::steady_clock::now()>=nextDisplay) {
                    vm.display(before,width,height,false);
                    nextDisplay=std::chrono::steady_clock::now()+std::chrono::seconds(1);
                    if (windows && width>=800 && height>=600 && std::chrono::steady_clock::now()-bootStart>=std::chrono::seconds(30) &&
                        vm.hypervReport().find("\"responsive\":true")!=std::string::npos) break;
                }
            } while (windows && !vm.stopped() && std::chrono::steady_clock::now()<deadline);
            check(!vm.stopped(),"Guest stopped before checkpoint");
            if (windows) {
                std::cout<<"Boot: slices="<<runs<<" requests="<<vm.diskRequests()<<" display="<<width<<'x'<<height<<" state="<<vm.cpuState()<<std::endl;
                std::ofstream(work/"serial.txt")<<vm.output();
                std::ofstream(work/"hyperv-before.json")<<vm.hypervReport();
                if (width && height) vm.saveFramebuffer((work/"boot.ppm").string());
                check(width>=800 && height>=600 && vm.diskRequests()>100,"Windows display/disk did not initialize");
            }
            vm.guestInput().release(); vm.pauseClock(true);
            cpuBefore=vm.cpuState(); hypervBefore=vm.hypervReport(); requests=vm.diskRequests();
            vm.display(before,width,height,false);
            if (windows) vm.saveFramebuffer((work/"before.ppm").string());
            vm.saveSnapshot(snapshot.string());
            bool rejected=false;
            try { vm.saveSnapshot(snapshot.string()); } catch (...) { rejected=true; }
            check(rejected && fs::exists(snapshot/"state.bin"),"Existing checkpoint was not preserved");
            std::cout<<"Saved checkpoint after "<<runs<<" slices, "<<requests<<" disk requests; framebuffer "<<width<<'x'<<height<<std::endl;
        }
        auto restoredConfig=ArmVirtMachine::snapshotConfig(snapshot.string(),(work/"restored").string());
        {
            ArmVirtMachine vm(restoredConfig); vm.restoreSnapshot(snapshot.string());
            check(vm.cpuState()==cpuBefore,"CPU/device diagnostics changed across restore");
            check(vm.diskRequests()==requests,"Storage queue diagnostics changed across restore");
            std::vector<uint32_t> after; unsigned w=0,h=0;
            vm.display(after,w,h,false);
            check(w==width && h==height && after==before,"Framebuffer was not restored exactly");
            if (windows) vm.saveFramebuffer((work/"restored.ppm").string());
            vm.pauseClock(false);
            auto until=std::chrono::steady_clock::now()+std::chrono::seconds(windows ? 30 : 0);
            do { vm.run(std::chrono::milliseconds(100)); } while (windows && !vm.stopped() && std::chrono::steady_clock::now()<until);
            check(!vm.stopped(),"Restored guest stopped unexpectedly");
            if (windows) {
                vm.saveFramebuffer((work/"resumed.ppm").string());
                vm.guestInput().key(40,false); vm.guestInput().key(40,true);
                const auto inputDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while (!vm.stopped() && std::chrono::steady_clock::now()<inputDeadline) vm.run(std::chrono::milliseconds(100));
                vm.saveFramebuffer((work/"input.ppm").string());
                std::ofstream(work/"usb-after.txt")<<vm.usbReport();
                const auto afterReport=vm.hypervReport();
                std::ofstream(work/"hyperv-after.json")<<afterReport;
                check(afterReport.find("\"responsive\":true")!=std::string::npos,"Restored Windows heartbeat is not responsive");
                check(afterReport.find("\"channelFaults\":0")!=std::string::npos,"Restored Windows VMBus channel fault");
                // Checkpoint again after resumed device/ring activity, then verify
                // another fresh VM can consume that checkpoint as well.
                vm.guestInput().release(); vm.pauseClock(true);
                vm.saveSnapshot((work/"checkpoint-resumed").string());
            }
        }
        if (!windows) {
            struct Commands final : SessionControl {
                unsigned polls=0;
                std::string destination, failure;
                std::vector<std::string> takeCommands() override {
                    switch (++polls) {
                    case 1: return {"pause"};
                    case 2: return {"resume"};
                    case 3: return {"reboot"};
                    case 4: return {"pause"};
                    case 5: return {"suspend "+destination};
                    default: return {"quit"};
                    }
                }
                void reportError(std::string_view message) override { failure=message; }
            } commands;
            commands.destination=(work/"session-checkpoint").string();
            ArmSessionOptions options; options.machine=config;
            check(runArmSession(options,{},&commands)==0 && commands.polls==5 && commands.failure.empty(),
                  "ARM session pause/reboot/suspend failed");
            check(fs::exists(fs::path(commands.destination)/"state.bin"),"ARM session did not save checkpoint");
            struct Quit final : SessionControl {
                std::vector<std::string> takeCommands() override { return {"quit"}; }
            } quit;
            options.snapshot=commands.destination; options.runDirectory=(work/"session-restore").string();
            check(runArmSession(options,{},&quit)==0,"ARM session restore failed");
            check(!fs::exists(options.runDirectory),"Private session run was not cleaned");
        }
        if (windows) {
            struct Screen { std::vector<uint32_t> pixels; unsigned width=0,height=0; } screen;
            struct Display final : Frontend {
                Screen &screen;
                explicit Display(Screen &s) : screen(s) {}
                std::vector<FrontendEvent> poll() override { return {}; }
                void present(std::span<const uint32_t> pixels,unsigned w,unsigned h,DisplayShape) override {
                    screen.pixels.assign(pixels.begin(),pixels.end()); screen.width=w; screen.height=h;
                }
                bool capturePointer(bool) override { return true; }
            };
            struct Reboot final : SessionControl {
                bool requested=false;
                std::string failure;
                std::chrono::steady_clock::time_point until=std::chrono::steady_clock::now()+std::chrono::seconds(60);
                std::vector<std::string> takeCommands() override {
                    if (!requested) { requested=true; return {"reboot"}; }
                    if (std::chrono::steady_clock::now()>=until) return {"quit"};
                    return {};
                }
                void reportError(std::string_view error) override { failure=error; }
            } reboot;
            ArmSessionOptions options; options.snapshot=(work/"checkpoint-resumed").string();
            options.runDirectory=(work/"reboot-run").string();
            check(runArmSession(options,[&](unsigned,unsigned,DisplayShape) { return std::make_unique<Display>(screen); },&reboot)==0 && reboot.failure.empty(),
                  "Shared ARM session reboot failed");
            check(screen.width==width && screen.height==height && !screen.pixels.empty(),"Rebooted Windows did not return to its display mode");
            std::ofstream out(work/"rebooted.ppm",std::ios::binary);
            out<<"P6\n"<<screen.width<<' '<<screen.height<<"\n255\n";
            for (auto pixel : screen.pixels) { const char rgb[]{char(pixel>>16),char(pixel>>8),char(pixel)}; out.write(rgb,3); }
            check(!fs::exists(options.runDirectory),"Reboot session run was not cleaned");
        }
        // A truncated checksummed RAM stream must fail without touching the saved disk.
        if (!windows) {
            fs::resize_file(snapshot/"ram.zst",3);
            bool rejected=false;
            auto broken=ArmVirtMachine::snapshotConfig(snapshot.string(),(work/"broken").string());
            try { ArmVirtMachine vm(broken); vm.restoreSnapshot(snapshot.string()); } catch (...) { rejected=true; }
            check(rejected,"Truncated checkpoint was accepted");
        }
        if (windows) {
            for (const auto &path : {snapshot,work/"checkpoint-resumed",work/"restored"}) fs::remove_all(path);
            for (const auto &entry : fs::directory_iterator(work)) {
                const auto name=entry.path().filename().string();
                if (name.starts_with("working.pcd") || name.starts_with("variables.bin")) fs::remove(entry.path());
            }
            std::cout<<"Windows snapshot evidence: "<<work<<'\n';
        }
        else fs::remove_all(work);
        std::cout<<"ARM full-device, RAM, disk checkpoint/restore passed\n"; return 0;
    } catch(const std::exception &error) { std::cerr<<error.what()<<"; evidence: "<<work<<'\n'; return 1; }
}
