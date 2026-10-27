# ISA audio and byte DMA

Second Reality supports Sound Blaster, Sound Blaster Pro, Gravis UltraSound and
no sound. The no-sound run retains its music sequencing but produces no PCM.
A real audio path needs a peripheral sample clock, RAM DMA, interrupt handling,
and host output. The primary byte DMA controller is the first implemented part;
Sound Blaster/GUS playback and host audio are not implemented yet.

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
