# Fresh Windows 95 Setup validation

The frozen fetch-TLB candidate (`cb3f275` x86 dispatcher, executable SHA-256
`722e9c1636f29e27f0c40f3597060bcedfdafd4fecddc3f3b3bc2612b920c2e6`)
runs fresh OSR2 Setup from the existing recovered MS-DOS base. Preparation stages
and reads back all 56 source files. The initial compressed child occupies
81,999,324 bytes; no raw multi-GiB disk is created. The wizard uses the normal
installation choices and enables network-adapter detection.

File copying completes and Setup requests its first firmware restart in the same
process. Initial configuration then presents the Microsoft Networking login,
printer wizard and explicit final-configuration restart. The updated headless
checker cancels the two optional dialogs and acknowledges that specific Setup
restart. Two firmware resets are observed.

The original process disappears during the user's pause. Its log has no normal
exit marker, so the cause is unknown. After verifying that PID is absent, offline
FAT inspection finds the installed Windows/system files. A small child boots
that same installation independently. It reaches Explorer, runs Calculator 2+2,
copies through the clipboard, and saves a new Notepad file. After a resumed
guest-requested restart, exact stopped-guest FAT readback matches all 38 expected
bytes. The original install and resume children are then removed. Existing DOS,
Win95 and Office parents are preserved.

The verifier retains its independent unobscured-desktop pixel gate plus both
icon-name checks. Fresh Setup leaves My Computer selected; sparse OCR discards
the white-on-teal label. A small bright-glyph mask supplies an additional OCR
reading of the actual label, and a wider existing crop includes the full Recycle
Bin label. Unknown dialogs still leave the guest intact. The observed small-font
`Are`/`Ale` OCR variation is accepted only alongside the restart option text.

Observer processes were replaced while the live guest remained intact as these
specific gaps were corrected. Calculator/Notepad completed before the original
restart-dialog OCR check timed out; the restart check was then resumed, and exact
file readback proves persistence. The rtts applications/restart/exact-file
regression also passes with the common dialog/desktop changes. Python syntax
checks and `git diff --check` pass. This is functional evidence across an
interrupted run and independent cold boot; elapsed installation time includes
agent input and pause delays and is not a performance score.

[Commands, manifests, reset/application evidence, screenshots and compressed logs](measurements/fresh-win95-setup-fetch-tlb-2026-10-25.json).

## Uninterrupted run with native memory-indirect CALL

The frozen x86 candidate containing `1c5a879` now passes a fresh OSR2
installation from clean recovered DOS through application persistence in one
PID. It includes the PIC-idle and frame-rendering changes. Other in-progress
ARM/HVF/frontend changes in the shared checkout are not part of this frozen
executable; the precise executable identity and command are in the evidence.

The first staging attempt reports a file-readback mismatch and then times out
closing `disk-tool` over SMB. That process is later confirmed absent. Before
booting, a separate read-only reopen verifies all 56 staged source files byte
for byte. The agent drives the normal Setup wizard through the virtual guest
keyboard, using the previously supplied OEM fields without committing them.
Network-adapter detection is enabled. The automated finish chain then observes
file copying, acknowledges the specific Finish screen and verifies firmware
reset 0→1. Known first-boot networking, clock, printer and explicit Setup-restart
screens are handled; Explorer appears after reset 2.

Calculator computes 2+2, its result is copied into Notepad with a fresh marker,
and the file is saved. A guest-requested restart advances reset 2→3 and returns
to Explorer. The guest receives `quit`; PID absence and the normal
`stopped=budget/quit` trailer are observed before read-only FAT inspection.
All 38 expected bytes match exactly. The 154,663,677-byte compressed test child
is removed, leaving the existing DOS, Win95 and Office parents intact. No raw
multi-GiB disk is created. This run has no guest-process interruption or manual
recovery after boot. Wizard input was agent-driven; the post-copy/restart/file
verification chain was scripted.

The detached launcher cannot supply a process exit code, so none is claimed.
The separate normal-quit, PID-absence and exact-file gates provide the stopping
and persistence evidence. Elapsed time includes wizard and observation delays
and is not a performance comparison. This closes the fresh-installer gate for
this native CALL candidate; it does not establish the twofold performance target.

[Reset/application results, manifest, compressed logs and screenshots](measurements/fresh-win95-memory-call-2026-10-25.json).


## Uninterrupted run with native FF /6 PUSH (26 October)

The frozen executable containing the native register/memory PUSH lowering
(`d9068f7` source change, executable identity in the evidence) now completes a
fresh OSR2 installation from clean recovered DOS in one guest process. All 56
installer files stage and verify successfully in an 81,999,324-byte compressed
child on the external volume. No Office fixture or full-size raw disk is made.

The agent drives the Setup wizard using the already supplied OEM fields and
enables network-adapter detection. The scripted observer acknowledges the
specific copy-complete Finish screen and sees the first firmware reset. Known
network logon, printer and explicit final Setup-restart screens are handled;
Explorer appears after reset 2. Calculator computes 2+2, its clipboard result is
saved with a fresh marker in Notepad, and a guest-requested restart returns to
Explorer after reset 3. Both screenshots have been inspected.

The original guest receives `quit`. This run retains its launcher, so the actual
process exit status **0**, PID absence and normal quit trailer are all verified.
An independent read-only FAT reopen then matches all 38 expected saved bytes.
The owned compressed child and its index are removed after success. The DOS,
Win95 and Office parents remain intact. The measurement records the child's
final compressed size and hashes all evidence artifacts.

There is no guest-process interruption or manual recovery after boot. Wizard
input is agent-driven; this is not a newly implemented generic unattended
installer script. The human user need not supply further input during the run.
Installation elapsed time includes observation/input delays and is not a
performance comparison. This closes the fresh-installer gate for the native
PUSH candidate, while broader useful-work performance remains open.

[Manifest, exact executable identity, reset/application/exit evidence, compressed
logs and screenshots](measurements/fresh-win95-memory-push-2026-10-26.json).


## Uninterrupted run with bounded native batching (26 October)

The exact frozen executable measured for [native batching](native-batching.md)
passes a new clean-DOS OSR2 installation. All 56 installer files stage and verify
in an 81,999,324-byte external compressed child. A known-screen OCR/keyboard
driver completes the Setup wizard, using previously authorized OEM fields from
an ignored local transcript and enabling network-adapter detection. Unknown
screens would retain the original guest; no intervention or recovery is needed
in this run. The driver source is preserved without credential values.

The original process observes copy-complete Finish and reset 0→1, first-boot
network/clock/printer/final-Setup configuration and reset 1→2, then Explorer.
Calculator 2+2 transfers through the clipboard to Notepad, a unique file saves,
and a guest-requested restart returns to Explorer after reset 2→3. Screenshots
of the saved text and final desktop have been inspected.

The same guest then receives `quit`; actual exit status 0, PID absence and the
normal quit trailer are independently checked. A separate read-only FAT reopen
matches all 38 expected saved bytes. The 154,444,294-byte compressed child and
its index are removed after verification. Existing DOS, Win95 and Office parents
are preserved; no raw large disk or Office reinstall is made.

This validates fresh Setup under the new bounded polling granularity. It is a
functional result, not an installation speed comparison or universal performance
claim. The original process is never interrupted, replaced or manually recovered.

[Manifest, exact executable/PID identity, known-screen driver, reset/application/
exit results, compressed logs and screenshots](measurements/fresh-win95-native-batch-2026-10-26.json).
