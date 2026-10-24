#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
extern "C" void memory_kernel(uint32_t *cells, uint32_t loops);
int main(int argc, char **argv) {
    unsigned long loops = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 1000000;
    if (!loops || loops > UINT32_MAX)
        return 2;
    double samples[5];
    uint64_t checksum = 0;
    for (double &sample : samples) {
        alignas(64) uint32_t cells[16] = {};
        auto start = std::chrono::steady_clock::now();
        memory_kernel(cells, uint32_t(loops));
        sample = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        checksum = 0;
        for (uint32_t cell : cells) {
            if (cell != loops)
                return 1;
            checksum += cell;
        }
    }
    std::sort(samples, samples + 5);
    std::printf("%.3f million equivalent guest instructions/s; checksum=%llu; median=%.6fs\n",
                loops * 49.0 / samples[2] / 1e6, (unsigned long long)checksum, samples[2]);
}
