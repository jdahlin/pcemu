# Specification-oriented device code

Use this convention for new devices and when migrating existing devices. The first
example is `src/arm_pl031.{hpp,cpp}`. Migrate one device at a time; keep behavior
changes separate from naming and documentation changes.

## Source and scope

Start each device header with the hardware model, specification title/document ID,
revision and authoritative URL. Put short section/table references beside the
relevant implementation. Distinguish chip semantics from machine wiring, firmware
policy and host services. A reference establishes provenance, not full conformance.
If evidence is unavailable, say so rather than inventing a specification citation.

## Names and constants

Use the specification's terms for operations and state, expressed in the local
C++ style. Use descriptive snake_case for new private helpers and state; retain
public APIs when renaming would cause unrelated churn. Canonical register names
such as `RTCMR` are preferable to invented synonyms. Name offsets, command values,
bit masks and timing quantities; include units in timing names. Keep definitions
local unless callers need them. Identity bytes may remain literal values when
paired with named registers and a table reference. Ordinary zero/one values and
arithmetic need no decorative constants.

## Inline documentation

Use sentence comments explaining the hardware rule or reason, not narrating C++.
The following searchable prefixes distinguish the origin of behavior:

- `SPEC: <document ID>, section <number> (<topic>).` identifies specified behavior.
  Paraphrase accurately. For numbered algorithms, preserve step numbers; hardware
  register tables need register/bit names and section references instead.
- `AD-HOC: <policy and reason>.` identifies emulator decisions such as host clock
  selection, polling, reserved-access fallback or compatibility behavior.
- `FIXME: <reference>: <gap and required behavior>.` records a known conformance
  gap or bug next to the affected code. Explain any compatibility constraint.
- `TODO: <concrete follow-up and reason>.` records maintenance work that does not
  represent an existing correctness gap. Do not use vague “implement properly”
  notes, attribution or dates; link an issue when one exists.

Do not label an entire function SPEC when it mixes specified and emulator-specific
behavior without identifying the latter. Remove resolved TODO/FIXME comments in
the same commit as the implementation. Find remaining work with
`rg -n 'SPEC:|AD-HOC:|FIXME:|TODO:' src/<device>*`.

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
3. Add local references and distinguish host policy and conformance gaps.
4. Check behavior using deterministic device tests with literal bus addresses,
   so tests independently check the register map. Cover observable side effects.
5. Run relevant headless checks and commit the device, tests and documentation.

This borrows specification naming and comment practices from
[Ladybird's coding style](https://github.com/LadybirdBrowser/ladybird/blob/master/Documentation/CodingStyle.md)
while retaining this repository's formatting and existing public APIs.
