#include "session_diagnostics.hpp"
#include "machine.hpp"
#include "machine_profile.hpp"
#include "jit.hpp"
#include <cstdio>
#include <iomanip>
#include <iostream>
namespace pc {
void dumpCpu(CPU &c) {
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
void writeMachineState(std::ostream &j, Machine &m, const Jit &jit, const CaptureContext &context) {
    auto &c = m.cpu;
    const auto &selectedProfile = context.profile;
    const auto executionWallNs = context.executionWallNs;
    const auto enableJit = context.enableJit, followJumps = context.followJumps,
               traceJit = context.traceJit, enableHostInput = context.enableHostInput;
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
      << ",\"jitDirectLinks\":" << jit.directLinksEnabled()
      << ",\"directLinkTransitions\":" << jit.directLinkTransitions
      << ",\"directLinkPromotions\":" << jit.directLinkPromotions
      << ",\"directLinkBlocks\":" << jit.directLinkBlocks
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
      << ",\"absoluteMouse\":{\"active\":" << m.absoluteMouse.active()
      << ",\"queuedWords\":" << m.absoluteMouse.count << "}"
      << ",\"hostCursor\":{\"active\":" << m.hostCursor.active
      << ",\"visible\":" << m.hostCursor.visible << ",\"size\":" << m.hostCursor.size
      << ",\"revision\":" << m.hostCursor.revision
      << ",\"shapeHash\":" << m.hostCursor.signature()
      << ",\"hotX\":" << HostCursor::word(m.hostCursor.shape,0)
      << ",\"hotY\":" << HostCursor::word(m.hostCursor.shape,2) << "}"
      << ",\"mouse\":{\"enabled\":" << m.ps2.state().mouse.enabled
      << ",\"id\":" << unsigned(m.ps2.state().mouse.id)
      << ",\"pendingPackets\":" << m.ps2.state().mouse.packets.size()
      << ",\"auxOutput\":" << (m.ps2.state().keyboardFull && m.ps2.state().keyboardAux) << "}"
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
      << "\"full\":" << m.ps2.state().keyboardFull << ",\"data\":" << unsigned(m.ps2.state().keyboardData)
      << ",\"pendingScans\":" << m.ps2.state().scanCodes.size()
      << ",\"pendingReplies\":" << m.ps2.state().keyboardReplies.size()
      << ",\"commandByte\":" << unsigned(m.ps2.state().keyboardCommandByte) << "}}";
}
} // namespace pc
