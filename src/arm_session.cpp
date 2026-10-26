#include "arm_session.hpp"
#include <filesystem>
#include <thread>
#include <sstream>
#include <iostream>
namespace pc {
// Toolkit-independent lifecycle; no callbacks or locks are added to vCPU run.
int runArmSession(ArmSessionOptions options, FrontendFactory factory, SessionControl *control) {
    namespace fs=std::filesystem;
    std::unique_ptr<Frontend> frontend;
    bool stop=false, reboot=false, paused=options.paused;
    std::string ownedRun;
    struct Cleanup { std::string &path; ~Cleanup() { if (!path.empty()) { std::error_code error; fs::remove_all(path,error); } } } cleanup{ownedRun};
    try {
        if (!options.snapshot.empty()) {
            if (options.runDirectory.empty()) throw std::runtime_error("ARM snapshot restore needs a private run directory");
            auto audioSink=options.machine.audioSink;
            options.machine=ArmVirtMachine::snapshotConfig(options.snapshot,options.runDirectory);
            options.machine.audioSink=std::move(audioSink);
            ownedRun=options.runDirectory;
        }
        if (factory) {
            frontend=factory(800,600,DisplayShape::SquarePixels);
            if (!frontend) throw std::runtime_error("ARM frontend factory returned no frontend");
        }
        uint64_t slices=0;
        do {
            reboot=false;
            ArmVirtMachine vm(options.machine);
            if (!options.snapshot.empty()) { vm.restoreSnapshot(options.snapshot); options.snapshot.clear(); }
            vm.pauseClock(paused);
            FrontendInput input(frontend.get(),vm.guestInput());
            input.setAbsolutePointer(true);
            std::vector<uint32_t> pixels;
            auto nextFrame=std::chrono::steady_clock::now();
            bool redraw=true;
            while (!stop && !reboot && !vm.stopped() && (!options.maximumSlices || slices<options.maximumSlices)) {
                if (control) for (const auto &line : control->takeCommands()) {
                    std::istringstream command(line); std::string op; command>>op;
                    if (op=="pause" || op=="resume") {
                        input.release(); paused=op=="pause"; vm.pauseClock(paused);
                    } else if (op=="reboot") { input.release(); reboot=true; paused=false; }
                    else if (op=="quit") stop=true;
                    else if (op=="close") {
                        if (!vm.requestGuestShutdown()) throw std::runtime_error("Guest shutdown channel is not ready");
                        vm.pauseClock(false); paused=false;
                    } else if (op=="save-snapshot" || op=="suspend") {
                        std::string path; std::getline(command>>std::ws,path);
                        try {
                            if (path.empty()) throw std::runtime_error("Snapshot directory is required");
                            input.release();
                            vm.saveSnapshot(path);
                            if (op=="suspend") stop=true;
                        } catch (const std::exception &error) {
                            if (control) control->reportError(error.what());
                            if (frontend) frontend->reportError(error.what());
                        }
                    } else if (!op.empty()) throw std::runtime_error("Unknown ARM session command: "+op);
                }
                if (stop || reboot) break;
                auto now=std::chrono::steady_clock::now();
                if (frontend && now>=nextFrame) {
                    nextFrame=now+std::chrono::milliseconds(16);
                    for (const auto &event : frontend->poll()) {
                        if (event.kind==FrontendEvent::Kind::Redraw) redraw=true;
                        if (!input.event(event,!paused)) stop=true;
                    }
                    unsigned width=0,height=0;
                    bool changed=vm.display(pixels,width,height,false);
                    frontend->setPointerShape(vm.pointerShape());
                    if (width && height && (changed || redraw)) {
                        frontend->present(pixels,width,height); redraw=false;
                    }
                }
                if (!paused && !stop) { vm.run(std::chrono::milliseconds(frontend ? 16 : 100)); ++slices; }
                else std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            input.release(); vm.saveVariables();
            if (vm.resetRequested() && !stop) { reboot=true; paused=false; }
            if (options.maximumSlices && slices>=options.maximumSlices) reboot=false;
            // Reboot opens the same private working disk/variables, never the
            // immutable checkpoint. Restored CPU state is consumed only once.
            if (reboot) options.machine.restoringSnapshot=false;
        } while (reboot && !stop);
        return 0;
    } catch (const std::exception &error) {
        if (control) control->reportError(error.what());
        if (frontend) frontend->reportError(error.what());
        std::cerr<<"ARM session: "<<error.what()<<'\n'; return 1;
    }
}
}
