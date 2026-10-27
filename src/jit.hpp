#pragma once
#include "cpu.hpp"
#include "jit_memory.hpp"
#include <memory>
#include <fstream>
#include <unordered_map>
namespace pc {
namespace jit_detail {
struct DecodedBlock;
}
// ARM64 baseline recompiler. Register arithmetic, direct/counter branches and guarded
// 8/16/32-bit RAM moves, MOVZX/MOVSX, stack operations and near CALL/RET
// run in native blocks;
// instructions outside the supported lowering set use the reference interpreter.
class Jit {
  public:
    enum class BlockMode { Basic, FollowJumps, Trace };
    explicit Jit(CPU &cpu, bool enabled = true, unsigned warmup = 8,
                 size_t arenaBytes = 16 * 1024 * 1024,
                 BlockMode mode = BlockMode::Basic);
    ~Jit();
    Jit(const Jit &) = delete;
    Jit &operator=(const Jit &) = delete;
    uint32_t run();
    struct BatchResult {
        uint32_t instructions = 0, dispatches = 0;
    };
    // Soft budget capped at 64, at most 127 completed guest instructions.
    // Stops on interpreter/replay boundaries. Caller must poll devices next;
    // use run() instead for per-block debugger/trace observations.
    BatchResult runBatch(unsigned budget = 64);
    void clear();
    // End blocks/batches before these linear guest addresses. The first run()
    // in a new batch executes a point normally, so observers cannot stall there.
    // Direct links are incompatible with these externally observed boundaries.
    void setObservationPoints(std::vector<uint32_t> linear);

    void setDirectLinks(bool enabled, unsigned hotEntries = 32);
    bool directLinksEnabled() const { return directLinks; }
    uint64_t directLinkTransitions = 0, directLinkPromotions = 0, directLinkBlocks = 0;
    // Optional diagnostic sidecar. Does not change arena capacity or generated code.
    void setDebugMap(const std::string &path);
    void flushDebugMap();
    uint64_t nativeHelperInstructions = 0; // Decoded x87 helper operations, separately visible.
    uint64_t nativeInstructions = 0, compiledBlocks = 0, cacheHits = 0, cacheRecycles = 0;
    bool enabled() const {
        return active;
    }
    size_t residentCodeBytes() const {
        return used + stubUsed; // Arena occupancy, including block alignment; resets on recycle/clear.
    }

