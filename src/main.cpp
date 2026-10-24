#include "host_keyboard.hpp"
#include "jit.hpp"
#include "machine.hpp"
#include "machine_profile.hpp"
#include "network.hpp"
#include "profile.hpp"
#include "frontend.hpp"
#include "pc_display.hpp"
#include "session_diagnostics.hpp"
#include "cpu_benchmarks.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <zstd.h>
using namespace pc;
static volatile std::sig_atomic_t captureRequested = 0;
static void requestCapture(int) {
    captureRequested = 1;
}
[[gnu::noinline]] static void waitForProfiler(unsigned milliseconds) {
    // Give external symbol collection time to finish without advancing the
    // guest. This named stack lets analysis exclude the attachment barrier.
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
static std::vector<uint8_t> readFile(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot read " + p);
    return {std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char **argv) {
    std::string hd, fd, rom = "roms/vgabios.bin", type, waitText, watchRange, controlPath;
    size_t ramMB = 16;
    bool ramExplicit = false, noJit = false, listMachines = false, followJumps = true, traceJit = false;
    std::string machineId = "pc-legacy", backendId = "auto";
    bool headless = false, fromFloppy = false, enableJit = true, verbose = false, benchmark = false,
         executeVga = false, memoryBenchmark = false, enableNetwork = true, enableHostInput = true;
    uint64_t limit = 100000000, benchmarkSteps = 10000000;
    std::string tracePath, dumpPath, framePath, untilText, traceTailPath;
    std::string snapshotPath;
    std::string stateJsonPath;
    uint64_t captureEvery = 0;
    uint64_t breakAt = 0x100000000ull, afterBreak = 0;
    try {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            auto value = [&]() {
                if (i + 1 >= argc)
                    throw std::runtime_error("Missing value for " + arg);
                return std::string(argv[++i]);
            };
            if (arg == "--machine")
                machineId = value();
            else if (arg == "--backend")
                backendId = value();
            else if (arg == "--list-machines")
                listMachines = true;
            else if (arg == "--no-host-input")
                enableHostInput = false;
            else if (arg == "--no-network")
                enableNetwork = false;
            else if (arg == "--execute-vgabios")
                executeVga = true;
            else if (arg == "--load-snapshot")
                snapshotPath = value();
            else if (arg == "--state-json")
                stateJsonPath = value();
            else if (arg == "--control-file")
                controlPath = value();
            else if (arg == "--watch-write")
                watchRange = value();
            else if (arg == "--after-break")
                afterBreak = std::stoull(value());
            else if (arg == "--break-at")
                breakAt = std::stoull(value(), nullptr, 0);
            else if (arg == "--capture-every")
                captureEvery = std::stoull(value());
            else if (arg == "--trace-tail")
                traceTailPath = value();
            else if (arg == "--ram-mb") {
                ramMB = std::stoul(value());
                ramExplicit = true;
            } else if (arg == "--wait-text")
                waitText = value();
            else if (arg == "--disk")
                hd = value();
            else if (arg == "--floppy")
                fd = value();
            else if (arg == "--boot-floppy")
                fromFloppy = true;
            else if (arg == "--headless")
                headless = true;
            else if (arg == "--jit-basic-blocks") {
                followJumps = traceJit = false;
            } else if (arg == "--jit-follow-jumps") {
                followJumps = true;
                traceJit = false;
            } else if (arg == "--jit-traces") {
                followJumps = traceJit = true;
            }
            else if (arg == "--no-jit")
                noJit = true;
            else if (arg == "--verbose")
                verbose = true;
            else if (arg == "--steps")
                limit = std::stoull(value());
            else if (arg == "--type")
                type = value();
            else if (arg == "--until-text")
                untilText = value();
            else if (arg == "--frame")
                framePath = value();
            else if (arg == "--dump-memory")
                dumpPath = value();
            else if (arg == "--trace")
                tracePath = value();
            else if (arg == "--vgabios")
                rom = value();
            else if (arg == "--benchmark-steps")
                benchmarkSteps = std::stoull(value());
            else if (arg == "--benchmark")
                benchmark = true;
            else if (arg == "--benchmark-memory")
                memoryBenchmark = true;
            else if (arg == "--help") {
                std::cout << "pcemu (Apple Silicon)\n  --disk FILE --floppy FILE [--boot-floppy]\n "
                             " --headless --steps N --type TEXT --trace FILE --verbose --no-jit\n  "
                             "--frame FILE.ppm --dump-memory FILE --until-text TEXT\n"
                             "  --break-at LINEAR [--after-break INSTRUCTIONS]\n"
                             "  --benchmark | --benchmark-memory [--benchmark-steps N]\n"
                             "    --backend jit|interpreter selects one benchmark engine\n"
                             "  --machine ID --list-machines --backend auto|interpreter|jit\n"
                             "  --jit-traces (experimental conditional branch traces)\n"
                             "  --jit-basic-blocks (disable default same-page jump joining)\n"
                             "  --ram-mb N (override profile default; snapshot RAM is restored)\n"
                             "  --load-snapshot DIRECTORY (private writable disk clone)\n"
                             "  --no-network (disconnect host socket gateway)\n"
                             "  --no-host-input (automated GUI runs; control input remains active)\n"
                             "  --state-json FILE (capture metadata without a RAM dump)\n"
                             "Click to capture mouse; Cmd+G releases/captures it. Cmd+Q: quit.\n";
                return 0;
            } else
                throw std::runtime_error("Unknown option: " + arg);
        }
        if (listMachines) {
            for (const auto &p : machineProfiles)
                std::cout << p.id << " [" << (p.implemented ? "available" : "planned")
                          << "] arch=" << p.architecture << " platform=" << p.platform
                          << " runner=" << p.runner << " ram=" << p.defaultRamMB << " MiB: " << p.description << '\n';
            return 0;
        }
        const auto &selectedProfile = machineProfile(machineId);
        auto backend = executionBackend(backendId);
        if (noJit) {
            if (backend != ExecutionBackend::Auto && backend != ExecutionBackend::Interpreter)
                throw std::runtime_error("--no-jit conflicts with --backend " + backendId);
            backend = ExecutionBackend::Interpreter;
        }
        enableJit = useJit(selectedProfile, backend);
        if (!ramExplicit)
            ramMB = selectedProfile.defaultRamMB;
        if (memoryBenchmark) {
            for (bool paging : {false, true}) {
                if (backend != ExecutionBackend::Auto)
                    benchmarkCpuMemory(enableJit, benchmarkSteps, paging);
                else {
                    auto ref = benchmarkCpuMemory(false, benchmarkSteps, paging);
                    auto native = benchmarkCpuMemory(true, benchmarkSteps, paging);
                    std::cout << "Memory speedup: " << ref / native << "x\n";
                }
            }
            return 0;
        }
        if (benchmark) {
            if (backend != ExecutionBackend::Auto)
                benchmarkCpuRegisters(enableJit, benchmarkSteps);
            else {
                double ref = benchmarkCpuRegisters(false, benchmarkSteps), jit = benchmarkCpuRegisters(true, benchmarkSteps);
                std::cout << "Speedup: " << ref / jit << "x (register-only synthetic workload)\n";
            }
            return 0;
        }
        if (ramMB < 2 || ramMB > 64)
            throw std::runtime_error("RAM must be 2..64 MiB");
        const auto stepBudget = limit;
        if (!snapshotPath.empty() && (!hd.empty() || !fd.empty() || fromFloppy))
            throw std::runtime_error("Snapshot restore supplies its own disk images");
        Machine m(hd, fd, ramMB * 1024 * 1024);
        if (!snapshotPath.empty()) {
            m.loadSnapshot(snapshotPath);
            executeVga = m.vgaOnReset;
            if (limit)
                limit += m.cpu.instructions;
            std::cerr << "Snapshot restored: " << snapshotPath
                      << " disk=" << (m.disk ? m.disk->path : "none") << '\n';
        }
        m.verbose = verbose;
        if (!watchRange.empty()) {
            auto colon = watchRange.find(':');
            m.cpu.watchStart = std::stoul(watchRange.substr(0, colon), nullptr, 0);
            m.cpu.watchLength = std::stoul(watchRange.substr(colon + 1), nullptr, 0);
            m.cpu.watchWrite = [&](uint32_t a, uint32_t v, int w) {
                std::cerr << "WRITE " << m.cpu.instructions << ' ' << m.cpu.location()
                          << " address=" << std::hex << a << " value=" << v << " width=" << w
                          << std::dec << '\n';
            };
        }

        auto &c = m.cpu;
        // VGA ROM is an allowed external asset; use its IBM-compatible text font.
        PcDisplay display;
        if (snapshotPath.empty() && !rom.empty()) {
            auto bytes = readFile(rom);
            for (size_t i = 0; i < bytes.size() && 0xc0000 + i < c.mem.size(); i++)
                c.mem[0xc0000 + i] = bytes[i];
            m.vgaRom = std::move(bytes);
        }
        display.loadFont(m.vgaRom);
        if (snapshotPath.empty() && executeVga) {
            m.initializeVga();
            std::cerr << "VGA ROM initialized: INT10=" << std::hex << c.read(0x40, 4) << std::dec
                      << " instructions=" << c.instructions << '\n';
        }
        if (snapshotPath.empty())
            m.boot(fromFloppy);
        Jit jit(c, enableJit, 8, 16 * 1024 * 1024,
                traceJit ? Jit::BlockMode::Trace
                         : followJumps ? Jit::BlockMode::FollowJumps : Jit::BlockMode::Basic);
        std::ofstream trace;
        if (!tracePath.empty() && !afterBreak)
            trace.open(tracePath);
        unsigned displayWidth = m.vga.graphics ? m.vga.width : 640;
        unsigned displayHeight = m.vga.graphics ? m.vga.height : 400;
        std::vector<uint32_t> pixels(size_t(displayWidth) * displayHeight);
        std::unique_ptr<Frontend> frontend;
        if (!headless) frontend = createFrontend(displayWidth, displayHeight);
        struct TraceSample {
            uint32_t ip, base, flags, cr0, cr3, r[8];
            uint16_t cs;
            uint64_t count;
        };
        std::array<TraceSample, 32768> history{};
        uint64_t historyCount = 0;
        std::ofstream faultTrace;
        uint64_t executionWallNs = 0;
        auto capture = [&]() {
            if (!dumpPath.empty()) {
                std::ofstream f(dumpPath, std::ios::binary);
                f.write(reinterpret_cast<const char *>(c.mem.data()), c.mem.size());
            }
            if (!dumpPath.empty() || !stateJsonPath.empty()) {
                std::ofstream j(stateJsonPath.empty() ? dumpPath + ".json" : stateJsonPath);
                writeMachineState(j, m, jit, {selectedProfile, executionWallNs, enableJit,
                                               followJumps, traceJit, enableHostInput});
            }
            if (!traceTailPath.empty()) {
                std::ofstream f(traceTailPath);
                auto begin = historyCount > history.size() ? historyCount - history.size() : 0;
                for (auto n = begin; n < historyCount; n++) {
                    auto h = history[n % history.size()];
                    f << std::dec << h.count << ' ' << std::hex << h.cs << ':' << h.ip
                      << " base=" << h.base << " flags=" << h.flags << " cr0=" << h.cr0
                      << " cr3=" << h.cr3;
                    for (auto r : h.r)
                        f << ' ' << r;
                    f << '\n';
                }
            }
        };
        std::signal(SIGUSR1, requestCapture);
        std::ifstream control;
        std::streamoff controlOffset = 0;
        if (!controlPath.empty()) {
            std::ofstream create(controlPath, std::ios::app);
            create.close();
            control.open(controlPath);
            if (!control)
                throw std::runtime_error("Cannot open guest control file");
        }

        auto refreshVideo = [&]() { display.refresh(m, executeVga); };
        auto saveFrame = [&]() { refreshVideo(); display.save(m, framePath, verbose); };
        bool quit = false;
        uint64_t nextTimer = 50000, nextCapture = captureEvery;
        size_t typePos = 0;
        uint64_t graphicsStart = 0, nextTyped = 0;
        uint64_t loops = 0;
        HostKeyboard keyboard([&](uint8_t byte) { m.ps2.enqueueScan(byte); });
        bool mouseCaptured = false;
        uint8_t mouseButtons = 0;
        auto setMouseCapture = [&](bool enabled) {
            bool success = frontend && frontend->capturePointer(enabled);
            mouseCaptured = enabled && success;
            if (enabled && !success) std::cerr << "Mouse capture unavailable\n";
        };
        auto releaseInput = [&] {
            keyboard.release();
            c.halted = false;
            mouseButtons = 0;
            m.ps2.motion(0, 0, 0);
            setMouseCapture(false);
        };
        const char *networkCapture = std::getenv("PCEMU_NET_PCAP");
        std::unique_ptr<Network> network;
        if (enableNetwork) network = std::make_unique<Network>(networkCapture ? networkCapture : "");
        uint64_t lastFrame = 0;
        std::string lastText;
        auto started = std::chrono::steady_clock::now();
        try {
            while (!quit && (headless ? (!limit || c.instructions < limit) : true)) {
                if (network) network->pump(m.nic, c.executionEpoch, m.resets);
                else m.nic.outgoing.clear();
                const auto batchStarted = std::chrono::steady_clock::now();
                for (int batch = 0; batch < 20000 && !c.halted; batch++) {
                    if (c.address(CS, c.ip) == breakAt) {
                        std::cerr << "Guest breakpoint at " << c.location() << '\n';
                        if (!afterBreak) {
                            quit = true;
                            break;
                        }
                        limit = c.instructions + afterBreak;
                        breakAt = 0x100000000ull;
                        if (!tracePath.empty())
                            trace.open(tracePath);
                    }
                    if (trace) {
                        trace << c.location();
                        for (auto reg : c.r)
                            trace << ' ' << std::hex << reg;
                        trace << ' ' << c.flags << std::dec << '\n';
                    }
                    if (!traceTailPath.empty()) {
                        auto &h = history[historyCount++ % history.size()];
                        h.ip = c.ip;
                        h.cs = c.s[CS].selector;
                        h.base = c.s[CS].base;
                        h.flags = c.flags;
                        h.cr0 = c.cr[0];
                        h.cr3 = c.cr[3];
                        h.count = c.instructions;
                        std::copy_n(c.r, 8, h.r);
                    }
                    jit.run();
                    m.poll();
                    if (c.instructions >= nextTimer) {
                        m.poll();
                        nextTimer = c.instructions + 50000;
                    }
                    if (headless && limit && c.instructions >= limit)
                        break;
                }
                // Includes CPU/device dispatch and host preemption, but excludes
                // control, framebuffer captures, video refresh and halted sleeps.
                executionWallNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - batchStarted)
                                       .count();
                if (control.is_open()) {
                    control.clear();
                    control.seekg(controlOffset);
                    std::string line;
                    while (std::getline(control, line)) {
                        controlOffset += line.size() + 1;
                        std::istringstream command(line);
                        std::string op;
                        command >> op;
                        if (op == "host-input") {
                            std::string setting;
                            command >> setting;
                            if (setting != "on" && setting != "off")
                                throw std::runtime_error("host-input needs on or off");
                            releaseInput();
                            enableHostInput = setting == "on";
                        } else if (op == "key") {
                            unsigned k;
                            command >> std::hex >> k;
                            m.key(k);
                        } else if (op == "scan") {
                            unsigned k;
                            command >> std::hex >> k;
                            m.ps2.enqueueScan(k);
                            c.halted = false;
                        } else if (op == "text") {
                            std::string text;
                            std::getline(command, text);
                            if (!text.empty() && text[0] == ' ')
                                text.erase(0, 1);
                            for (unsigned char ch : text)
                                m.key(ch);
                        } else if (op == "mouse") {
                            int dx = 0, dy = 0, buttons = 0, wheel = 0;
                            if (!(command >> dx >> dy >> buttons))
                                throw std::runtime_error("mouse needs DX DY BUTTONS [WHEEL]");
                            command >> wheel;
                            m.ps2.motion(dx, dy, buttons, wheel);
                        } else if (op == "capture")
                            captureRequested = 1;
                        else if (op == "profile-wait") {
                            unsigned milliseconds = 0;
                            command >> milliseconds;
                            if (milliseconds > 10000)
                                throw std::runtime_error("Profiler wait exceeds 10 seconds");
                            waitForProfiler(milliseconds);
                        }
#ifdef PCEMU_PROFILE
                        else if (op == "profile-start")
                            profile::start(c.instructions);
                        else if (op == "profile-stop") {
                            std::string path;
                            command >> path;
                            profile::stop(path, c.instructions);
                        }
#endif
                        else if (op == "fault-trace") {
                            std::string path;
                            command >> path;
                            unsigned stopVector = 256;
                            command >> std::hex >> stopVector;
                            faultTrace.close();
                            faultTrace.open(path);
                            if (!faultTrace)
                                throw std::runtime_error("Cannot open fault trace: " + path);
                            c.observeFault = [&, stopVector](const Fault &fault) {
                                faultTrace << std::hex << unsigned(fault.vector)
                                           << " error=" << fault.error << " at " << c.location()
                                           << " flags=" << c.flags << " cr2=" << c.cr[2]
                                           << " ldt=" << c.ldtBase;
                                for (auto reg : c.r)
                                    faultTrace << ' ' << reg;
                                faultTrace << '\n' << std::flush;
                                if (fault.vector == stopVector)
                                    throw std::runtime_error(
                                        "Diagnostic stop before guest fault delivery");
                            };
                        } else if (op == "save-snapshot" || op == "load-snapshot") {
                            std::string path;
                            std::getline(command >> std::ws, path);
                            try {
                                if (path.empty())
                                    throw std::runtime_error("Snapshot directory is required");
                                auto began = std::chrono::steady_clock::now();
                                if (op == "save-snapshot") {
                                    if (network) network->pump(m.nic, c.executionEpoch, m.resets);
                                    else m.nic.outgoing.clear();
                                    saveFrame();
                                    m.saveSnapshot(path);
                                    if (!framePath.empty()) {
                                        auto frame = readFile(framePath);
                                        std::vector<uint8_t> packed(
                                            ZSTD_compressBound(frame.size()));
                                        auto n = ZSTD_compress(packed.data(), packed.size(),
                                                               frame.data(), frame.size(), 1);
                                        if (ZSTD_isError(n))
                                            throw std::runtime_error(
                                                "Cannot compress snapshot preview");
                                        std::ofstream preview(std::filesystem::path(path) /
                                                                  "preview.ppm.zst",
                                                              std::ios::binary);
                                        preview.write(reinterpret_cast<const char *>(packed.data()),
                                                      n);
                                        if (!preview)
                                            throw std::runtime_error(
                                                "Cannot write snapshot preview");
                                    }
                                } else {
                                    m.loadSnapshot(path);
                                    jit.clear();
                                    executeVga = m.vgaOnReset;
                                    display.loadFont(m.vgaRom);
                                    nextTimer = c.instructions + 50000;
                                    nextCapture = c.instructions + captureEvery;
                                    nextTyped = c.instructions;
                                    typePos = type.size();
                                    historyCount = 0;
                                    graphicsStart = 0;
                                    lastFrame = 0;
                                    lastText.clear();
                                    limit = stepBudget ? c.instructions + stepBudget : 0;
                                }
                                auto seconds = std::chrono::duration<double>(
                                                   std::chrono::steady_clock::now() - began)
                                                   .count();
                                std::cerr << op << " completed in " << seconds << "s: " << path
                                          << '\n';
                                captureRequested = 1;
                            } catch (const std::exception &error) {
                                std::cerr << "Snapshot error: " << error.what() << '\n';
                            }
                        } else if (op == "break") {
                            std::string address;
                            command >> address;
                            breakAt = std::stoull(address, nullptr, 0);
                            afterBreak = 0;
                        } else if (op == "quit")
                            quit = true;
                        else if (!op.empty() && op[0] != '#')
                            throw std::runtime_error("Unknown guest-control command: " + op);
                        if (!op.empty())
                            std::cerr << "Guest control " << op << " @ " << c.instructions << '\n';
                    }
                }
                if (captureRequested || (captureEvery && c.instructions >= nextCapture)) {
                    captureRequested = 0;
                    capture();
                    saveFrame();
                    dumpCpu(c);
                    nextCapture = c.instructions + captureEvery;
                }
                refreshVideo();
                if (m.vga.graphics && !graphicsStart)
                    graphicsStart = c.instructions;
                if (c.instructions >= nextTyped &&
                    (m.vga.graphics || m.waitingKey ||
                     (!waitText.empty() && m.text().find(waitText) != std::string::npos) ||
                     (m.keys.empty() && (m.text().find("C:\\") != std::string::npos &&
                                         m.text().find('>') != std::string::npos))) &&
                    typePos < type.size()) {
                    char ch = type[typePos];
                    if (ch == '\f') {
                        if (graphicsStart && c.instructions > graphicsStart + 20000000)
                            typePos++;
                    } else if (ch == '\v') {
                        if (m.waitingKey)
                            typePos++;
                    } else {
                        typePos++;
                        m.key(ch == '\n' ? 0x1c0d : ch);
                        if (m.vga.graphics)
                            nextTyped = c.instructions + 100000;
                    }
                }
                if (headless) {
                    if (!untilText.empty() && m.text().find(untilText) != std::string::npos)
                        break;
                    if (!control.is_open() && c.halted &&
                        (m.waitingKey ? typePos >= type.size() : true))
                        break;
                    if (c.halted && control.is_open()) {
                        if (c.flags & IF)
                            m.timer();
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    if (++loops > 1000000 && !control.is_open())
                        break;
                    continue;
                }
                for (const auto &event : frontend->poll()) {
                    using K = FrontendEvent::Kind;
                    if (event.kind == K::Quit) { quit = true; continue; }
                    if (event.kind == K::FocusLost) { releaseInput(); continue; }
                    if (!enableHostInput) continue;
                    if (event.kind == K::ToggleCapture) {
                        keyboard.consumeCommand();
                        if (mouseCaptured) releaseInput();
                        else setMouseCapture(true);
                    } else if (event.kind == K::Key) {
                        keyboard.event(event.key, event.released);
                        c.halted = false;
                    } else if (event.kind == K::Button) {
                        if (!mouseCaptured) {
                            if (!event.released && event.button == 1) setMouseCapture(true);
                            continue;
                        }
                        mouseButtons = event.released ? mouseButtons & ~event.button
                                                      : mouseButtons | event.button;
                        m.ps2.motion(0, 0, mouseButtons);
                    } else if (event.kind == K::Motion && mouseCaptured)
                        m.ps2.motion(event.x, -event.y, mouseButtons);
                    else if (event.kind == K::Wheel && mouseCaptured)
                        m.ps2.motion(0, 0, mouseButtons, -event.y);
                }
                auto now = std::chrono::steady_clock::now();
                uint64_t frameTicks = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
                if (frameTicks - lastFrame < 16) {
                    if (c.halted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                lastFrame = frameTicks;
                unsigned newWidth = m.vga.graphics ? m.vga.width : 640;
                unsigned newHeight = m.vga.graphics ? m.vga.height : 400;
                if (displayHeight != newHeight || displayWidth != newWidth) {
                    displayWidth = newWidth;
                    pixels.resize(size_t(displayWidth) * newHeight);
                    displayHeight = newHeight;

                }
                display.render(m, pixels, displayWidth, displayHeight, frameTicks);
                frontend->present(pixels, displayWidth, displayHeight);
                if (c.halted) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            dumpCpu(c);
            saveFrame();
            std::cerr << "PIC mask=" << std::hex << unsigned(m.ports[0x21])
                      << " base=" << unsigned(m.pic.state().picBase[0])
                      << " pending scan bytes=" << m.ps2.state().scanCodes.size() << std::dec << "\n";
            capture();
            std::cout << m.text();
            return 1;
        }
        saveFrame();
        std::cerr << "PIC mask=" << std::hex << unsigned(m.ports[0x21])
                  << " base=" << unsigned(m.pic.state().picBase[0])
                  << " pending scan bytes=" << m.ps2.state().scanCodes.size() << std::dec << "\n";
        capture();
        if (headless) {
            std::cout << m.text();
            dumpCpu(c);
        }
        double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cerr << "Executed " << c.instructions << " guest instructions in " << elapsed
                  << "s; native=" << jit.nativeInstructions << ", blocks=" << jit.compiledBlocks
                  << ", recycles=" << jit.cacheRecycles << ", stopped="
                  << (m.waitingKey ? "keyboard"
                      : c.halted   ? "HLT"
                                   : "budget/quit")
                  << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "pcemu: " << e.what() << '\n';
        return 1;
    }
}
