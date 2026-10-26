# Specification-oriented device code

Apply this convention to every new device and when migrating an existing device.
The PL031 RTC in `src/arm_pl031.{hpp,cpp}` is the first example. Migrate one device
at a time. Keep naming/documentation refactors separate from hardware behavior
changes so reviewers can verify both independently.

The intended reader knows C++ but may know nothing about this chip. Explain what
an operation or value means, why it exists, and what guest software does with it.
Expanding an acronym or listing bit positions alone is not enough.

## Device overview

Start the implementation file with a plain-language overview covering:

- What the device does and how the guest OS or application uses it.
- Its major parts and how data, control and interrupts flow between them.
- The register interface, state, timing and externally visible side effects.
- Which responsibilities belong to machine wiring or host services.
- Emulator policies, unsupported functionality and known specification gaps.

Define acronyms on first use. For example, explain that the RTC exposes seconds,
software converts those seconds to calendar dates, the alarm compares against a
programmed time, and masking suppresses the interrupt output without clearing
latched status. Explain signal terminology such as active-high when introducing
it. State that register offsets are relative to the device's mapped base address.

Include the hardware model, specification title, document ID, revision and an
authoritative URL in the header and implementation overview. Do not imply full
conformance merely because a reference is present.

## Specification links and function documentation

Every function definition needs a purpose comment immediately above it. Describe
what it does, its result and side effects, and the hardware rule it implements.
Include the specification section title/number and a direct URL to that section.
A document ID alone, a `SPEC:` label, or one URL at the top of the file does not
replace these local links. Keep a short interface contract in the header too.

Use HTML section anchors when available. For PDFs, use `#page=N`, where N is the
one-based PDF page number including front matter, and name the printed section.
Check the target against the actual manual; printed page 3-4 is not PDF page 4.

```cpp
// Store RTCMR, the Match Register, and recognize a match at the current RTC value.
// ARM DDI 0224C, 3.3.2 Match Register, RTCMR.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=24
void ArmPl031::write_match_register(uint32_t value) {
    match_register = value;
    if (match_register == uint32_t(previous_counter))
        raw_interrupt = true;
}
```

For specification algorithms, put the relevant steps beside their implementation.
Preserve original step numbers and wording when quoting. Explanatory paraphrases
must not be presented as quotations. Do not invent numbered algorithms for
register tables; identify the registers, fields and table instead. Add local
links inside a function when its branches implement different sections.

For a host-only helper, describe its implementation role and label the policy.
Link the hardware requirement it supports when applicable, but do not invent a
specification origin. If evidence is unavailable, state that explicitly.

## Function and state names

Use the specification's operation, register and signal terminology, expressed as
descriptive snake_case method/state names. Rename generic methods and update
callers as part of the device migration. Examples from PL031:

| Construct | Method |
|---|---|
| Match Register, RTCMR | `write_match_register()` |
| Load Register, RTCLR | `write_load_register()` |
| Interrupt mask, RTCIMSC | `write_interrupt_mask()` |
| Interrupt Clear Register, RTCICR | `clear_interrupt()` |
| RTCINTR output signal | `rtcintr_asserted()` |
| RTC value and Update block | `update_rtc_value()` |
| Register bus access | `read_register()` / `write_register()` |

Split substantial register operations into methods named after their hardware
purpose. Avoid vague names such as `update()` or `irq()` when a more precise name
is available. Do not extract trivial getters just to add more documentation.
The specification need not name an algorithm for every C++ adapter or helper.

## Constants and encoded values

Use full register titles as scoped constants, retaining abbreviations in comments
so readers can find them in the manual:

```cpp
// ARM DDI 0224C, 3.2 Summary of RTC registers, Table 3-1.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=23
enum class Register : unsigned {
    // Data Register: reads the current RTC seconds value.
    DataRegister = 0x00,
    // Control Register: controls startup; this model always reports enabled.
    ControlRegister = 0x0c,
};
```

Prefer `Register::DataRegister` to `RTCDR` in executable code. Name hardware
commands, bit masks, fixed identifiers and timing quantities as well; timing
names must include units. Keep constants local unless callers need them.
Document every constant's purpose and source, including register effects, bit
meaning, field positions and units. Do not merely repeat its name.

A fixed multi-byte signature should have one named value; derive the register
bytes from it rather than scattering literal bytes through read handlers:

```cpp
// Fixed signature identifying ARM's PrimeCell identification scheme across devices.
// The Peripheral Identification registers separately identify the PL031 part.
// ARM DDI 0224C, 3.5 PrimeCell identification registers.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=29
constexpr uint32_t PrimeCellIdentification = 0xb105f00d;

// In read_register():
case Register::PrimeCellIdentificationRegister1:
    // Return identification bits 15:8 in the low byte; upper register bits are zero.
    return uint8_t(PrimeCellIdentification >> 8);
```

