# Second Reality compatibility

The target is the unmodified 7 October 1993 release: complete visual sequence,
correct raster effects, synchronized music, and clean return to DOS in both
execution engines. Setup entry alone is not a pass for that target.

## Reproducible setup check

```sh
python3 tools/freedos_fixture.py  # once, if the shared parent is absent
make test-second-reality-setup
```

The opt-in check downloads the [Scene.org release](https://files.scene.org/view/parties/1993/assembly93/demo/2nd_real.zip),
verifies the archive and both executable/data files, and uses a small temporary
compressed overlay of the existing FreeDOS disk. HIMEMX and DOS=HIGH preserve
the required conventional memory. No sound driver is loaded. Each engine runs
headless against a frozen runner/ROM. Success requires the original sound-card
and loop-selection setup screen. Reports retain input hashes, instruction
count, state, screenshot, and log under build/second-reality-setup-*; successful
overlays and copied runners are removed. Media, VM disks, and output stay ignored.
This target is deliberately separate from offline `make test`.

Pinned release archive SHA-256:
`9dfdb9c312d736003cc363b151b6f366c481b3bd3bb78233cfff72ae677c5c95`.
The script separately checks SECOND.EXE and REALITY.FC. No patch launcher or
modified executable is used. The [author's source release](https://github.com/mtuomi/SecondReality/tree/071a82eddb7a7390dbaa491a8a473e0e923a55c2)
is the diagnostic reference; reproducing its TASM/Microsoft C and binary-player
build is not a prerequisite to testing the released executable.

## Verified progress and remaining work

- The PIT and VGA retrace share the guest clock; a RAM-only calibration probe
  passes in both engines and QEMU precise-retrace mode. See [VGA timing](vga-bios.md#shared-pitvga-raster-clock).
  The default clock remains instruction-derived, not cycle-accurate.
- Single-step INT 1 delivery fixes the release unpacker's execution of encrypted
  bytes. Literal CPU regressions and an independent QEMU guest check cover the
  trap boundary. See [x86 debug traps](x86-real-mode.md#single-step-debug-traps-and-packed-dos-executables).
- The checksum-pinned original executable reaches setup in interpreter and JIT.
- A no-sound launch now advances to animated graphics after the copper calibration
  correction described below. Full completion and correct visuals remain
  unverified. Host-time sleeps used during exploration are not an automated
  progress/completion oracle.
- Captures still render a single stable VGA state. Add beam-timed scanout that
  retains palette/start-address/panning changes, with source-derived copper tests.
  Implement blanking intervals and relevant register latching as required.
- ISA Sound Blaster/GUS and 8237 DMA remain absent. First validate the complete
  no-sound sequence (including music sequencing), then implement and validate a
  real sound path and its sample-clock/IRQ synchronization.
- Add a separate full-run test using start points 1–5 for localization and a
  complete non-looping run for acceptance. Scene/music markers and reference
  captures must distinguish real progress from a running CPU or black frame.
  Realtime performance and audio/visual reference comparison remain unmeasured.

### Copper calibration correction

The released executable reissues control word 36h before reading counter 0.
Previously that command reset the model's count origin, so the demo measured
almost zero and installed zero copper intervals. Programming control logic must
not reload the Counting Element from the initial count. Retaining the count
origin until actual count data is written fixes the calibration. The exact
mode-3/retrace/read sequence now passes in the RAM-only guest probe on PCEmu
(both engines and both ROM sources) and QEMU precise retrace. Literal PIT tests
check that control-word/first-byte writes preserve the origin and the completed
count write changes it. The no-sound release now advances to populated, animated
graphics; an exploratory capture shows the translucent polygon scene. Full-run,
raster fidelity and sound validation remain separate requirements.
