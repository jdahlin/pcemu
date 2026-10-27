# ISA audio and byte DMA

Second Reality supports Sound Blaster, Sound Blaster Pro, Gravis UltraSound and
no sound. The no-sound run retains its music sequencing but produces no PCM.
A real audio path needs a peripheral sample clock, RAM DMA, interrupt handling,
and host output. The primary byte DMA controller and Sound Blaster Pro DSP PCM model are now
implemented. SDL/native frontends consume the bounded stereo PCM queue and pace
against the shared guest clock. GUS remains absent. Headless runs stay silent and
unpaced; explicit WAV capture records the generated PCM without opening a host
audio device.

## Primary Intel 8237A DMA controller

Reference: Intel 8237A High Performance Programmable DMA Controller, 231466,
September 1993, [Register Description](https://openwatcom.org/ftp/devel/docs/231466.pdf#page=7),
[mode/mask programming](https://openwatcom.org/ftp/devel/docs/231466.pdf#page=8),
and [status/software commands](https://openwatcom.org/ftp/devel/docs/231466.pdf#page=9).
This is a four-channel byte-transfer model; it does not imply complete 8237A
bus-cycle conformance.

The board maps ports 00h–0Fh to the controller. Each channel has a current and
base address/count. Address/count port accesses share one low/high-byte pointer;
port 0Ch clears it. Acknowledge consumes one eligible RAM-to-peripheral byte,
returns its 24-bit physical address, advances/decrements the 16-bit offset, and
decrements the count. A programmed count N means N+1 bytes. Crossing from zero
to FFFFh latches terminal count. Without auto-initialize, the channel masks
itself; with auto-initialize it restores base address/count and remains enabled.
Status reads clear terminal-count bits and preserve request levels.

PC/AT external page latches are 87h/83h/81h/82h for channels 0/1/2/3. Offset
wrap does not carry into the page. DMA bypasses CPU segment translation and A20;
the peripheral/machine will perform the physical RAM access. Device DREQ levels
remain separate from software requests. Software request service is permitted
only in block mode. Disabled, masked, wrong-direction and cascade requests
cannot consume data.

Demand/single/block mode share the byte acknowledgment path. There is no bus
arbitration, wait-state/electrical timing, memory-to-memory transfer, verify/write
transfer, secondary word controller, or automatic refresh/FDC activity. Undefined
read offsets return FFh. Unmodeled page ports retain the board's previous latch
behavior. These limits matter for broader ISA compatibility beyond 8-bit audio.

Snapshot version 10 preserves all channel/base/page/control state and the shared
byte pointer. Versions 1–9 default the absent controller to reset state. Snapshot
checks exercise versions 6–10, including half-read register restoration.

`make test` includes literal controller tests for count-minus-one, page wrapping,
auto-initialize, decrementing, masks, wrong direction, controller disable,
cascade, software request eligibility, byte-pointer sharing, status clear and
master clear. Machine component tests exercise actual 8237/page ports, and
snapshot tests preserve/restore controller state. These are deterministic tests;
audio transfer timing and independent guest playback validation remain required
when the sound peripheral is added.


## Sound Blaster Pro DSP PCM

Reference: Creative, *Sound Blaster Series Hardware Programming Guide*, first
edition, [DSP ports/reset (chapter 2)](https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=24),
[DMA playback (chapter 3)](https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=40),
[CT1345 mixer (chapter 4)](https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=65),
[DSP commands (chapter 6)](https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=86).

The board exposes base 220h, DMA 1, IRQ7 and DSP 3.02. Reset produces AAh after
100 microseconds. Commands implement identity/version/test register, speaker
control, time constant, block size, 8-bit single-cycle/auto-initialize/high-speed
PCM, pause/resume, silence, exit-auto and software interrupt. High-speed mode
accepts reset rather than ordinary stop commands. Reading 22Eh acknowledges the
DSP latch; PIC EOI remains independent. Reset leaves the mixer and cumulative
validation counters intact.

A byte interval is `256 - timeConstant` microseconds. The PIT-frequency clock
uses integer remainder accumulation, including fractional periods across long
poll intervals. SB Pro stereo alternates bytes left/right; the time constant is
per byte, not per stereo frame. DSP block length and the 8237 automatic buffer
length are independent. DMA reads physical RAM, bypassing CPU paging/A20. Muted
speaker output still transfers bytes and generates block interrupts.

CT1345 reset sets master/voice/MIDI volume to level four and mono output. Host
PCM uses an approximate Q15 attenuation curve anchored at the documented -46 dB,
-11 dB default and 0 dB maximum levels. Analog filters, exact intermediate gain,
crosstalk, ADC, ADPCM, FM, MIDI and SB16 commands remain unsupported. Direct DAC
commands latch the output but do not yet produce a continuously timed direct-DAC
stream. This is an explicitly limited DSP 3.02 implementation.

The model resamples held DAC values to stereo signed-16-bit 48 kHz. The queue is
bounded to one second; consumer backpressure can discard queued presentation
samples but cannot change guest DMA or IRQ progress. Paused/inactive periods do
not append PCM, so the queue is not yet a complete continuously timed recording.
SDL and native PC frontends now consume this queue through the existing stereo
48 kHz host service. The sink opens lazily on the first samples. A sink failure
leaves guest DMA/IRQs running. Headless runs do not open it.

Snapshot version 11 adds DSP/parser/mixer/response/sample-phase/block/IRQ state
and cumulative audio counters. Older snapshots get a reset DSP. The PCM queue
is transient and cleared on load. Compatibility tests cover versions 6–11.

`make test` now runs a small RAM-only guest which resets/probes the DSP, programs
8237 auto-initialize playback, receives three block IRQs, pauses and checks the
DMA address is inside the circular buffer. The same guest passes QEMU SB16:

```sh
make test-sound-blaster
make test-sound-blaster-reference
```

Literal model tests additionally check exact signed stereo amplitudes, speaker
mute with continuing transfers, independent DSP/DMA blocks, fractional-clock
partition invariance, pause/resume, reset, high-speed stopping and bounded output.
Machine tests check raw physical RAM with A20 disabled and separate DSP/PIC
acknowledgments. Snapshot round-trip comparison includes every new field.

The opt-in real-demo check accepts `--sound sb` or `--sound sbpro`, boots JEMMEX
for EMS and verifies nonzero DMA/block/non-silent PCM counters in addition to
clean return and changing graphics. A 90-second ending-only SB trial generated
6.7 million DMA bytes and non-silent PCM but timed out without DOS return; that
is not a completion pass. The full frozen-runner JIT SB mono validation now passes: clean DOS return,
19,856,872 DMA bytes, 309 completed DSP blocks and 46,486,195 non-silent host-format
PCM frames. Input hashes and counters are in the [demo measurement record](measurements/second-reality-2026-10-27.json).
The full SB Pro stereo JIT run also passes: 39,765,351 DMA bytes and 615 DSP
blocks, with clean DOS return. Host transport/pacing tests are described below;
reference waveform comparison remains unverified.
The committed completion gate is:

```sh
make test-second-reality-sb
```


## Live output, pacing and offline capture

Normal SDL/native PC sessions now deliver borrowed stereo16/48 kHz PCM to a
lazy host output device. The queue is bounded to 100 ms by the host service.
Successful live output paces execution against the deterministic PIT/VGA/DSP
clock, allowing 10 ms of lead and bounded 20 ms waits. This also paces programmed
silence/pause gaps after the sink opens. It never changes the clock source or
feeds host playback state into DMA/IRQ behavior. UI pause and snapshot/reset
transitions clear stale queued audio and rebase pacing. A slow host can underrun;
realtime performance is not guaranteed by these functional tests.

`--no-audio` mutes live presentation without disabling the emulated card.
`--audio-wav FILE` records generated PCM in a portable little-endian stereo16,
48 kHz RIFF/WAVE file. It works headless and finalizes sizes on normal return or
exception unwinding. RIFF's 32-bit length limit is checked before append.
Inactive/paused intervals currently do not emit PCM, so WAV output concatenates
active stream segments; it is not yet a continuous timestamped reference capture.
Full-demo recording is opt-in to avoid retaining large files during normal tests.

`make test` now includes deterministic pacing tests and literal WAVE header/sample
byte checks. SDL frontend tests use dummy video/audio drivers, and the embeddable
session test delivers a real guest DSP silence block to a fake frontend without
opening host devices. The RAM-only DSP probe additionally records a tiny WAV in
both engines, independently parses it with Python `wave`, and requires its frame
count and PCM hash to exactly match device output. That guest also passes QEMU
SB16; this verifies protocol behavior, not equality of QEMU audio waveforms.
