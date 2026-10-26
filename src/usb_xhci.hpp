#pragma once
#include <array>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <vector>
namespace pc {
// xHCI 1.0-compatible register subset, referenced against Intel 625472 rev 1.2b
// (April 2023), chapters 5-7; newer optional features are not advertised.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=347
// HID 1.11 (June 27 2001), report/control protocol for three fixed USB full-speed
// devices. https://www.usb.org/sites/default/files/documents/hid1_11.pdf#page=61
// Three fixed USB2 HID devices: keyboard, relative mouse and absolute tablet. Single interrupter,
// 32-byte contexts, INTx. All guest DMA and ring walks are bounded; serviced on the VM owning
// thread.
class UsbXhci {
  public:
    // Bind RAM/INTx delivery and create the halted controller with three connected ports.
    UsbXhci(std::span<uint8_t> ram, uint64_t base, std::function<void(bool)> irq);
    // Read PCI identity, command, BAR probe and INTA configuration bytes.
    uint64_t configRead(uint64_t offset, unsigned width) const;
    // Store supported PCI Command/BAR writes and recompute INTx.
    void configWrite(uint64_t offset, uint64_t value, unsigned width);
    // Test enabled BAR decoding within the board-approved MMIO aperture.
    bool contains(uint64_t address, unsigned width) const;
    // Read a naturally aligned MMIO access as little-endian register bytes.
    uint64_t read(uint64_t address, unsigned width);
    // Apply dword/qword MMIO writes; bounded DMA failures halt/latch a host error.
    void write(uint64_t address, uint64_t value, unsigned width);
    // Update one HID keyboard usage and queue a complete boot-style keyboard report.
    void receive_key_usage(uint16_t usage, bool released);
    // Split host relative motion/wheel into signed-byte mouse reports.
    void receive_relative_motion(int dx, int dy, int wheel, uint8_t buttons);
    // Scale host absolute coordinates and queue tablet/wheel reports.
    void receive_absolute_motion(int x, int y, int wheel, uint8_t buttons);
    // Release held keys/buttons while retaining the tablet position.
    void release_all_input();
    uint64_t commands = 0, transfers = 0, reports = 0;
    // Describe bounded host input and active-slot metadata for diagnostics.
    std::string input_state() const;
    // Return the last host validation failure latched while servicing this controller.
    // IMPLEMENTATION NOTE: Host diagnostics around xHCI system-error handling.
    // https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=362
    const std::string &last_error() const {
        return failure;
    }

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    // IMPLEMENTATION NOTE: Fixed topology/capability limits, advertised in HCSPARAMS1.
    // xHCI 5.3.3 Structural Parameters 1.
    // https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=348
    static constexpr unsigned MaximumSlots = 8, RootPortCount = 3, EndpointContextCount = 32;
    // Each device context has slot plus 31 endpoint contexts; 32-byte CSZ=0 format.
    static constexpr unsigned ContextBytes = 32;
    // Host HID state bounds; keyboard usage byte and report descriptor define this range.
    static constexpr unsigned KeyboardUsageCount = 256, InitialTabletCoordinate = 16384;
    struct Ring {
        uint64_t pointer = 0;
        bool cycle = true;
    };
    struct Slot {
        bool enabled = false;
        unsigned port = 0;
        std::array<Ring, EndpointContextCount> endpoints{};
        std::array<bool, EndpointContextCount> active{};
        std::vector<uint8_t> control;
        std::array<uint32_t, EndpointContextCount> actual{};
        std::array<bool, EndpointContextCount> shortPacket{};
        bool controlIn = false;
        unsigned configuration = 0, protocol = 1, idle = 0;
    };
    std::span<uint8_t> ram;
    uint64_t ramBase;
    std::function<void(bool)> interrupt;
    uint32_t bar = 0, pciCommand = 0, usbcmd = 0, status = 1, iman = 0;
    uint32_t erstSize = 0, config = 0, imod = 0;
    uint64_t crcr = 0, dcbaa = 0, erst = 0, erdp = 0;
    uint64_t eventBase = 0, eventIndex = 0, eventCount = 0, outstanding = 0;
    bool eventCycle = true, probe = false;
    Ring commandRing;
    std::array<Slot, MaximumSlots + 1> slots{};
    std::array<uint32_t, RootPortCount> ports{};
    std::array<std::deque<std::vector<uint8_t>>, RootPortCount> pending;
    std::array<bool, KeyboardUsageCount> keys{};
    uint8_t buttons = 0;
    unsigned tabletX = InitialTabletCoordinate, tabletY = InitialTabletCoordinate;
    std::string failure;
    uint64_t keyInputs = 0, pointerInputs = 0;
    // Read a validated little-endian scalar from guest RAM.
    uint64_t read_guest_value(uint64_t address, unsigned width) const;
    // Write a validated little-endian scalar into guest RAM.
    void write_guest_value(uint64_t address, uint64_t value, unsigned width);
    // Reject any DMA interval outside the supplied contiguous guest RAM.
    void validate_guest_range(uint64_t address, uint64_t length) const;
    // Reset runtime/rings/input state while preserving PCI configuration and activity counters.
    void reset_controller();
    // Drive INTx from PCI interrupt disable, USBCMD.INTE and IMAN.IP/IE.
    void update_intx();
    // Publish a cycle-tagged event to the sole segment and latch its interrupt.
    void enqueue_event(uint64_t parameter, uint32_t status, uint32_t control);
    // Read the next owned TRB, following bounded Link TRBs and cycle toggles.
    bool next_transfer_ring_block(Ring &ring, std::array<uint32_t, 4> &trb, uint64_t &address);
    // Dispatch command-ring or supported endpoint work after an accepted doorbell.
    void ring_doorbell(unsigned slot, unsigned endpoint);
    // Process bounded slot/context/endpoint commands and publish completions.
    void process_command_ring();
    // Service control or interrupt-IN TRBs, retaining an empty input queue as NAK.
    void process_transfer_ring(unsigned slot, unsigned endpoint);
    // Decode the fixed devices' standard/HID control request and prepare its response.
    void execute_setup_request(Slot &slot, uint64_t packet);
    // Bound pending host reports and resume an active interrupt-IN endpoint.
    void queue_input_report(unsigned port, std::vector<uint8_t> report);
    // Publish slot/endpoint state and dequeue pointer to the output device context.
    void write_endpoint_context(unsigned slot, unsigned endpoint, unsigned state);
    // Read named capability, operational, port, runtime or protocol registers.
    uint32_t read_register(unsigned offset) const;
    // Apply named operational/port/runtime register side effects.
    void write_register(unsigned offset, uint32_t value);
};
} // namespace pc