Explain what software learns from the value. Here, PrimeCell is ARM's family of
reusable peripheral designs; the shared signature identifies its identification
scheme, while separate peripheral registers identify the PL031 part. Explain how
software reconstructs split values, and distinguish byte significance from
register spacing: these byte-valued registers are four bytes apart.

For other encoded read results, explain the fields beside the return expression.
For example, `0x10` contains designer bits in its high nibble and part-number bits
in its low nibble. Explain how those fields combine with adjacent registers and
what revision encodings represent. Likewise explain a returned control value of
one as an enabled bit, or a zero fallback as emulator policy. Ordinary arithmetic
and zero/one values need no decorative constants when their meaning is clear.

## Implementation choices and outstanding work

Keep specified behavior distinct from emulator policy and known gaps:

- `IMPLEMENTATION NOTE:` explains a deliberate implementation choice and its
  reason, such as sampling an injected host clock or detecting skipped alarms.
  It does not imply a temporary hack or a specification defect.
- `COMPATIBILITY:` explains behavior retained for existing guests. State what is
  retained and why; a known specification violation also needs a FIXME.
- `FIXME:` identifies a bug or conformance gap, with the required behavior and a
  direct specification link/section reference. Explain compatibility constraints.
- `TODO:` identifies concrete maintenance work and its reason, without implying
  an existing correctness gap. Link the relevant spec or issue when applicable.

The pilot's older `AD-HOC:` comments mean behavior chosen by the emulator rather
than prescribed by the hardware specification. For future device migrations,
use the more precise `IMPLEMENTATION NOTE:` or `COMPATIBILITY:` labels above.
Do not use attribution, dates or vague notes such as “implement properly.” Remove
resolved TODO/FIXME comments in the same commit as their implementation.

Find these comments with:

```sh
rg -n 'IMPLEMENTATION NOTE:|COMPATIBILITY:|AD-HOC:|FIXME:|TODO:' src/<device>*
```

## Validation and migration checklist

1. Read the authoritative reference and record its exact model/revision. Inventory
   registers, state, interactions and known gaps before annotating behavior.
2. Write the device overview for a reader unfamiliar with the hardware.
3. Align function/state names and scoped register constants with the specification;
   update callers, and explain every hardware constant and encoded read value.
4. Give every function a purpose comment and direct relevant section link. Put
   algorithm steps beside their code and separate host policy from hardware rules.
5. Preserve existing behavior during this refactor. Document conformance gaps
   honestly; do not silently turn compatibility behavior into a hardware claim.
6. Run relevant deterministic, headless device checks and compile affected machine
   integrations. Test observable register effects, interrupts and timing boundaries.
   Use literal bus addresses/expected values in tests to check the map independently
   of implementation constants. Documentation-only changes need link/reference
   review and a diff check, rather than new behavior tests.
7. Review the overview, every function and every constant for unexplained jargon
   or magic values. Commit the code, caller updates, tests and documentation.

The PL031 pilot covers clock loads, skipped matches, masking, clearing, backward
host time and wraparound. It preserves the always-enabled RTCCR behavior and
unsupported-access fallback; integration test registers remain unimplemented.
Its tests preserve that compatibility behavior and do not establish full hardware
conformance. A future device must identify and test its own boundaries.

