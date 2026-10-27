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
- Legacy graphics now presents beam-timed completed frames, retaining mid-frame
  palette/VRAM changes and latching display start for the next frame. Physical
  doubled scanlines are retained. Blanking and wrapped retrace are implemented.
  See [scanout scope and tests](vga-bios.md#beam-timed-legacy-graphics-presentation)
  for remaining fetch/latch/address-counter limitations and reference needs.
- ISA Sound Blaster/GUS and 8237 DMA remain absent. First validate the complete
  no-sound sequence (including music sequencing), then implement and validate a
  real sound path and its sample-clock/IRQ synchronization.
- `make test-second-reality` now runs an opt-in no-sound completion check in
  both engines. `--part 1` through `--part 5` select released start points for
  localization. It waits for observed setup/confirmation text before sending
  keys, records frame hashes/instruction positions, and requires clean DOS
  return plus changing graphics. This is a completion smoke, not a pixel oracle.
  Source scene/music markers and reference captures remain needed for fidelity.
  Realtime performance and audio/visual comparison remain unmeasured.

The first full-run check fails honestly: 955 observations contain 71 distinct
frames, then the frame stops changing at about 1.14 billion instructions. The
120-second watchdog expires without DOS return (about 15.3 billion instructions).
The final screenshot, state, `frames.json`, and failure report remain in ignored
build evidence. Successful checks remove copied runners/ROMs and overlays.
Do not interpret a running CPU, timer IRQs, or graphics entry as completion.

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

### BIOS clock coherence correction

The later stall exposed a second timing defect: INT 1Ah returned a private tick
mirror even after the demo's timer replacement updated the public BIOS data
area. BIOS get/set-time and firmware IRQ0 now share 0040:006Ch and the midnight
flag. The same 40-second JIT completion probe then records 274 distinct frames
and continues through the plasma and 3D scenes; its final samples are still
changing at 2.44 billion instructions. That shorter watchdog still expires, so
it does not establish completion. The earlier 1.14-billion-instruction stall is
resolved. Literal BIOS clock tests pass with the main CPU/component suites.


The 180-second interpreter run still advances through the 3D scene at roughly
2.9 billion instructions when sampled; that watchdog is insufficient to establish
completion. The opt-in completion check now defaults to a bounded 600 seconds
per engine and remains outside `make test`. The frozen-runner JIT validation of the
new scanout now completes successfully: 4,760,683,102 instructions, 1,588 frame
observations and 1,386 distinct frame hashes, followed by the demo's closing text
and clean DOS return. [Pinned input hashes and result](measurements/second-reality-2026-10-27.json).
The temporary overlay and copied runner/ROM were removed after success. This
establishes full no-sound execution completion; it does not establish reference
pixel fidelity, sound, or realtime performance. A full interpreter run with the
new scanout is in progress.
