#include "machine.hpp"
#include "quake_benchmark.hpp"
#include <iostream>
using namespace pc;
static unsigned checks;
static void check(bool value, const char *message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        for (auto text : {"969 frames  12.3 seconds  78.8 fps\n", "Loading...\n969 frames 12.3 seconds 78.8 fps\r\n"}) {
            auto result = parseQuakeTimedemo(text);
            check(result && result->frames == 969 && result->seconds == 12.3 && result->fps == 78.8,
                  "parse a complete original timedemo line");
        }
        for (auto text : {"969 frames 12.3 seconds 78.8 fps", "0 frames 1 seconds 1 fps\n",
             "-1 frames 1 seconds 1 fps\n", "1 frames 0 seconds 1 fps\n", "1 frames 1 seconds nan fps\n",
             "1 frames inf seconds 1 fps\n", "1 frames 1 seconds 0 fps\n", "1 frames 1 seconds 1 fps extra\n",
             "1 frames 1 seconds\n1 fps\n", "Can't play demo1.dem\n"})
            check(!parseQuakeTimedemo(text), "reject incomplete, invalid or unrelated output");
        auto owner = std::make_unique<Machine>("", "", 2 * 1024 * 1024);
        auto &m = *owner;
        m.cpu.instructions = 400;
        check(m.pitTicks() == 100, "normal machines retain the deterministic instruction clock");
        uint64_t now = 1000;
        m.pitClock = [&] { return now; };
        m.pit.reset(now);
        m.out(0x43, 0x34, 1); m.out(0x40, 100, 1); m.out(0x40, 0, 1); // Counter0 mode2 divisor100.
        m.cpu.instructions += 1000000;
        now += 25;
        m.out(0x43, 0, 1); // Latch current counter0 count.
        auto low = m.in(0x40, 1), high = m.in(0x40, 1);
        check((low | (high << 8)) == 75, "PIT ports use injected elapsed time rather than instructions");
        m.pic[0].irr = 0; m.cpu.flags &= ~IF;
        m.poll(); check(!(m.pic[0].irr & 1), "host-clock IRQ0 is not due before its terminal count");
        now += 75;
        m.poll(); check(m.pic[0].irr & 1, "host-clock terminal count raises IRQ0");
        std::cout << "Passed " << checks << " Quake benchmark checks\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
