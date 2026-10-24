#include "host_keyboard.hpp"
#include "jit.hpp"
#include "machine.hpp"
#include "machine_profile.hpp"
#include "network.hpp"
#include "profile.hpp"
#include "frontend.hpp"
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

static void dump(CPU &c) {
    std::cerr << "\nCPU " << c.location() << " instructions=" << c.instructions
              << " flags=" << std::hex << c.flags << " CR0=" << c.cr[0] << " CR3=" << c.cr[3]
              << " CR2=" << c.cr[2] << '\n';
    for (int i = 0; i < 8; i++)
        std::fprintf(
            stderr, "%s=%08x ",
            std::array<const char *, 8>{"EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"}[i],
            c.r[i]);
    std::cerr << '\n';
    for (int i = 0; i < 6; i++)
        std::fprintf(stderr, "S%d=%04x(base %08x) ", i, c.s[i].selector, c.s[i].base);
    std::cerr << std::dec << '\n';
}
static double bench(bool enabled, uint64_t target) {
    CPU c;
    c.s[CS].big = true;
    unsigned p = 0;
    for (int i = 0; i < 48; i++) {
        c.mem[p++] = 0x83;
        c.mem[p++] = 0xc0;
        c.mem[p++] = 1;
    }
    c.mem[p++] = 0x83;
    c.mem[p++] = 0xf8;
    c.mem[p++] = 0;
    c.mem[p++] = 0xe9;
    int32_t disp = -int32_t(p + 4);
    std::memcpy(&c.mem[p], &disp, 4);
    Jit jit(c, enabled);
    auto begin = std::chrono::steady_clock::now();
    while (c.instructions < target)
        jit.run();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::cout << (enabled ? "JIT" : "Interpreter") << ": " << c.instructions / sec / 1e6
              << " million guest instructions/s, " << jit.nativeInstructions << " native, "
              << jit.compiledBlocks << " blocks; EAX=" << c.r[AX] << '\n';
    return sec;
}
static double benchMemory(bool enabled, uint64_t target, bool paging) {
    CPU c;
    c.s[CS].big = true;
    unsigned p = 0;
    auto address = [&](uint32_t value) {
        std::memcpy(c.mem.data() + p, &value, 4);
        p += 4;
    };
    for (unsigned cell = 0; cell < 16; cell++) {
        c.mem[p++] = 0xa1;
        address(0x1000 + cell * 4);
        c.mem[p++] = 0x83;
        c.mem[p++] = 0xc0;
        c.mem[p++] = 1;
        c.mem[p++] = 0xa3;
        address(0x1000 + cell * 4);
    }
    c.mem[p++] = 0xe9;
    address(uint32_t(-int32_t(p + 4)));
    if (paging) {
        c.write(0x2000, 0x3007, 4);
        c.write(0x3000, 7, 4);
        c.write(0x3004, 0x1007, 4);
        c.cr[0] = 0x80000011;
        c.cr[3] = 0x2000;
        c.s[CS].selector = 3;
    }
    target = std::max(uint64_t(49), target / 49 * 49);
    Jit jit(c, enabled);
    auto begin = std::chrono::steady_clock::now();
    while (c.instructions < target)
        jit.run();
    double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    uint64_t checksum = 0;
    for (unsigned cell = 0; cell < 16; cell++) {
        auto value = c.read(0x1000 + cell * 4, 4);
        if (value != target / 49)
            throw std::runtime_error("Memory benchmark checksum mismatch");
        checksum += value;
    }
    std::cout << (paging ? "Paged" : "Flat") << " memory " << (enabled ? "JIT" : "interpreter")
              << ": " << c.instructions / seconds / 1e6 << " million guest instructions/s, "
              << jit.nativeInstructions << " native; checksum=" << checksum << '\n';
    return seconds;
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
                auto ref = benchMemory(false, benchmarkSteps, paging);
                auto native = benchMemory(true, benchmarkSteps, paging);
                std::cout << "Memory speedup: " << ref / native << "x\n";
            }
            return 0;
        }
        if (benchmark) {
            double ref = bench(false, benchmarkSteps), jit = bench(true, benchmarkSteps);
            std::cout << "Speedup: " << ref / jit << "x (register-only synthetic workload)\n";
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
        std::vector<uint8_t> font(2048);
        auto loadFont = [&](const std::vector<uint8_t> &bytes) {
            if (bytes.empty())
                return;
            const uint8_t glyph[] = {0x30, 0x78, 0xcc, 0xcc, 0xfc, 0xcc, 0xcc, 0};
            auto it = std::search(bytes.begin(), bytes.end(), std::begin(glyph), std::end(glyph));
            if (it == bytes.end() || it - bytes.begin() < 65 * 8)
                throw std::runtime_error("Cannot locate 8x8 font in VGA ROM");
            size_t off = it - bytes.begin() - 65 * 8;
            std::copy_n(bytes.begin() + off, 2048, font.begin());
        };
        if (snapshotPath.empty() && !rom.empty()) {
            auto bytes = readFile(rom);
            for (size_t i = 0; i < bytes.size() && 0xc0000 + i < c.mem.size(); i++)
                c.mem[0xc0000 + i] = bytes[i];
            m.vgaRom = std::move(bytes);
        }
        loadFont(m.vgaRom);
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
        const uint32_t palette[16] = {0xff000000, 0xff0000aa, 0xff00aa00, 0xff00aaaa,
                                      0xffaa0000, 0xffaa00aa, 0xffaa5500, 0xffaaaaaa,
                                      0xff555555, 0xff5555ff, 0xff55ff55, 0xff55ffff,
                                      0xffff5555, 0xffff55ff, 0xffffff55, 0xffffffff};
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
                j << "{\"ip\":" << c.ip << ",\"flags\":" << c.flags << ",\"cr0\":" << c.cr[0]
                  << ",\"cr3\":" << c.cr[3] << ",\"cr2\":" << c.cr[2]
                  << ",\"gdtBase\":" << c.gdtBase << ",\"gdtLimit\":" << c.gdtLimit
                  << ",\"idtBase\":" << c.idtBase << ",\"idtLimit\":" << c.idtLimit
                  << ",\"trBase\":" << c.trBase << ",\"trSelector\":" << c.trSelector
                  << ",\"ldtBase\":" << c.ldtBase << ",\"registers\":[";
                for (int k = 0; k < 8; k++) {
                    if (k)
                        j << ',';
                    j << c.r[k];
                }
                j << "],\"segments\":[";
                for (int k = 0; k < 6; k++) {
                    if (k)
                        j << ',';
                    j << "{\"selector\":" << c.s[k].selector << ",\"base\":" << c.s[k].base
                      << ",\"limit\":" << c.s[k].limit << ",\"big\":" << c.s[k].big
                      << ",\"unusable\":" << c.s[k].unusable << '}';
                }
                j << "],\"instructions\":" << c.instructions << ",\"resets\":" << m.resets
                  << ",\"resetRequested\":" << m.resetRequested
                  << ",\"nativeInstructions\":" << jit.nativeInstructions
                  << ",\"executionWallNs\":" << executionWallNs
                  << ",\"nativeMemoryCalls\":" << c.nativeMemoryCalls
                  << ",\"executionEpoch\":" << c.executionEpoch
                  << ",\"machineProfile\":" << std::quoted(std::string(selectedProfile.id))
                  << ",\"machinePlatform\":" << std::quoted(std::string(selectedProfile.platform))
                  << ",\"executionBackend\":" << std::quoted(enableJit ? "jit" : "interpreter")
                  << ",\"jitFollowJumps\":" << (enableJit && followJumps)
                  << ",\"jitTraces\":" << (enableJit && traceJit)
                  << ",\"ramBytes\":" << c.mem.size()
                  << ",\"disk\":" << std::quoted(m.disk ? m.disk->path : "")
                  << ",\"compiledBlocks\":" << jit.compiledBlocks
                  << ",\"cacheRecycles\":" << jit.cacheRecycles << ",\"halted\":" << c.halted
                  << ",\"waitingKey\":" << m.waitingKey
                  << ",\"hostInput\":" << enableHostInput
                  << ",\"network\":{\"isr\":" << unsigned(m.nic.isr)
                  << ",\"imr\":" << unsigned(m.nic.imr) << ",\"command\":" << unsigned(m.nic.command)
                  << ",\"current\":" << unsigned(m.nic.current) << ",\"boundary\":" << unsigned(m.nic.boundary)
                  << ",\"transmitted\":" << m.nic.transmitted << ",\"received\":" << m.nic.received
                  << ",\"dropped\":" << m.nic.dropped << "}"
                  << ",\"mouse\":{\"enabled\":" << m.mouse.enabled
                  << ",\"id\":" << unsigned(m.mouse.id)
                  << ",\"pendingPackets\":" << m.mouse.packets.size()
                  << ",\"auxOutput\":" << (m.keyboardFull && m.keyboardAux) << "}"
                  << ",\"video\":{\"graphics\":" << m.vga.graphics << ",\"width\":" << m.vga.width
                  << ",\"height\":" << m.vga.height << ",\"memoryMode\":" << unsigned(m.vga.seq[4])
                  << ",\"shiftMode\":" << unsigned(m.vga.gfx[5])
                  << ",\"dispiEnabled\":" << unsigned(m.vga.dispi[4])
                  << ",\"bpp\":" << m.vga.dispi[3] << ",\"crtc\":[";
                for (unsigned index = 0; index < 32; index++) {
                    if (index)
                        j << ',';
                    j << unsigned(m.vga.crtc[index]);
                }
                j << "]},\"pic\":[";
                for (int k = 0; k < 2; k++) {
                    if (k)
                        j << ',';
                    j << "{\"irr\":" << unsigned(m.pic[k].irr)
                      << ",\"isr\":" << unsigned(m.pic[k].isr)
                      << ",\"mask\":" << unsigned(m.pic[k].mask)
                      << ",\"base\":" << unsigned(m.pic[k].base)
                      << ",\"autoEOI\":" << m.pic[k].autoEOI << '}';
                }
                auto &counter = m.pit.counters[0];
                j << "],\"pit0\":{\"reload\":" << counter.reload
                  << ",\"loadedAt\":" << counter.loadedAt << ",\"mode\":" << unsigned(counter.mode)
                  << ",\"nullCount\":" << counter.nullCount
                  << ",\"writeHigh\":" << counter.writeHigh
                  << ",\"value\":" << counter.value(c.instructions / 4) << "},\"keyboard\":{"
                  << "\"full\":" << m.keyboardFull << ",\"data\":" << unsigned(m.keyboardData)
                  << ",\"pendingScans\":" << m.scanCodes.size()
                  << ",\"pendingReplies\":" << m.keyboardReplies.size()
                  << ",\"commandByte\":" << unsigned(m.keyboardCommandByte) << "}}";
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

        auto refreshVideo = [&]() {
            m.vga.updateGeometry();
            if (!executeVga)
                return;
            auto mode = c.mem[0x449] & 127;
            m.videoMode = mode;
            unsigned columns = c.mem[0x44a] | (c.mem[0x44b] << 8);
            if (columns == 40 || columns == 80)
                m.columns = columns;
            m.activePage = c.mem[0x462] & 7;
            m.rows = std::min(50u, unsigned(c.mem[0x484]) + 1);
            if (!m.rows)
                m.rows = 25;
            auto pos = c.mem[0x450 + m.activePage * 2] | (c.mem[0x451 + m.activePage * 2] << 8);
            m.cursor = (pos >> 8) * m.columns + (pos & 255);
        };
        auto saveFrame = [&]() {
            refreshVideo();
            if (verbose) {
                std::cerr << "VGA attribute palette:";
                for (int k = 0; k < 16; k++)
                    std::cerr << ' ' << unsigned(m.vga.attr[k]);
                std::cerr << " DAC1=" << unsigned(m.vga.dac[1][0]) << ','
                          << unsigned(m.vga.dac[1][1]) << ',' << unsigned(m.vga.dac[1][2]) << '\n';
            }
            if (framePath.empty())
                return;
            unsigned width = m.vga.graphics ? m.vga.width : 640,
                     height = m.vga.graphics ? m.vga.height : 400;
            std::ofstream f(framePath, std::ios::binary);
            f << "P6\n" << width << ' ' << height << "\n255\n";
            std::vector<uint8_t> rgb(size_t(width) * height * 3);
            for (unsigned y = 0; y < height; y++)
                for (unsigned x = 0; x < width; x++) {
                    uint32_t px;
                    if (m.vga.graphics)
                        px = m.vga.pixel(x, y);
                    else {
                        auto off =
                            0xb8000 + m.activePage * 4096 + ((y / 16) * m.columns + x / 8) * 2;
                        auto ch = c.mem[off], attr = c.mem[off + 1];
                        bool on = font[ch * 8 + (y % 16) / 2] & (128 >> (x & 7));
                        px = palette[on ? (attr & 15) : (attr >> 4) & 7];
                    }
                    auto at = (size_t(y) * width + x) * 3;
                    rgb[at] = px >> 16;
                    rgb[at + 1] = px >> 8;
                    rgb[at + 2] = px;
                }
            f.write(reinterpret_cast<const char *>(rgb.data()), rgb.size());
        };
        bool quit = false;
        uint64_t nextTimer = 50000, nextCapture = captureEvery;
        size_t typePos = 0;
        uint64_t graphicsStart = 0, nextTyped = 0;
        uint64_t loops = 0;
        HostKeyboard keyboard(m.scanCodes);
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
            m.mouse.motion(0, 0, 0);
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
                            m.scanCodes.push_back(k);
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
                            m.mouse.motion(dx, dy, buttons, wheel);
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
                                    loadFont(m.vgaRom);
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
                    dump(c);
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
                        m.mouse.motion(0, 0, mouseButtons);
                    } else if (event.kind == K::Motion && mouseCaptured)
                        m.mouse.motion(event.x, -event.y, mouseButtons);
                    else if (event.kind == K::Wheel && mouseCaptured)
                        m.mouse.motion(0, 0, mouseButtons, -event.y);
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
                for (unsigned y = 0; y < displayHeight; y++)
                    for (unsigned x = 0; x < displayWidth; x++) {
                        if (m.vga.graphics) {
                            pixels[y * displayWidth + x] = m.vga.pixel(x, y);
                            continue;
                        }
                        unsigned col = x / 8, row = y / 16;
                        uint32_t px = palette[0];
                        if (col < m.columns && row < m.rows) {
                            auto off = 0xb8000 + m.activePage * 4096 + (row * m.columns + col) * 2;
                            uint8_t ch = c.mem[off], attr = c.mem[off + 1],
                                    bits = font[ch * 8 + (y % 16) / 2];
                            px = palette[(bits & (128 >> (x % 8))) ? (attr & 15)
                                                                   : ((attr >> 4) & 7)];
                            if (row * m.columns + col == m.cursor && y % 16 >= 14 &&
                                (frameTicks / 500) % 2 == 0)
                                px = palette[7];
                        }
                        pixels[y * displayWidth + x] = px;
                    }
                frontend->present(pixels, displayWidth, displayHeight);
                if (c.halted) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            dump(c);
            saveFrame();
            std::cerr << "PIC mask=" << std::hex << unsigned(m.ports[0x21])
                      << " base=" << unsigned(m.picBase[0])
                      << " pending scan bytes=" << m.scanCodes.size() << std::dec << "\n";
            capture();
            std::cout << m.text();
            return 1;
        }
        saveFrame();
        std::cerr << "PIC mask=" << std::hex << unsigned(m.ports[0x21])
                  << " base=" << unsigned(m.picBase[0])
                  << " pending scan bytes=" << m.scanCodes.size() << std::dec << "\n";
        capture();
        if (headless) {
            std::cout << m.text();
            dump(c);
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
