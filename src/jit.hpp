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
    void setDirectLinks(bool enabled, unsigned hotEntries = 32);
    bool directLinksEnabled() const { return directLinks; }
    uint64_t directLinkTransitions = 0, directLinkPromotions = 0, directLinkBlocks = 0;
    // Optional diagnostic sidecar. Does not change arena capacity or generated code.
    void setDebugMap(const std::string &path);
    void flushDebugMap();
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
                    const std::vector<DebugRegion> &regions);
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
        bool vm = false, watched = false, usesStack = false, stack32 = false;
        const uint8_t *ram = nullptr;
        const uint64_t *codeBits = nullptr;
        size_t ramSize = 0;
        unsigned visits = 0;
        std::shared_ptr<std::vector<NativeMemoryMove>> memory;
        std::shared_ptr<LinkState> link;
    };
    CPU &cpu;
    uint8_t *arena = nullptr;
    size_t used = 0, capacity = 16 * 1024 * 1024;
    bool active = false;
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
    CachedBatch runCached(unsigned budget) noexcept;
    Block compile(bool translate = true);
    jit_detail::DecodedBlock decode(Block &block);
    Block emitArm64(Block block, const jit_detail::DecodedBlock &decoded);
    std::unique_ptr<DebugState> debugState; // Keep diagnostic storage out of hot dispatch fields.
    bool directLinks = false;
    unsigned linkWarmup = 32;
    struct IncomingLink { uint64_t source; unsigned slot; std::weak_ptr<LinkState> state; };
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
