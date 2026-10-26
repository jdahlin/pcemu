// RAM-only host timer benchmark. No guest images, HVF or interrupt threads.
#include "arm_hyperv_timers.hpp"
#include <chrono>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace pc;
int main(int argc, char **argv) {
    try {
        const uint64_t iterations = argc > 1 ? std::stoull(argv[1]) : 2000000;
        const unsigned trials = argc > 2 ? unsigned(std::stoul(argv[2])) : 3;
        if (!iterations || !trials || trials > 20)
            throw std::runtime_error("Invalid iterations/trials");
        std::cout << "{\"iterations\":" << iterations << ",\"trials\":[";
        bool comma = false;
        for (const std::string workload :
             {"quiet", "writes", "disable-autoenable", "periodic", "eight-vp"}) {
            for (unsigned trial = 0; trial < trials; ++trial) {
                ArmHyperVTimers timers(workload == "eight-vp" ? 8 : 2);
                timers.setDelivery(
                    [](unsigned, unsigned, uint64_t, uint64_t, uint64_t) { return true; });
                timers.write(0xb0000, 0, workload == "periodic" ? 0x110a : 0x1108, 0);
                timers.write(0xb0001, 0, workload == "periodic" ? 32 : iterations + 100, 0);
                uint64_t checksum = 0;
                const auto start = std::chrono::steady_clock::now();
                const auto cpu = std::clock();
                for (uint64_t now = 1; now <= iterations; ++now) {
                    if (workload == "writes" || (now % 4 == 0 && workload == "quiet"))
                        timers.write(0xb0001, 0, now + 100, now);
                    if (workload == "disable-autoenable") {
                        const bool periodic = (now / 8) & 1;
                        if (now % 8 == 0)
                            timers.write(0xb0000, 0, periodic ? 0x110a : 0x1108, now);
                        if (now % 8 == 1)
                            timers.write(0xb0001, 0, periodic ? 32 : now + 100, now);
                    }
                    timers.service(now);
                    checksum += timers.nextDelay(now, false).value_or(0);
                    checksum += timers.nextDelay(now, true).value_or(0);
                }
                const auto cpuSeconds = double(std::clock() - cpu) / CLOCKS_PER_SEC;
                const auto wall =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                if (comma)
                    std::cout << ',';
                comma = true;
                std::cout << "{\"workload\":\"" << workload << "\",\"trial\":" << trial
                          << ",\"cpuSeconds\":" << cpuSeconds << ",\"wallSeconds\":" << wall
                          << ",\"checksum\":" << checksum
                          << ",\"deliveries\":" << timers.delivered() << '}';
            }
        }
        std::cout << "]}\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
