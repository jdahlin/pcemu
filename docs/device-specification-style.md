# Specification-oriented device code

Use this convention for new devices and when migrating existing devices. The first
example is `src/arm_pl031.{hpp,cpp}`. Migrate one device at a time; keep behavior
changes separate from naming and documentation changes.

## Source and scope

Start each device header with the hardware model, specification title/document ID,
revision and authoritative URL. Every function definition must have a purpose
comment and a direct link to its relevant specification section immediately
above it, as in Ladybird. Include the document ID and section title. Use HTML
section anchors when available; for PDFs, use `#page=N` with the one-based PDF
page number (including front matter), and name the printed section as well.
A document ID or a link at the top of the file alone is insufficient. Distinguish
chip semantics from machine wiring, firmware policy and host services. A reference establishes provenance, not full conformance.
If evidence is unavailable, say so rather than inventing a specification citation.

Start the implementation file with a plain-language device overview. Explain
its purpose, its major parts and how they interact, the guest's view of the
device, and the boundary with machine wiring and host services. Define acronyms
on first use and state model limitations. A reader should not need prior chip
knowledge to understand the register handlers.

## Names and constants

Use the specification's terms for operations and state, expressed in the local
C++ style. Use descriptive snake_case for methods and state. Rename generic
methods in the migrated device and update their callers as part of the migration. Use the
specification's operation, register and signal names: `write_match_register()`
for RTCMR, `write_load_register()` for RTCLR, and `rtcintr_asserted()` for the
RTCINTR output. Bus adapters can use `read_register()` and `write_register()`;
the specification does not name an algorithm for every C++ boundary. Do not
invent a specification algorithm name for host-only helpers. Use the full register titles from the specification as scoped constants, such
as `Register::DataRegister`, `Register::ControlRegister`, and
`Register::InterruptMaskSetOrClearRegister`. Retain hardware abbreviations such
as RTCDR in documentation and section references so readers can search the
manual. Prefer descriptive names over unexplained acronyms in executable code. Name offsets, command values,
bit masks and timing quantities; include units in timing names. Keep definitions
local unless callers need them. Document every constant: explain the register's
purpose, the effect of a bit or command, or the quantity and units represented.
For offsets, state that they are relative to the device's mapped base address.
Explain acronyms and encoded fields rather than merely repeating the identifier.
Identity bytes may remain literal values when paired with named registers,
a table reference and an explanation beside each returned value of its encoded
meaning, including field positions, combined identifiers and revision mappings. Ordinary zero/one
values and arithmetic need no decorative constants.

## Inline documentation

Each function's comment must explain what it does, its result or side effects,
and the relevant hardware rule. Keep the explanation next to the definition,
with a short interface contract in the header. Split substantial register
operations into methods named after the corresponding specification construct.
Do not extract trivial getters merely to add more comments.

For example, the PL031 implementation follows this shape:

```cpp
// Store RTCMR, the Match Register, and recognize a match at the current RTC value.
// ARM DDI 0224C, 3.3.2 Match Register, RTCMR.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=24
void ArmPl031::write_match_register(uint32_t value) {
    // Implementation of the register operation.
}
```

Inside an algorithm, put the relevant specification steps beside the matching
code, preserving original step numbers and wording when quoting. Do not invent
numbered steps for register tables. Paraphrases must be clearly explanatory
prose, rather than presented as quotations. A `SPEC:` label is not a substitute
for the URL and explanation.

- `AD-HOC: <policy and reason>.` identifies emulator decisions such as host clock
  selection, polling, reserved-access fallback or compatibility behavior.
- `FIXME: <gap and required behavior>.` records a conformance gap or bug next to
  the affected code, with a direct specification link and section reference.
  Explain any compatibility constraint.
- `TODO: <concrete follow-up and reason>.` records maintenance work that does not
  represent an existing correctness gap. Link the relevant spec or issue when
  applicable. Do not use vague “implement properly” notes, attribution or dates.

Remove resolved TODO/FIXME comments in the same commit as the implementation.
Find remaining work with `rg -n 'AD-HOC:|FIXME:|TODO:' src/<device>*`.

## PL031 pilot

Reference: [ARM PrimeCell Real Time Clock (PL031) Technical Reference Manual,
ARM DDI 0224C, r1p3](https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e).
Register accesses, match interrupts and identification are annotated in the code.
The injected host clock and sampling are emulator policy. Guest writes adjust
only the VM clock. Existing behavior is preserved, including these limitations:

- RTCCR always reads enabled and ignores writes; its enable/reset semantics remain
  a FIXME. The counter starts from host time rather than the hardware reset value.
- Integration test registers are unimplemented.
- Reserved/unimplemented reads return zero and unsupported writes are ignored.

`tests/arm_devices_test.cpp` covers clock loads, skipped matches, masking, clearing,
backward host time and wraparound. Tests for reserved accesses and RTCCR preserve
current compatibility behavior; they are not conformance claims.

## Migration checklist

1. Identify the exact reference and inventory registers, state and known gaps.
2. Replace hardware literals with named constants and align private helper names.
3. Document every function with its purpose and a direct section link; put spec
   steps beside the matching code and identify host policy and conformance gaps.
4. Check behavior using deterministic device tests with literal bus addresses,
   so tests independently check the register map. Cover observable side effects.
5. Run relevant headless checks and commit the device, tests and documentation.

This borrows specification naming and comment practices from
[Ladybird's coding style](https://github.com/LadybirdBrowser/ladybird/blob/master/Documentation/CodingStyle.md)
while retaining this repository's formatting and existing public APIs.
