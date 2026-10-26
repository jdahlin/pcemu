#include "virtio_balloon.hpp"
#include "virtio_pci.hpp"
#include "virtio_tool_fixture.hpp"
#include <iostream>
using namespace pc;
using namespace tooltest;
int main() {
    Memory m;
    bool irq = false;
    unsigned discards = 0;
    VirtioBalloon balloon(
        m.ram, m.base, [&](bool level, unsigned) { irq = level; }, 16384,
        [&](uint64_t address, size_t length) {
            check(address == m.base + 0x40000 && length == 16384);
            ++discards;
            return true;
        });
    auto setup = [&] {
        m.configure(balloon, 3, 3);
        discards = 0;
    };
    auto pages = [&](unsigned q, std::initializer_list<unsigned> offsets) {
        unsigned i = 0;
        for (auto offset : offsets)
            m.put(0x20000 + 4 * i++, (m.base + offset) / 4096, 4);
        m.descriptor(q, 0, 0x20000, offsets.size() * 4, 0);
        m.submit(balloon, q);
    };
    setup();
    balloon.setTargetPages(64);
    check(balloon.read(0x100) == 64 && balloon.read(0xfc) == 1 && irq);
    balloon.write(0x64, 2);
    check(!irq);
    pages(0, {0x40000, 0x41000, 0x42000});
    check(discards == 0 && balloon.inflatedPages() == 3);
    pages(0, {0x43000});
    check(discards == 1 && balloon.inflatedPages() == 4 && balloon.discardBytes == 16384);
    pages(0, {0x43000, 0x43000});
    check(discards == 1 && balloon.inflatedPages() == 4);
    balloon.write(0x104, 4);
    check(balloon.actualPages() == 4 && balloon.read(0x104, 2) == 4);
    pages(1, {0x41000});
    check(balloon.inflatedPages() == 3);
    pages(0, {0x41000});
    check(discards == 2);
    balloon.setTargetPages(0);
    check(balloon.read(0x100) == 0 && (balloon.read(0x60) & 2));
    pages(1, {0x40000, 0x41000, 0x42000, 0x43000});
    check(balloon.inflatedPages() == 0);
    // Packed 10-byte statistics, including an unknown tag, and pull/requeue semantics.
    m.put(0x21000, 4, 2);
    m.put(0x21002, 123456789, 8);
    m.put(0x2100a, 99, 2);
    m.put(0x2100c, UINT64_MAX, 8);
    m.descriptor(2, 0, 0x21000, 20, 0);
    m.submit(balloon, 2);
    check(m.get(m.used(2) + 2, 2) == 0 && balloon.statistics().at(4) == 123456789);
    balloon.requestStats();
    check(m.get(m.used(2) + 2, 2) == 1 && balloon.statistics().at(99) == UINT64_MAX);
    m.put(0x21002, 42, 8);
    m.submit(balloon, 2);
    check(balloon.statistics().at(4) == 42 && m.get(m.used(2) + 2, 2) == 1);
    // Invalid late PFNs must not release even valid earlier pages.
    setup();
    pages(0, {0x40000, 0x41000, 0x42000, unsigned(m.ram.size())});
    check(!balloon.error().empty() && balloon.inflatedPages() == 0 && discards == 0);
    setup();
    m.descriptor(0, 0, 0x20000, 3, 0);
    m.submit(balloon, 0);
    check(!balloon.error().empty());
    setup();
    m.descriptor(1, 0, 0x20000, 4, 2);
    m.submit(balloon, 1);
    check(!balloon.error().empty());
    setup();
    m.descriptor(2, 0, 0x21000, 11, 0);
    m.submit(balloon, 2);
    check(!balloon.error().empty());
    setup();
    balloon.write(0x104, UINT32_MAX);
    check(!balloon.error().empty());
    setup();
    bool refused = false;
    try {
        balloon.setTargetPages(UINT32_MAX);
    } catch (const std::runtime_error &) {
        refused = true;
    }
    check(refused);
    // Drivers that omit MUST_TELL_HOST can coordinate memory, but their pages
    // may be reused before notification; never issue discard hints for them.
    m.configure(balloon, 3, 2);
    discards = 0;
    pages(0, {0x40000, 0x41000, 0x42000, 0x43000});
    check(balloon.error().empty() && balloon.inflatedPages() == 4 && discards == 0);
    setup();
    VirtioPciDevice pci([&](unsigned o, unsigned w) { return balloon.read(o, w); },
                        [&](unsigned o, uint64_t v, unsigned w) { balloon.write(o, v, w); }, 5,
                        0x05800001, 3, [](bool) {});
    check(pci.configRead(0, 4) == 0x10451af4);
    pci.configWrite(0x10, 0x10000000, 4);
    pci.configWrite(4, 6, 2);
    balloon.setTargetPages(16);
    check(pci.read(0x10000015, 1) == balloon.read(0xfc));
    check(pci.read(0x10003000, 4) == 16);
    pci.write(0x10003004, 12, 4);
    check(balloon.actualPages() == 12);
    pci.write(0x10003000, 99, 4);
    check(balloon.targetPages() == 16);
    balloon.write(0x70, 0);
    check(balloon.actualPages() == 0 && balloon.targetPages() == 16 &&
          balloon.statistics().empty());
    VirtioBalloon legacy(m.ram, m.base, [](bool, unsigned) {}, 4096, {}, true);
    m.configure(legacy, 3, 2);
    m.put(0x20000, (m.base + 0x40000) / 4096, 8);
    m.put(0x20008, (m.base + 0x41000) / 4096, 8);
    m.descriptor(0, 0, 0x20000, 16, 0);
    m.submit(legacy, 0);
    check(legacy.error().empty() && legacy.inflatedPages() == 2);
    m.descriptor(1, 0, 0x20000, 16, 0);
    m.submit(legacy, 1);
    check(legacy.inflatedPages() == 0);
    m.put(0x20000, UINT64_MAX, 8);
    m.submit(legacy, 0);
    check(!legacy.error().empty());
    std::cout << checks << " virtio balloon checks passed\n";
}
