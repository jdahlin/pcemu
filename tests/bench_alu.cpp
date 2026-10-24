#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
extern "C" uint32_t alu_kernel(uint32_t target);
int main(int argc, char **argv) {
    unsigned long blocks = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 5000000;
    if (!blocks || blocks > UINT32_MAX / 48)
        return 2;
    uint32_t expected = blocks * 48;
    alu_kernel(48000);
    double samples[5];
    for (double &sample : samples) {
        auto start = std::chrono::steady_clock::now();
        uint32_t actual = alu_kernel(expected);
        sample = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (actual != expected)
            return 1;
    }
    std::sort(samples, samples + 5);
    std::printf("%.3f million equivalent guest instructions/s; checksum=%u; median=%.6fs\n",
                blocks * 50.0 / samples[2] / 1e6, expected, samples[2]);
}