  private:
    struct Block;
    struct DebugRegion { size_t offset; const char *kind; uint32_t guestIP; };
    struct DebugRecord {
        int64_t time;
        uint64_t generation;
        uintptr_t start = 0;
        uint32_t cs = 0, csBase = 0, ip = 0, cr3 = 0, page = 0, codeGeneration = 0;
        std::vector<uint32_t> code;
        std::vector<DebugRegion> regions;
        bool patch = false;
    };
    struct DebugState {
        std::vector<DebugRecord> records;
        std::ofstream stream;
        uint64_t generation = 0;
    };
    void debugReset();
    void debugBlock(const Block &block, const std::vector<uint32_t> &code,
                    const std::vector<DebugRegion> &regions, uintptr_t address = 0);
    using Function = uint64_t (*)(CPU *, unsigned);
    struct LinkState {
        std::array<uintptr_t, 2> target{};
        std::array<uint32_t, 2> ips{};
        uint32_t startIP = 0, entry = 0, saveMask = 0;
        uint32_t loadedEntry = 0, loopEntry = 0, usedRegisters = 0;
        const uint32_t *generationData = nullptr;
        unsigned count = 0;
        std::array<uint32_t, 2> branchOffsets{};
        uint32_t fallbackOffset = 0, fullExitOffset = 0, earlyExitOffset = 0;
        uint32_t resetFlagOffset = 0, codeBytes = 0;
        bool promoted = false, fetchCheck = false;
        std::vector<uint32_t> helperFlagSites;
    };
    struct Block {
        Function fn = nullptr;
        uint32_t page = 0, generation = 0, cr0 = 0, cr3 = 0, csBase = 0, tlbEpoch = 0;
        uint64_t executionEpoch = 0;
        bool a20 = true, attempted = false, linkRequested = false;
        uint8_t shortKind = 0; // Supported but rejected: 1 register-only, 2 memory.
        bool vm = false, watched = false, usesStack = false, stack32 = false;
        bool pollBoundary = false; // Completed control instructions must return to the device poll.
        bool changesShadow = false; // STI and segment loads into SS need an accounted instruction boundary.
        uint32_t cachedContext = 0; // Identity of common compiled execution assumptions.
        const uint8_t *ram = nullptr;
        const uint64_t *codeBits = nullptr;
        size_t ramSize = 0;
        unsigned visits = 0;
        struct BoundaryEntry {
            Function fn=nullptr;
            NativeCodeTail fetchTail;
            CPU::DecodedX87 x87;
            NativeString string;
            uint8_t interruptVector=0;
            uint32_t interruptEnd=0;
            union {
                NativePortIO operation{};
                NativeFarReturn farReturn;
            };
            std::unique_ptr<NativeFarTransfer> farTransfer;
        };
        struct NativeState {
            std::vector<NativeMemoryMove> memory;
            std::vector<CPU::DecodedSegment> segments;
            std::vector<CPU::DecodedMemoryArithmetic> arithmetic;
            std::vector<NativeString> strings;
            std::unique_ptr<LinkState> link;
            std::unique_ptr<BoundaryEntry> boundary;
        };
        // Negative entries and register-only unlinked blocks own no native
        // metadata. Helper operands remain alive until the block is replaced.
        std::unique_ptr<NativeState> native;
        BoundaryEntry *boundaryEntry() const { return native ? native->boundary.get() : nullptr; }
        LinkState *link() const { return native ? native->link.get() : nullptr; }
    };
    CPU &cpu;
    uint8_t *arena = nullptr;
    size_t used = 0, capacity = 16 * 1024 * 1024;
    bool active = false;
    bool stopBatch = false; // Set by run(); boundary blocks never enter cached tails.
    std::vector<uint32_t> observationPoints;
    bool atObservationPoint() const {
        if(observationPoints.empty())return false;
        const auto linear=cpu.s[CS].base+cpu.ip;
        for(auto point:observationPoints)if(point==linear)return true;
        return false;
    }
    unsigned warmup;
    BlockMode blockMode;
    std::unordered_map<uint64_t, Block> blocks;
    struct FastEntry {
        uint64_t key = 0;
        Block *block = nullptr;
    };
    std::array<FastEntry, 4096> fast{};
    struct CachedBatch {
        BatchResult count;
        bool stoppedZero = false;
    };
    struct CachedContext {
        uint64_t executionEpoch = 0;
        const uint8_t *ram = nullptr;
        const uint64_t *codeBits = nullptr;
        size_t ramSize = 0;
        uint32_t cr0 = 0, cr3 = 0, csBase = 0;
        bool a20 = false, vm = false, watched = false;
        bool operator==(const CachedContext &) const = default;
    };
    struct CachedContextHash { size_t operator()(const CachedContext &) const noexcept; };
    std::unordered_map<CachedContext,uint32_t,CachedContextHash> cachedContexts;
    uint32_t nextCachedContext = 0, validatedCachedContext = 0;
    uint32_t contextFor(const Block &);
    CachedBatch runCached(unsigned budget) noexcept;
    template <bool Linking> CachedBatch runCachedImpl(unsigned budget) noexcept;
    static bool warmInstructionPage(CPU&,uint32_t ip,uint32_t &page);
    static uint32_t instructionPage(CPU&,uint32_t ip);
    void finishDecodedExit();
    Block compile(bool translate = true, bool hotShort = false);
    jit_detail::DecodedBlock decode(Block &block);
    Block emitArm64(Block block, const jit_detail::DecodedBlock &decoded);
    std::unique_ptr<DebugState> debugState; // Keep diagnostic storage out of hot dispatch fields.
    bool directLinks = false;
    unsigned linkWarmup = 32;
    // Native entry addresses are unique until recycle, which clears this table.
    struct IncomingLink { uint64_t source; unsigned slot; Function sourceEntry; };
    std::unordered_map<uint64_t, std::vector<IncomingLink>> incomingLinks;
    Block *installBlock(uint64_t key, Block block);
    void patchIncoming(uint64_t key);
    void setLinkTarget(Block &source, unsigned slot, uintptr_t target);
    void publishLinks(uint64_t key, Block &block);
    bool promoteBlock(Block &block);
    size_t stubCapacity = 0, stubUsed = 0;
    void debugPatch(uintptr_t address, uint32_t instruction);
};
} // namespace pc
