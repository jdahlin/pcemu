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
