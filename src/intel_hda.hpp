#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
namespace pc {
// Intel ICH9 identity, Intel High Definition Audio Specification rev. 1.0a
// (17 June 2010), chapters 3 and 7. One playback stream and one fixed speaker.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=26
// The board owns time/INTx routing; a host sink receives stereo 48 kHz signed PCM.
class IntelHda {
  public:
    using PlaybackSink = std::function<void(std::span<const int16_t>)>;
    // Bind bounded guest RAM, a level interrupt output and an optional PCM sink.
    IntelHda(std::span<uint8_t> ram, uint64_t ram_base, std::function<void(bool)> interrupt,
             PlaybackSink sink = {});
    // Read/write PCI identity, Command and a 16 KiB 64-bit BAR.
    uint64_t read_configuration(uint64_t offset, unsigned width) const;
    void write_configuration(uint64_t offset, uint64_t value, unsigned width);
    // Test enabled BAR decoding in the board PCI aperture.
    bool contains(uint64_t address, unsigned width) const;
    // Transfer byte/word/DWORD register lanes relative to the mapped BAR address.
    uint64_t read_register(uint64_t address, unsigned width) const;
    void write_register(uint64_t address, uint64_t value, unsigned width);
    // Describe guest command/playback activity and register state without mutation.
    std::string diagnostics() const;
    // Advance link time and playback by elapsed host microseconds, with VPs stopped.
    void service_elapsed_microseconds(uint64_t microseconds);

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    std::span<uint8_t> ram;
    uint64_t ram_base;
    std::function<void(bool)> interrupt;
    PlaybackSink sink;
    std::array<uint8_t, 256> registers{};
    uint64_t bar = 0;
    uint16_t pci_command = 0;
    bool bar_probe = false, bar_upper_probe = false;
    uint64_t wall_ticks = 0, frame_fraction = 0;
    uint64_t codec_commands = 0, playback_frames = 0, output_frames = 0, nonzero_samples = 0;
    unsigned descriptor_index = 0, descriptor_offset = 0, response_count = 0;
    uint8_t converter_stream_channel = 0, pin_control = 0x40;
    uint16_t converter_format = 0x11;
    std::array<uint8_t, 4> power_state{};
    // Read/store internal little-endian register fields without bus side effects.
    uint64_t load_register(unsigned offset, unsigned width) const;
    void store_register(unsigned offset, uint64_t value, unsigned width);
    // Resolve a complete DMA span, rejecting overflow and addresses outside RAM.
    std::span<uint8_t> dma_memory(uint64_t address, size_t bytes);
    // Restore controller/link registers and codec state; PCI setup survives CRST.
    void reset_controller();
    // Decode one codec verb for root/AFG/DAC/speaker nodes; absent codecs do not reply.
    uint32_t execute_codec_verb(uint32_t command);
    // Consume a bounded CORB batch and publish solicited RIRB responses in guest RAM.
    void process_command_ring();
    // Consume playback buffer descriptors, update LPIB/position DMA and completion status.
    void advance_output_stream(unsigned frames);
    // Derive pending controller/stream causes and drive PCI INTx after all masks.
    uint32_t interrupt_status() const;
    void update_interrupt_output();
};
} // namespace pc
