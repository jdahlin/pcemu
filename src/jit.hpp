#pragma once
#include "cpu.hpp"
#include <memory>
#include <unordered_map>
namespace pc {
struct NativeMemoryMove {
    int base = -1, index = -1, scale = 0, segment = DS, reg = 0, width = 4;
    uint32_t displacement = 0, begin = 0, end = 0;
    bool address32 = false, store = false;
};
// ARM64 baseline recompiler. Register arithmetic, direct/counter branches and guarded
// 8/16/32-bit RAM moves, register stack operations and near CALL/RET run in native blocks;
// instructions outside the supported lowering set use the reference interpreter.
class Jit {
  public:
    explicit Jit(CPU &cpu, bool enabled = true, unsigned warmup = 8,
                 size_t arenaBytes = 16 * 1024 * 1024);
    ~Jit();
    Jit(const Jit &) = delete;
    Jit &operator=(const Jit &) = delete;
    uint32_t run();
    void clear();
    uint64_t nativeInstructions = 0, compiledBlocks = 0, cacheHits = 0, cacheRecycles = 0;
    bool enabled() const {
        return active;
    }

  private:
    using Function = uint32_t (*)(CPU *);
    struct Block {
        Function fn = nullptr;
        uint32_t page = 0, generation = 0, cr0 = 0, cr3 = 0, csBase = 0, tlbEpoch = 0;
        uint64_t executionEpoch = 0;
        bool a20 = true, attempted = false;
        bool vm = false, watched = false, usesStack = false, stack32 = false;
        const uint8_t *ram = nullptr;
        const uint64_t *codeBits = nullptr;
        size_t ramSize = 0;
        unsigned visits = 0;
        std::shared_ptr<std::vector<NativeMemoryMove>> memory;
    };
    CPU &cpu;
    uint8_t *arena = nullptr;
    size_t used = 0, capacity = 16 * 1024 * 1024;
    bool active = false;
    unsigned warmup;
    std::unordered_map<uint64_t, Block> blocks;
    struct FastEntry {
        uint64_t key = 0;
        Block *block = nullptr;
    };
    std::array<FastEntry, 4096> fast{};
    Block compile(bool translate = true);
};
} // namespace pc
