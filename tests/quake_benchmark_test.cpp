#include "machine.hpp"
#include "pc_execution.hpp"
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
        {
            Machine m("","",1024*1024);m.cpu.cr[0]=0x11;m.cpu.s[CS].big=true;m.cpu.flags=2;
            m.cpu.mem[0]=0x40;m.cpu.mem[1]=0xf4;unsigned polls=0;
            m.pitClock=[&]{++polls;return uint64_t(0);};
            Jit jit(m.cpu,true,0);uint64_t next=0;
            auto batch=runPcBatch(m,jit,next,64);
            check(batch.instructions==2 && m.cpu.r[AX]==1 && m.cpu.halted,"shared session batch executes guest code");
            check(polls==2 && next==m.cpu.instructions+50000,"shared session timer checkpoint polls exactly twice");
            polls=0;pollPcDevices(m,next);check(polls==1,"ordinary session poll retains timer checkpoint");
        }
        for (auto text : {"969 frames  12.3 seconds  78.8 fps\n", "Loading...\n969 frames 12.3 seconds 78.8 fps\r\n"}) {
            auto result = parseQuakeTimedemo(text);
            check(result && result->frames == 969 && result->seconds == 12.3 && result->fps == 78.8,
                  "parse a complete original timedemo line");
        }
        auto rounded = parseQuakeTimedemo("1 frames 0.0 seconds 20.4 fps\n");
        check(rounded && rounded->frames == 1 && rounded->seconds == 0 && rounded->fps == 20.4,
              "short timedemo rounded elapsed seconds");
        for (auto text : {"969 frames 12.3 seconds 78.8 fps", "0 frames 1 seconds 1 fps\n",
             "-1 frames 1 seconds 1 fps\n", "1 frames -0.1 seconds 1 fps\n", "1 frames 1 seconds nan fps\n",
             "1 frames inf seconds 1 fps\n", "1 frames 1 seconds 0 fps\n", "1 frames 1 seconds 1 fps extra\n",
             "1 frames 1 seconds\n1 fps\n", "Can't play demo1.dem\n"})
            check(!parseQuakeTimedemo(text), "reject incomplete, invalid or unrelated output");
        {
            CPU c(2 * 1024 * 1024);
            c.cr[0] = 1; c.s[CS].selector = 3; c.s[CS].big = true;
            c.s[CS].base = c.s[DS].base = 0x10000;
            const uint8_t begin[] = {0xd9,0x1d,0x94,0xc9,6,0};
            const uint8_t end[] = {0x83,0xec,4,0xc7,5,0x80,0xc9,6,0,0,0,0,0};
            for (unsigned n=0;n<sizeof(begin);++n) c.write(0x13ac3+n,begin[n],1);
            for (unsigned n=0;n<sizeof(end);++n) c.write(0x13f86+n,end[n],1);
            c.write(0x10000+0x95bb4,101,4); c.write(0x10000+0x6c990,100,4);
            QuakeProfileWindow w;
            c.ip=0x3f86;
            check(w.observe(c)==QuakeProfileWindow::Event::None,"finish cannot start a profiling interval");
            c.ip=0x3ac9; c.cr[0]=0;
            check(w.observe(c)==QuakeProfileWindow::Event::None,"real-mode aliases cannot trigger the Quake window");
            c.cr[0]=1;
            check(w.observe(c)==QuakeProfileWindow::Event::Begin&&w.beginFrame==101,"profile starts after the excluded first frame");
            check(w.observe(c)==QuakeProfileWindow::Event::None,"begin event is emitted only once");
            c.ip=0x3f86;c.write(0x10000+0x95bb4,151,4);
            check(w.observe(c)==QuakeProfileWindow::Event::End&&w.endFrame-w.beginFrame==50,"profile records exactly fifty completed frames");
            check(w.observe(c)==QuakeProfileWindow::Event::None,"end event is emitted only once");
            QuakeProfileWindow bad;c.ip=0x3ac9;
            bool rejected=false;
            try{bad.observe(c);}catch(const std::runtime_error &){rejected=true;}
            check(rejected,"wrong warmup frame rejects a misleading profile");
            c.write(0x10000+0x95bb4,101,4);c.write(0x13ac3,0x90,1);rejected=false;
            try{bad.observe(c);}catch(const std::runtime_error &){rejected=true;}
            check(rejected,"changed instruction fingerprints reject the profile map");
        }
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
