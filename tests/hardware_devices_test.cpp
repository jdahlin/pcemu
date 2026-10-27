#include "ata.hpp"
#include "disk.hpp"
#include "i8259_pair.hpp"
#include "pit.hpp"
#include "i8237.hpp"
#include "sound_blaster.hpp"
#include "vga.hpp"
#include "pci.hpp"
#include "ne2000.hpp"
#include "network.hpp"
#include "system_port_b.hpp"
#include "port92.hpp"
#include "debug_console.hpp"
#include "arm_pl011.hpp"
#include "arm_pl031.hpp"
#include "arm_nor_flash.hpp"
#include "arm_fw_cfg.hpp"
#include "arm_ramfb.hpp"
#include "virtio_blk.hpp"
#include "virtio_net.hpp"
#include "virtio_rng.hpp"
#include "nvme.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <unistd.h>
using namespace pc;
static unsigned checks;
static void check(bool condition, const std::source_location location=std::source_location::current()) {
    ++checks;
    if (!condition) throw std::runtime_error("Hardware device check " + std::to_string(checks)+" at line "+std::to_string(location.line()));
}
int main() {
    auto root = std::filesystem::temp_directory_path() / ("pcemu-devices-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    try {
        {
            std::ofstream raw(root / "disk.raw", std::ios::binary);
            raw.seekp(64 * 512 - 1); raw.put(0);
        }
        Disk disk((root / "disk.raw").string());
        ATA ata(&disk);
        ata.write_register(0x1f7, 0xec, 1);
        check(ata.interrupt_level() && (ata.read_register(0x3f6, 1) & 8));
        check(ata.interrupt_level());
        check(ata.read_register(0x1f7, 1) & 8); check(!ata.interrupt_level());
        check(ata.read_register(0x1f0, 2) == 0x40);
        for (unsigned i = 1; i < 256; ++i) ata.read_register(0x1f0, 2);
        ata.write_register(0x1f2, 1, 1); ata.write_register(0x1f3, 2, 1); ata.write_register(0x1f6, 0xe0, 1);
        ata.write_register(0x1f7, 0x30, 1);
        for (unsigned i = 0; i < 256; ++i) ata.write_register(0x1f0, 0x5a5a, 2);
        std::array<uint8_t, 512> sector{};
        check(disk.read(2, 1, sector.data()) && sector.front() == 0x5a && sector.back() == 0x5a);
        check(ata.interrupt_level()); ata.write_register(0x3f6, 2, 1); check(!ata.interrupt_level());
        ata.reset(&disk); check(!ata.interrupt_level());

        I8259PairDevice pic;
        pic[0].base = 0x20; pic[1].base = 0x28;
        pic[0].mask = 0xfb; pic[1].mask = 0xbf;
        pic.raise_interrupt(14);
        check(!pic.next_interrupt_vector(false) && (pic[0].irr & 4));
        check(pic.next_interrupt_vector(true) == 0x2e && (pic[0].isr & 4) && (pic[1].isr & 0x40));
        check(pic.state().ataInService);
        pic.write_register(1, false, 0x20); pic.write_register(0, false, 0x20);
        check(!pic.state().ataInService && pic[0].isr == 0);
        pic[0].mask = 0xfe; pic.raise_interrupt(0);
        check(!pic.next_interrupt_vector(false) && pic.state().timerPending);
        check(pic.next_interrupt_vector(true) == 0x20 && pic.state().timerInService);
        pic.reset(); check(pic[0].mask == 0xff && !pic.state().cascadeLine);
        PIT8254 pit; pit.reset(1234); check(pit.counters[0].loadedAt == 1234);
        pit.write_register(3, 0x36, 2000);
        pit.write_register(0, 0xff, 2000);
        pit.write_register(0, 0xff, 2000);
        check(pit.counters[0].counter_value(3000) == 63536);
        pit.write_register(3, 0x36, 3000);
        check(pit.counters[0].loadedAt == 2000 && pit.counters[0].nullCount);
        check(pit.read_counter(0, 3000) == 0x30 && pit.read_counter(0, 3000) == 0xf8);
        pit.write_register(0, 0x34, 3001);
        check(pit.counters[0].loadedAt == 2000);
        pit.write_register(0, 0x12, 3002);
        check(pit.counters[0].loadedAt == 3002 && pit.counters[0].reload == 0x1234 &&
              !pit.counters[0].nullCount);
        I8237Device dma;
        auto pair=[&](unsigned port,uint16_t value){
            dma.write_register(0x0c,0);dma.write_register(port,value);dma.write_register(port,value>>8);
        };
        pair(2,0xffff);pair(3,1);
        dma.state.channels[1].page=0x12;
        dma.write_register(0x0b,0x49);dma.set_request(1,true);
        check(!dma.acknowledge_memory_read(1)); // Masked after reset.
        dma.write_register(0x0a,1);
        auto byte=dma.acknowledge_memory_read(1);
        check(byte && byte->address==0x12ffff && !byte->terminalCount);
        byte=dma.acknowledge_memory_read(1);
        check(byte && byte->address==0x120000 && byte->terminalCount && dma.state.mask==15);
        check(dma.state.channels[1].count==0xffff && !dma.acknowledge_memory_read(1));
        check(dma.read_register(8)==0x22 && dma.read_register(8)==0x20);
        pair(2,0x3456);pair(3,0);dma.write_register(0x0b,0x59);dma.write_register(0x0a,1);
        for(unsigned i=0;i<3;++i) {
            byte=dma.acknowledge_memory_read(1);
            check(byte && byte->address==0x123456 && byte->terminalCount && !(dma.state.mask&2));
        }
        check(dma.state.channels[1].address==0x3456 && dma.state.channels[1].count==0);
        pair(2,0);pair(3,1);dma.write_register(0x0b,0x69);
        byte=dma.acknowledge_memory_read(1);check(byte && byte->address==0x120000);
        byte=dma.acknowledge_memory_read(1);check(byte && byte->address==0x12ffff && byte->terminalCount);
        dma.write_register(0x0a,1);dma.write_register(0x0b,0x45); // Wrong transfer direction.
        check(!dma.acknowledge_memory_read(1));
        dma.write_register(0x0b,0xc9);check(!dma.acknowledge_memory_read(1)); // Cascade.
        dma.write_register(0x0b,0x49);dma.write_register(8,4);
        check(!dma.acknowledge_memory_read(1)); // Controller disabled.
        dma.write_register(8,0);dma.set_request(1,false);dma.write_register(9,5);
        check(!dma.acknowledge_memory_read(1)); // Software request needs block mode.
        dma.write_register(0x0b,0x89);check(bool(dma.acknowledge_memory_read(1)));
        pair(2,0);
        dma.write_register(0x0c,0);dma.write_register(0,0x12);dma.write_register(2,0x34);
        check(dma.state.channels[0].address==0x12 && dma.state.channels[1].address==0x3400);
        dma.write_register(0x0c,0);check(dma.read_register(0)==0x12 && dma.read_register(2)==0x34);
        auto savedAddress=dma.state.channels[1].address;
        dma.set_request(1,true);
        dma.write_register(0x0d,0);
        check(dma.state.mask==15 && !dma.state.command && !dma.state.terminalCount &&
              !dma.state.softwareRequest && !dma.state.highByte && dma.state.channels[1].address==savedAddress);
        check(dma.read_register(8)==0x20 && !dma.acknowledge_memory_read(1));
        {
            I8237Device audioDma;SoundBlasterDevice sb;
            std::array<uint8_t,16> ram{0,128,255,128,64,192,128,128};
            auto write=[&](uint8_t v){sb.write_register(12,v,sb.state.last_ticks);};
            auto dmaPair=[&](unsigned port,uint16_t v){audioDma.write_register(12,0);audioDma.write_register(port,v);audioDma.write_register(port,v>>8);};
            auto advanceUs=[&](uint64_t us){sb.advance_sample_clock((us*1193182+999999)/1000000,audioDma,ram);};
            sb.write_register(6,1,0);sb.write_register(6,0,0);
            check(sb.read_register(14)==0 && sb.read_register(12)==0x80);
            sb.advance_sample_clock(119,audioDma,ram);check(sb.read_register(14)==0);
            sb.advance_sample_clock(120,audioDma,ram);check(sb.read_register(14)==0x80 && sb.read_register(10)==0xaa);
            write(0xe1);check(sb.read_register(10)==3 && sb.read_register(10)==2 && sb.read_register(10)==0xff);
            write(0xe0);write(0x35);check(sb.read_register(10)==0xca);
            write(0xe4);write(0x5a);write(0xe8);check(sb.read_register(10)==0x5a);
            dmaPair(2,0);dmaPair(3,7);audioDma.write_register(11,0x59);audioDma.write_register(10,1);
            write(0x40);write(156);write(0x48);write(3);write(0);write(0xd1);write(0x1c);
            // Reset released at tick 0; playback starts at tick 120 (~100 us).
            advanceUs(510);
            check(sb.counters.dma_bytes==4 && sb.counters.blocks==1 && sb.state.irq_pending);
            check(sb.state.remaining==4 && audioDma.state.channels[1].address==4 && sb.counters.non_silent_frames>0);
            auto blocks=sb.counters.blocks;sb.read_register(14);check(!sb.state.irq_pending && sb.counters.blocks==blocks);
            advanceUs(910);
            check(sb.counters.dma_bytes==8 && sb.counters.blocks==2 && audioDma.state.channels[1].address==0);
            write(0xd0);advanceUs(1910);check(sb.counters.dma_bytes==8);
            write(0xd4);write(0xda);advanceUs(2310);
            check(sb.counters.dma_bytes==12 && sb.counters.blocks==3 && !sb.state.active);
            // Speaker mute does not suppress DMA completion or its interrupt.
            write(0xd3);write(0x14);write(0);write(0);
            auto nonSilent=sb.counters.non_silent_frames;advanceUs(2420);
            check(sb.counters.dma_bytes==13 && sb.counters.blocks==4 && sb.counters.non_silent_frames==nonSilent);
            // A high-speed auto-init block ignores ordinary stop until DSP reset.
            write(0x90);write(0xd0);check(!sb.state.paused && sb.read_register(12)==0x80);
            sb.write_register(6,1,sb.state.last_ticks);sb.advance_sample_clock(sb.state.last_ticks,audioDma,ram);
            check(!sb.state.active && !sb.state.irq_pending && audioDma.state.hardwareRequest==0);
            sb.write_register(6,0,sb.state.last_ticks);advanceUs(2540);
            check(sb.read_register(10)==0xaa && sb.counters.dma_bytes==13);
        }
        {
            std::vector<uint8_t> ram(65536,129);
            I8237Device d1,d2;SoundBlasterDevice s1,s2;
            auto prepare=[&](I8237Device &d,SoundBlasterDevice &s){
                d.write_register(12,0);d.write_register(2,0);d.write_register(2,0);
                d.write_register(12,0);d.write_register(3,255);d.write_register(3,255);
                d.write_register(11,0x59);d.write_register(10,1);
                for(uint8_t v:{0x40,165,0xd1,0x1c})s.write_register(12,v,0);
            };
            prepare(d1,s1);prepare(d2,s2);
            s1.advance_sample_clock(1193182*3/2,d1,ram);
            s2.advance_sample_clock(1193182/2,d2,ram);s2.advance_sample_clock(1193182*3/2,d2,ram);
            check(s1.counters.dma_bytes==16483 && s2.counters.dma_bytes==16483 &&
                  s1.state.phase==s2.state.phase && s1.counters.pcm_hash==s2.counters.pcm_hash);
            check(s1.output.size()<=96000 && s1.counters.dropped_frames>0);
            I8237Device d;SoundBlasterDevice s;
            prepare(d,s);s.state.active=false;
            check(s.state.mixer[4]==0x88 && s.state.mixer[0x22]==0x88);
            s.write_register(4,4,0);s.write_register(5,0xee,0);
            s.write_register(4,0x22,0);s.write_register(5,0xee,0);
            s.write_register(4,0x0e,0);s.write_register(5,2,0);
            for(uint8_t v:{0x40,156,0x14,1,0})s.write_register(12,v,0);
            ram[0]=0;ram[1]=255;s.advance_sample_clock(120,d,ram);
            check(s.counters.dma_bytes==1 && s.output[0]==-32768 && s.output[1]==0);
            s.advance_sample_clock(240,d,ram);
            check(s.counters.dma_bytes==2 && s.output[s.output.size()-2]==-32768 && s.output.back()==32512);
        }
        SystemPortBDevice portB;
        check(portB.write_register(0xcf) && portB.read_register(false, false) == 0xcf);
        check(portB.read_register(true, true) == 0xff);
        portB.reset(); check(portB.read_register(false, true) == 0x20);
        portB.restore(1); check(portB.read_register(false, false) == 1);
        Port92Device fast;
        check(fast.read_register(false) == 0 && fast.read_register(true) == 2);
        check(fast.write_register(3).a20 && fast.write_register(3).reset && !fast.write_register(0).a20);
        std::string output;
        DebugConsoleDevice debug;
        debug.bind_output_sink([&](uint8_t value) { output.push_back(char(value)); });
        debug.write_data_register('A'); debug.restore('B');
        check(output == "A" && debug.read_data_register() == 'B');
        debug.reset(); check(debug.read_data_register() == 0 && output == "A");
        VGA vga;
        check(vga.read_io_register(0x3da, 1) == 0 && vga.read_io_register(0x3da, 2) == 0);
        vga.write_display_interface_port(false, 1); vga.write_display_interface_port(true, 800);
        check(vga.read_display_interface_port(true) == 800);
        PCI bus;
        bus.write_io_register(0xcf8, 0x80001000, 4); check(bus.read_io_register(0xcfc, 4) == 0x11111234);
        bus.write_io_register(0xcf8, 0x80001010, 4); bus.write_io_register(0xcfc, 0xffffffffu, 4);
        check(bus.display().barProbe && bus.read_io_register(0xcfc, 4) == 0xff000000u);
        bus.reset(); check(!bus.address && !bus.display().barProbe);
        NE2000 nic; check(!nic.interrupt_level()); nic.reset();
        Network gateway; // Host packet backend also links without an execution engine.
        ArmPl011 uart; uart.write_register(0x30, 0x301); uart.write_register(0, 'Z'); check(uart.transmitted_output() == "Z");
        ArmPl031 rtc([] { return int64_t(1000); }); check(rtc.read_register(0) == 1000);
        std::vector<uint8_t> flashBytes(0x40000, 0xff), ram(65536);
        ArmNorFlash flash(flashBytes); check(flash.bytes().front() == 0xff);
        ArmFwCfg config(ram, 0x40000000); config.add_file("etc/ramfb", std::vector<uint8_t>(28), true);
        ArmRamFb fb(config, ram, 0x40000000); check(fb.mode_description() == "inactive");
        auto irq = [](bool) {};
        VirtioBlock block(disk, ram, 0x40000000, irq);
        VirtioNet network(ram, 0x40000000, irq);
        VirtioRng entropy(ram, 0x40000000, irq);
        Nvme nvme(disk, ram, 0x40000000, irq);
        check(block.read(0) == 0x74726976 && network.read(0) == 0x74726976 && entropy.read(0) == 0x74726976);
        check((nvme.configRead(0, 4) & 0xffff) == 0x1b36);
        std::filesystem::remove_all(root);
        std::cout << checks << " standalone hardware device checks passed (no CPU/machine backend)\n";
    } catch (...) { std::filesystem::remove_all(root); throw; }
}
