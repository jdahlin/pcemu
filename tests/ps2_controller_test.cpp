#include "i8042.hpp"
#include "pc_input.hpp"
#include <iostream>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool condition) {
    ++checks;
    if (!condition) throw std::runtime_error("PS/2 controller check " + std::to_string(checks));
}
int main() {
    const I8042Device::IrqState ready{false, false, false, false};
    I8042Device controller;
    controller.enqueueScan(0x1e); controller.enqueueScan(0x9e);
    auto status = controller.readStatus(0, ready);
    check((status.value & 1) && status.irqs.keyboard && !status.irqs.mouse);
    auto byte = controller.readData(0, ready);
    check(byte.value == 0x1e && byte.full && !byte.irqs.keyboard);
    check(controller.readData(3999, ready).value == 0x1e && !controller.state().keyboardFull);
    check(!(controller.readStatus(3999, ready).value & 1));
    status = controller.readStatus(4000, ready);
    check((status.value & 1) && status.irqs.keyboard);
    check(controller.readData(4000, ready).value == 0x9e);
    controller.reset(); controller.enqueueScan(0x30);
    check(!controller.poll(0, {true, false, false, false}).keyboard);
    check(!controller.state().keyboardFull && controller.state().scanCodes.size() == 1);
    check(!controller.poll(0, {false, true, false, false}).keyboard);
    check(!controller.state().keyboardFull);
    check(controller.poll(0, ready).keyboard);
    check(!controller.poll(0, ready).keyboard); // Only a new edge raises PIC input.
    controller.readData(0, ready);
    controller.reset(); controller.enqueueScan(0x20);
    controller.write(0xed);
    check(controller.poll(0, {true, true, false, false}).keyboard); // Replies bypass scan gating.
    check(controller.readData(0, ready).value == 0xfa);
    controller.write(7);
    check(controller.state().keyboardLEDs == 7 && controller.state().scanCodes.size() == 1);
    check(controller.readData(4000, ready).value == 0xfa);
    check(controller.readData(8000, ready).value == 0x20);
    controller.reset();
    controller.command(0xd1, true);
    auto effects = controller.write(0);
    check(effects.a20.has_value() && !*effects.a20 && effects.reset);
    controller.command(0xd1, false);
    effects = controller.write(3);
    check(effects.a20.has_value() && *effects.a20 && !effects.reset);
    controller.command(0xd0, false);
    check(controller.readData(0, ready).value == 1); // Board A20 is sampled, not cached.
    check(controller.command(0xfe, true).reset);
    check(!controller.command(0xff, true).reset);
    controller.reset();
    controller.command(0x60, true); controller.write(3);
    controller.command(0xd4, true); controller.write(0xf4);
    status = controller.readStatus(0, ready);
    check((status.value & 0x21) == 0x21 && status.irqs.mouse && !status.irqs.keyboard);
    check(controller.readData(0, ready).value == 0xfa);
    controller.motion(5, -3, 1);
    check(controller.readStatus(4000, ready).irqs.mouse);
    auto saved = controller; // Controller state is independent of board pointers/callbacks.
    check(saved.readData(4000, ready).value == controller.readData(4000, ready).value);
    controller.reset();
    check(controller.state().scanCodes.empty() && controller.state().keyboardReplies.empty() &&
          !controller.state().keyboardFull && !controller.mouse().enabled);
    enqueuePcKey(controller, 'A');
    check(controller.state().scanCodes == std::deque<uint8_t>({0x2a, 0x1e, 0x9e, 0xaa}));
    std::cout << checks << " standalone PS/2 controller checks passed\n";
}