This convention follows the specification naming and local reference pattern in
[Ladybird's coding style](https://github.com/LadybirdBrowser/ladybird/blob/master/Documentation/CodingStyle.md)
and [AbortSignal implementation](https://github.com/LadybirdBrowser/ladybird/blob/master/Libraries/LibWeb/DOM/AbortSignal.cpp),
while retaining this repository's formatting.

## Migrated devices

| Device | Reference | Documented model boundaries |
|---|---|---|
| PL031 RTC | ARM DDI 0224C, r1p3 | Host clock sampling, alarm wraparound, always-enabled control and unsupported test registers. |
| PL011 UART | ARM DDI 0183G, r1p5 with revision history | Revision-1 identity, immediate transmit transcript, host receive queue, approximate interrupts and stored-only baud/FIFO configuration. |
| PSCI fallback | ARM DEN 0022C, PSCI 0.2 behavior | Firmware function IDs and errors, argument widths, one online boot CPU, immediate standby and machine-owned shutdown/reset. |
| 8259A and cascaded PC/AT pair | Intel 231468-003, December 1988 | IRR/IMR/ISR, priority rotation, ICW/OCW commands, fixed cascade wiring, polling and snapshot mirrors; unsupported electrical/processor modes are explicit. |
| 8254 PIT | Intel 231164-005, September 1993 | Named count modes and control fields, count/status latches, instruction-derived ticks, gate handling, coalesced periods and documented CLK/reload timing gaps. |
| PC/AT CMOS/RTC | IBM 6280070, September 1985, and Motorola MC146818A | Calendar BCD fields, divider/status encodings, BIOS equipment/memory bytes, VM-only host-time offset and unsupported writable/interrupt state. |
| System control ports A/B and debug console | Intel 290562-001, sections 4.2.4.1/4.2.7.2; Bochs port_e9_hack convention | A20/reset effects, timer/speaker status, snapshot latches, host byte sink and compatibility differences from reference behavior. |
| ARM fw_cfg | QEMU Firmware Configuration interface, including v2.9 DMA writes | Selector/directory formats, discovery signatures, endian conversions, synchronous RAM-only transfers and descriptor error reporting. |
| ARM NOR variable store | Intel StrataFlash 290667-011, April 2002 | Interleaved x16 command sequences, erase/program rules, named IDs, persistent versus visible bytes, dirty remapping and incomplete timing/protection behavior. |
| ARM RAM framebuffer | QEMU RAMFBCfg wire layout and Linux DRM XRGB8888 | Big-endian descriptor, little-endian pixels, padded rows, host captures/change detection, fixed selector and host size limits. |
| ARM OS debug lock | Arm AArch64 system-register descriptions: ESR_EL2, OSLAR_EL1, OSLSR_EL1, OSDLR_EL1 | Trap encoding/direction, lock-model identification, per-CPU reset policy and sticky double-lock compatibility boundaries. |
| PCI configuration buses | PCI Local Bus 3.0, 3.2.2.3.2; AMD PG055 v2.9, Table 2-24 ECAM map | CONFIG_ADDRESS/DATA masks, fixed PC display BDF, single-bus/function-zero ECAM routing, live BAR decoding and host overlap precedence. |
| VGA PCI function | PCI Local Bus 3.0, 6.2.1/6.2.2/6.2.5; QEMU Standard VGA PCI convention | Named vendor/device/class fields, fixed framebuffer/ROM addresses, size probing, firmware initialization and unsupported relocation/decoding controls. |
| Primary ATA PIO disk | T13/1153D revision 18, ATA/ATAPI-4, clauses 7-9 | Task-file/command names, CHS/LBA progression, status/error/IRQ semantics, IDENTIFY words and synchronous/no-op compatibility boundaries. |
| ARM Non-secure GIC Distributor view | Arm GICD register descriptions (CTLR, TYPER, SPI bitmap/configuration/priority/routing registers) | Native single-state backend requirements, Non-secure aliases, shared interrupt bounds, lane handling, hidden security state and bounded extension support. |
| Shared Virtio split-ring core | OASIS Virtio 1.2, 2.1/2.2/2.7/3.1/4.2.2 | Named MMIO/descriptor/ring fields, negotiation/reset/IRQ flow, direct chain ordering, host RAM/queue bounds, deferred requests and unsupported queue features. |
| Virtio traditional balloon | OASIS Virtio 1.2, 5.5 | Requested versus reported/tracked pages, PFN wire units, host-granule reclamation, MUST_TELL_HOST gating, retained statistics buffers and explicit 64-bit PFN compatibility option. |
| Virtio console/serial ports | OASIS Virtio 1.2, 5.3 | Multiport queue numbering, control packet fields/events, DEVICE_READY/PORT_READY/open handshake, host byte streams, backpressure and fixed-port/buffer policy. |
| Virtio entropy | OASIS Virtio 1.2, 5.4 plus MMIO/split-ring clauses | Modern discovery/features, writable direct request chains, host cryptographic entropy, partial fills, completion lengths and retained queue-reset/status compatibility. |
| Virtio block | OASIS Virtio 1.2, 5.2 plus MMIO/split-ring clauses | Capacity/features, IN/OUT/FLUSH/GET_ID request/status fields, used lengths, scatter/gather ordering, streamed host I/O limits and retained transport compatibility. |
| Virtio network | OASIS Virtio 1.2, 5.1 plus MMIO/split-ring clauses; RFC 1071 checksum | MAC/link/MTU fields, receive/transmit headers, checksum completion, EVENT_IDX, used-ring publication and host frame/backlog bounds. |
| Virtio PCI transport | OASIS Virtio 1.2, 4.1; PCI Local Bus 3.0, 6.8.2 | PCI identity/capabilities, common-to-MMIO translation, notify/ISR/device regions, BAR policy, INTx/MSI-X table/PBA routing and explicit capability gap. |
| Hyper-V reference clock page | Microsoft TLFS Timers, Reference TSC Page/Register/Mechanism | 100 ns units, fixed-point scale/signed offset, page registration, sequence publication/invalidation, stopped-VP contract and representability limits. |
| Hyper-V synthetic timers | Microsoft TLFS Timers and HV_REGISTER_NAME | Four Config/Count pairs per VP, flags/SINT/direct vector masks, one-shot/periodic/auto-enable behavior, busy-delivery retry, wrap extension, lazy deadlines and host cache/activity policy. |
| Virtio GPU 2D | OASIS Virtio 1.2, 5.7; VESA E-EDID A2 base EDID 1.4 | Command/response/layout/format names, resource/backing/scanout/cursor workflow, fencing/errors, monitor fields, captures and host limits/EDID compatibility. |
| NVMe controller | NVM Express 1.1a, sections 2-6 | BAR/controller/queue registers, Admin versus NVM opcodes, PRP validation, Identify/log/feature fields, completions/phase/status and explicit sparse/later-style compatibility boundaries. |
| PS/2 mouse | IBM PS/1 Technical Reference (1990), section 6; Linux PS/2 driver IntelliMouse wire reference | Command/response/status/report fields, scaling/defaults, wheel handshake, instruction sampling, host input bounds and explicit overflow/resend compatibility differences. |
| i8042 controller and keyboard responder | IBM PS/2 Hardware Interface Technical Reference, May 1988, Type 1 Controller and 101/102-Key commands | Data/Command/Status, command-byte/output-port fields, IRQ latch/backpressure, keyboard responses/parameters and explicit scan-set/typematic/input-port gaps. |
| ISA NE2000 / DP8390D | National TL/F/8582, July 1995, sections 7–11; Linux ne.c board conventions | Paged/direction-selected registers, station PROM signature, board RAM/remote DMA, multicast CRC filtering, receive ring/FCS, interrupts and synchronous/diagnostic compatibility boundaries. |
| ARM Hyper-V SynIC | Microsoft TLFS Inter-Partition Communication, HV_REGISTER_NAME, HV_MESSAGE and HV_SYNIC_EVENT_FLAGS | VP registers, RAM overlay validation, SINT masking/polling, event transitions, occupied-slot/pending/EOM handling, native GIC delivery rollback and host queue limits. |
| VGA graphics core and Bochs DISPI | IBM 42G2193, September 1992, section 2; Bochs VBE Display API 0.6/B0C5 history | Indexed groups/ports, plane latches/read-write modes, geometry and palettes, packed/banked/linear memory, API identifiers and explicit rendering/timing/addressing compatibility gaps. |
| ARM VMBus GPADL mappings | Linux primary wire definitions: gpa_range and VMBus GPADL header/body/teardown | PFN/range units, fragmented collection, atomic validation, channel ownership, bounded RAM copies, alias checks and host mapping generations; implementation reference is explicit. |
| ARM VMBus shared rings | Linux hv_ring_buffer/vmpacket_descriptor and drivers/hv/ring_buffer.c | Header/packet/trailer fields, eight-byte units, wrap copying, reserved free space, pending-size notification thresholds, stopped-VP ordering and stale-mapping rejection. |
| ARM VMBus control/channels | Microsoft OpenVMM protocol.rs, revision 76420ea2 | Contact/version/offer/open/close/GPADL fields and IDs, NTSTATUS versus hypercall errors, dedicated events, retained Windows padded-envelope behavior and bounded backend lifecycle/retries. |
| Hyper-V heartbeat IC and common IC framing | Microsoft OpenVMM IC Header/NegotiateMessage/HeartbeatMessage, pinned 76420ea2; Linux legacy body | Nested descriptor/pipe/IC fields, flags/version/sequence validation, distinct Windows GUIDs, ring-publication ownership, 100 ns deadlines and transport versus application health. |
| Hyper-V time-sync IC | Microsoft OpenVMM TimesyncMessageV4/TimesyncFlags and IC framing, pinned 76420ea2 | FILETIME/Unix epoch and 100 ns units, paired reference time, GUIDs/version/transaction fields, SYNC versus SAMPLE, fresh ring-full retries and acknowledgment limits. |
| Hyper-V shutdown IC | Microsoft OpenVMM ShutdownMessage/ShutdownFlags/planned reason, pinned 76420ea2 | Explicit power action versus negotiation, v3.2/transaction fields, GUIDs, force/restart/hibernate bits, guest seconds versus host deadlines, acknowledgment limits and no automatic retries. |
