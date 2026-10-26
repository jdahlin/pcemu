# ICH9 HD Audio playback on ARM64 Windows

`./run11.sh` enables the ICH9-compatible PCI High Definition Audio (HDA)
controller and host playback. Restart the guest to discover this new PCI device.
The native macOS VM library uses the same launcher. Audio can also be enabled
on a direct ARM64 UEFI invocation:

```sh
build/pcemu-hvf --machine arm64-uefi --audio ich9 --audio-backend sdl ...
```

`--audio none` disables the device; this is the direct runner's default.
`--audio-backend null` keeps the guest-visible controller and consumes playback
without opening a host audio device. `--audio-backend sdl` is the playback
default and works independently of the display frontend. SPICE clients do not
receive an audio stream; sound plays on the emulator host. Use
`./run11.sh --audio none` to override the launcher's audio default.

The PCI function identifies as Intel `8086:293e`, revision 3, multimedia HDA class
`04:03:00`, corresponding to QEMU's `ich9-intel-hda` identity. QEMU's `intel-hda`
name instead denotes its ICH6 identity; these are alternative controllers.
The device model is original code; no QEMU device implementation is linked.

## Specification and model boundaries

The hardware interface follows Intel's **High Definition Audio Specification,
revision 1.0a, 17 June 2010**. The published specification has no separate
document number. The implementation links each function to the relevant
sections using physical PDF page numbers, including the front matter.

- [Controller registers, section 3.3, Table 2](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=28).
- [DMA position and buffer descriptor layouts, sections 3.6.1–3.6.3](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=57).
- [Command/response ring layouts, sections 3.6.4–3.6.5](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=58).
- [Codec controls, section 7.3.3](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=143).
- [Codec discovery parameters, section 7.3.4](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=200).
- [PCI type-0 configuration, PCI Local Bus Specification 3.0, sections 6.2.1/6.2.2/6.2.5](https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=216).
- [ICH9 discovery identity in QEMU](https://github.com/qemu/qemu/blob/master/hw/audio/intel-hda.c).

This is a playback subset, not full ICH9 chipset emulation. One output stream
supports stereo signed 16-bit PCM at 48 kHz. There are no input or bidirectional
streams. The codec graph is root node 0, Audio Function Group node 1,
Digital-to-Analog Converter node 2, and fixed internal speaker pin node 3.
The speaker connects directly to the converter. Discovery advertises only this
format, PCM streams, output-only pin capability and D0/D3 power states.
There are no codec amplifiers; the guest performs volume mixing in software.

| Controller offsets | Registers and effects |
| --- | --- |
| `00–06` | GCAP stream/addressing capabilities, version 1.0 and link payload capacities. |
| `08`, `0c`, `0e` | GCTL controller reset/link start; WAKEEN codec wake mask; STATESTS write-one-to-clear codec enumeration. Wake mask and status survive controller reset. |
| `20`, `24` | INTCTL masks and derived INTSTS controller/stream summary; PCI interrupt disable independently masks the INTx wire. |
| `30`, `38` | WALCLK at 24 MHz and SSYNC playback synchronization hold. |
| `40–4e` | CORB base, write/read pointers, pointer-reset handshake, DMA/error control, error status and 2/16/256-entry sizing. |
| `50–5e` | RIRB base, write pointer/reset, response threshold, DMA/interrupt control, write-one-to-clear response/error status and sizing. |
| `60–68` | Immediate command, response and result-valid latch; runs synchronously when CORB DMA is stopped. |
| `70–74` | Position-buffer base and enable bit; slot zero receives the playback byte position. |
| `80–9c` | Output stream tag, run/reset, completion/error interrupt enables/status, LPIB, cyclic length, last descriptor index, FIFO size, format and 64-bit BDL base. |
| `2030`, `2084` | Read-only wall-clock and playback-position aliases. |

The driver programs guest RAM command/response rings, starts their DMA engines,
and submits codec verbs. Responses are published before the RIRB write pointer
advances. Response interrupts follow the programmed threshold; a later service
call also signals a partial batch, representing an empty link response slot.
Commands for an absent codec produce no response. Playback follows 16-byte
buffer descriptors, handles partial buffers and cyclic wrap, and latches
completion when an interrupt-on-completion descriptor ends. Stream reset clears
stream configuration and positions; controller reset also resets codec controls.
Diagnostic host counters remain cumulative across guest resets.

## Machine wiring and host policies

The ARM board attaches the device at PCI `00:08.0`, INTA, with dedicated GIC
interrupt 56 (SPI 24). ACPI `_PRT` and the device tree describe the same route.
Its relocatable 64-bit BAR requests 16 KiB inside the board's PCI aperture
`10000000–3effffff`; BAR halves support DWORD size probes and assignment.
PCI Memory Enable controls decoding; Bus Master Enable gates guest RAM DMA.
All DMA ranges are checked with overflow-safe bounds against existing RAM.
Invalid command/response buffers latch ring error status. Invalid playback
format, descriptor geometry or DMA latches a descriptor error and stops RUN.

The board services audio with all guest virtual processors stopped and caps run
slices at 5 ms while the device is present. Fractional 48 kHz sample frames are
retained between calls. Host scheduling stalls process at most 100 ms of audio
per service. The SDL backend caps queued audio at 100 ms and drops old queued
samples when catching up, keeping host memory and output latency bounded.
These are host policies rather than physical FIFO timing guarantees.

Recording, other rates/sample widths/channel counts, multiple playback streams,
MSI/MSI-X, unsolicited responses, jack detection, hardware gain/mute, controller
power-management capabilities, GCTL flush cycles and electrical link/FIFO timing
are unsupported. Reserved/unimplemented registers read zero and ignore writes.
Link start/reset, codec responses, power changes and FIFO readiness are immediate;
this timing difference is called out as a specification gap in the source.
This implementation is attached only to the ARM64 UEFI platform.

## Validation

```sh
make test-audio
make smoke-audio
python3 -m unittest discover -s tests -p test_hvf_cli.py
python3 -m unittest discover -s tests -p test_arm_acpi.py
python3 -m unittest discover -s tests -p test_arm_platform.py
```

The deterministic suite uses literal register addresses and expected values:
PCI discovery/BAR probes and decode masks, codec graph and controls, immediate
verbs, all ring sizes and wraparound, reset handshakes, response thresholds,
interrupt acknowledgment/masks, bus-master gating, stream synchronization,
fractional timing, cyclic PCM contents, position DMA/aliases, power/pin gating,
absent codecs and invalid/overflowing DMA. It also runs under ASan/UBSan.

The EFI probe boots headlessly from a small sparse disk. It exercises the real
machine's PCI/MMIO routing, command/response DMA, speaker configuration, playback,
completion/position updates, reset and SDL output using `SDL_AUDIODRIVER=dummy`.
Its temporary guest disk and variable-store files are removed after success.
Firmware tests independently check the compiled ACPI and device-tree route.

A headless installed Windows 11 ARM64 boot on 26 October 2026 exercised codec
commands and PCM playback without injecting an audio driver. See
[the recorded validation](measurements/intel-hda-windows-2026-10-26.json).
The test uses an existing guest image with a temporary compressed overlay, removed
after success. The host backend used SDL's dummy output, so this establishes
guest playback and host queuing, not an audible speaker listening test.
`--cpu-state` includes `hdaCommands`, `hdaFrames`, `hdaOutputFrames`,
`hdaNonzeroSamples` and controller/stream registers for diagnosis.
