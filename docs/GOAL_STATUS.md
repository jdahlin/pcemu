# Boot and performance status

Updated 2026-10-24. Work remains in progress.

- MS-DOS 6.22: boots the supplied VHD converted to a private raw image.
- Windows 3.11: standard-mode Program Manager, Notepad, and Calculator run.
  The independent full installer on `media/win311-install.raw` has copied all
  files, configured printers, and created Program Manager groups. Setup reports
  completion, and a cold reboot to standard-mode Program Manager is verified.
  Evidence: `build/win311-setup-complete.png`, `build/win311-cold.png`.
- Windows 95 OSR2 installer: first-stage file copying completed on
  `media/win95-install-v2.raw`, and Setup reached its restart request.
  The missing firmware reset-vector entry prevented automatic reboot. After
  stopping that instance, a private clone `media/win95-phase2.raw` cold-booted
  the installed system into graphical setup startup (`build/phase2.png`).
  Final setup completed and reached Explorer/Welcome
  (`build/win95-installed-desktop.png`). Calculator evaluates 2+2 as 4
  (`build/win95-installed-calculator.png`). Notepad saved exact file contents
  on FAT: `PCEMU installed Windows 95 passed.` in C:\PCEMU95.TXT
  (`build/win95-installed-notepad.png`). The completed disk cold-boots under the reset-capable binary
  (`build/win95-installed-cold.png`), without installer media or a setup command.
- rtts/win95: downloaded disk matches the repository's SHA-256 LFS pointer.
  Normal boot reaches Explorer. Calculator evaluates 2+2 as 4 and
  Notepad saved exact file contents on the FAT disk. Evidence:
  `build/rtts-calculator.png`, `build/rtts-notepad.png`, `media/rtts-v4.raw`.
  The optimized backend passes a same-process guest restart and exact file
  survival (`build/rtts-optimized-restart/result.json`). Clean shutdown reaches
  the complete safe-to-turn-off screen (`build/shutdown-optimized.png`), with
  real-mode terminal spin and interrupts disabled. Startup hardware warnings
  still require dismissal.
- Performance: native AArch64 register operations, direct branches and guarded
  8/16/32-bit RAM MOVs are accelerated, including paging. Latest synthetic memory
  loops reach 2,078 million guest instructions/s flat and 923 million/s paged.
  Indirect control flow and many instruction forms remain interpreted.
  Application performance comparable to Rosetta has not been demonstrated.

The implementation now includes original ATA PIO, dual 8259 PICs, an
instruction-clocked 8254 PIT, VM86 transitions, paging permissions and a software
TLB. An opt-in guest VGA ROM path avoids host BIOS shortcuts inside the VMM.
Hardware and CPU coverage remain incomplete; no existing emulator core is used.

Original archives and the downloaded rtts source disk are preserved. Installer
and boot experiments use writable private copies. `--control-file` permits guest
keyboard input and captures during headless testing; RAM dumps are diagnostics,
not restorable machine snapshots.

## Windows 95 investigation history

Earlier, both the mutable logged-boot disk and a fresh clone reached the
same wait. The current dump is `build/rtts-diskfix.bin`. At 300 million guest
instructions, the VMM is processing event restrictions with PIC IRQ0 functioning.
A thread event at `0xc0ff4bac` has handler `0xc101b704`, reference data
`0xc13d7e54` (the I/O packet), and flags `0x131`. The current scheduler mask
`0x800000e2` excludes this event. This is diagnostic evidence, not an identified
root cause. The nested V86 return BOP at `073a:2ff0` raises #UD and is recognized
by the VMM; do not treat that trap itself as an unsupported instruction.

`--break-at LINEAR --after-break N --trace FILE` permits a bounded trace after a
specific dispatcher address. `tools/inspect_guest.py` reads virtual addresses in
a RAM dump using its accompanying JSON paging state. No RAM/device restore is
implemented.

Archived Microsoft Windows 95 DDK HTML is available locally under
`build/ddk-reference/MSDN/WIN95DDK/HTML` (documentation only), from
https://github.com/techshelps/techshelps.github.io/tree/main/MSDN/WIN95DDK .
Useful event references are `S31E4.HTM` (Call_Priority_VM_Event) and `S31E8.HTM`
(Call_Restricted_Event).

The default launcher uses `media/win311.raw`, a private clone of the completed
full installation. `build/default-desktop.png` verifies the launcher path;
`build/win311-app.png` shows Calculator evaluating 2+2 as 4.

The earlier scheduler wait was traced to VPICD virtual-interrupt state. BIOS
IRQ stubs now execute their EOI with guest OUT instructions, allowing the V86
monitor to observe acknowledgement. The next visible fault was 0D at
`0117:6522`; the preceding `POP dword [ESP+8]` exposed an incorrect pre-increment
POP destination calculation, now fixed and covered in both operand-size modes.
A fresh source clone is used for each new first-boot investigation.

Latest completed application integration check:
`build/win311-smoke-z85olyjk`, with exact Notepad file readback after fixing
mode-dependent host key routing. Host keys now always enter the controller: a
brief real-mode transition inside Windows no longer diverts a key into the BIOS
queue. Firmware translates IRQ1 scan codes, while INT 16h AH=05 inserts BIOS keys.
Unit suite: 288310 checks passed, including native branches, mixed stack
address sizes on privilege transitions, and segment-type exception precedence. A second independent integration run
(`build/win311-smoke-o02y1g92`) also saved exact contents and exited cleanly. The optional
`make smoke-win311` check requires Tesseract to synchronize on the emulator's
rendered framebuffer artifacts. It does not inspect native macOS windows.

The modified rtts disk `media/rtts-reboot2.raw` cold-boots through ScanDisk to
Explorer, following network/display configuration prompts. Shutdown still fails:
its VMM stack contains a fatal exception 0E report at `0137:BFF9A3C0`. The
framebuffer retains the shutdown dialog rather than displaying the error text.
Evidence: `build/reboot2.bin` and `build/shutdown-kernel.bin`; this is an
unresolved failure, not a successful shutdown. The reported address follows a
call into the kernel exception-raising path, so it is not yet the root cause.

Direct short/near JMP and Jcc now terminate native blocks. A Win3.11 file-save
check passed with this backend (`build/win311-smoke-vwge_x48`). A later rtts
cold boot uses the same branch backend (`media/rtts-np-stop.raw`) for shutdown
diagnostics. The original shutdown trace is `build/shutdown-faults.log`: the
first relevant #NP is at `0777:4eae`, loading ES from DX=1a97, followed by a
#GP in the Windows handler at `0117:0c9b`. The root descriptor is being examined.

At the diagnostic stop before #NP delivery (`build/np-stop.bin`), selector
1a97 has descriptor bytes `d0 1a 00 00 00 00 0f 00`: an invalid system type,
not a loadable data segment. MOV ES must raise #GP before testing presence.
The emulator previously raised #NP instead; type checks now precede presence
checks, with regressions for invalid system/unreadable-code/stack/code types.
This follows Intel 80386 PRM, MOV protected-mode operation (PDF page 345).
A fresh boot with this fix and the full-ESP return fix uses `media/rtts-typefix.raw`.
Win3.11 exact file-save/exit passed (`build/win311-smoke-dbul0l7_`).

32-bit call gates and IRETD/RETF now preserve full saved ESP even with a
16-bit stack descriptor; parameter addressing still uses SS.B. Both stack
address sizes have privilege-transition tests. Reference: Intel SDM Volume 2B,
RET RETURN-TO-OUTER-PRIVILEGE-LEVEL:
https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf

With full-ESP and descriptor-type fixes, rtts shutdown passed the earlier
Explorer fault and left protected-mode GUI execution. It then waited in a
blank firmware/DOS keyboard screen; pressing Enter eventually stopped on an
invalid real-mode instruction at `857d:74c1`. Clean shutdown remains unproven.
Evidence: `build/typefix.bin`, `build/typefix.log`, `build/typefix-faults.log`.

Firmware reset now supports a jump through FFFF:0000, 8042 even pulse
commands F0..FE, output-port RESET# assertion, and port-92 fast reset. It
resets CPU and implemented devices, rebuilds IVT/BDA, optionally reinitializes
the permitted VGA ROM and reads the guest boot sector. RAM and disk contents
are preserved; the diagnostic instruction clock remains monotonic. Full ROM
POST is not implemented. Unit regressions cover reset from protected/paged
state and device restoration. `make smoke-reset` passed three actual DOS
programs, each followed by a verified second DOS boot through the VGA ROM.
Evidence: `build/reset-smoke-a22cll9t`.

The completed first installation was stopped after requesting a guest restart
and reaching a blank V86 firmware keyboard wait. Its disk was cloned to
`media/win95-installed.raw` for cold-boot testing with the reset-capable binary.
The earlier stalled state is preserved as `build/win95-phase2-shutdown.bin`.
A second reset-path bug is fixed: nonreturning INT19 ROM stubs no longer write
caller CF/ZF into the newly loaded boot sector after changing SP. This has a
regression using nonzero bytes at boot-sector offset four.

A DOS HMA flush loop exposed incorrect flags returned by saved BIOS vectors:
INT16 AH=1 could preserve stale caller ZF when working ZF already matched the
service result, then AH=0 blocked on an empty buffer. ROM callbacks now seed
CF/ZF from the saved caller flags image and write explicit result bits back.
Chained real-mode INT16/INT13 regression programs deliberately change working
flags after PUSHF. Win3.11 file-save/exit passed again (`build/win311-smoke-tcdkc_fy`),
and all three DOS reset programs still pass (`build/reset-smoke-xe6olyk4`).
The clean Win95 disk is live under this fix with `build/flags-control.txt`.

Win95 Restart now passes the earlier empty-keyboard flush wait and requests
a firmware reset (counter increases to one), then enters Windows startup again.
Return to Explorer is being verified with a bounded live-guest checker; it was
not established by the earlier captures. The VGA option ROM has 643 runtime
byte differences even after DOS-only tests, so these differences are not proof
of Windows patching. Restart now reloads the original asset before VGA init.
An explicit shadow-restoration regression passes.

`tools/check_win95_restart.py` waits for Explorer, requests Restart, requires
one actual firmware reset, waits for Explorer again, and checks the saved
Notepad file from FAT. Timeout leaves the confirmed-live guest intact. The
current run uses PID 47303 and `build/win95-restart-check.log`; evidence will be
written to `build/win95-restart-romreload` only on success.

Completed reset integration proof: `build/win95-restart-romreload/result.json`
records one guest-requested firmware reset, a return to the unobscured Explorer
desktop, and exact C:\PCEMU95.TXT readback after reboot. Before/after screenshots
are in the same directory. The driver configuration warning was dismissed
through the UI. No emulator restart or external VM was used during this check.
The checker reads modal interiors because sparse OCR misses their bordered
text, and validates the central desktop is unobscured before accepting icons.

Guarded 16/32-bit MOV memory operands now execute inside native ARM64 blocks.
512 differential address/segment/operand-size/paging combinations and precise
PF/GP, MMIO, watched-store and in-block SMC exits pass. Windows 3.11 exact
Notepad file-save/exit passed (`build/win311-smoke-rioqrub4`). The installed
Win95 disk also cold-booted and completed an actual guest restart with exact
file survival on this backend: `build/win95-memory-restart/result.json`.
Synthetic memory loops improved 3.04x (flat) and 3.98x (paged) over the reference
interpreter; see `docs/performance.md`. This does not establish Rosetta-like
Windows application performance. The rtts source clone is being rechecked on
the same backend with `build/rtts-memory-control.txt`.

The rtts source clone also reached an unobscured Explorer desktop under the
guarded-memory backend: `build/rtts-memory-desktop/result.json` (desktop-only
bootstrap check; this is not an application-I/O or restart assertion). Arena
recycling now has memory-operand lifetime regressions. A TLB flush with unchanged
code mapping reuses decoded code after permission/mapping checks; remapping and
fetch-permission revocation still invalidate or fault correctly. Capture metadata
now records native instruction counts, compiled blocks and arena recycles for
whole-guest profiling.

The rtts source clone also ran Calculator (2+2=4) and Notepad on the guarded
memory backend. Exact C:\PCEMU95.TXT bytes were read from FAT after saving:
`PCEMU installed Windows 95 passed.` Screenshots: `build/rtts-memory-calculator.png`
and `build/rtts-memory-notepad.png`. Its actual guest restart/file-survival check
is now running with PID 51282 and `build/rtts-memory-restart.log`.

The rtts restart/file-survival check passed: `build/rtts-memory-restart/result.json`
records a real firmware reset, return to unobscured Explorer and unchanged
Notepad file contents. Both the fresh OSR2 installation and rtts source clone
have now passed this integration on the guarded memory backend. Clean shutdown
and Rosetta-level application performance are still unverified.

## Cached memory backend and shutdown verification

The optimized memory backend retains GPRs and deferred flags across successful
RAM accesses, committing exact state at guarded exits or block completion.
The suite passes 288326 checks, including chained accesses followed by a fault.
The rtts restart and exact file-survival test passes on this backend
(`build/rtts-optimized-restart/result.json`). VGA scanout now derives geometry
from CRTC registers and selects 256-color shifting independently of chain-4.
Windows shutdown renders its complete unchained 320x400 safe-to-turn-off logo
(`build/shutdown-optimized.png`), with CR0=0x10, IF clear and the terminal
NOP/JMP loop at 0959:0213. The host process was then asked to quit.

Synthetic memory results improved to 7.05x flat and 6.83x paged versus the
interpreter; these do not establish application parity with Rosetta.

Fresh integration after the memory/VGA changes passes Windows 3.11 Notepad
save and exit (`build/win311-smoke-n6bhjzgt`) and all three DOS firmware reset
paths (`build/reset-smoke-cb1heisz`). Formatting and the final unit run preserve
288326 passing checks. A new matching memory comparison measures Rosetta at
11,778 million equivalent guest instructions/s versus PCEmu flat RAM at 388
million/s, approximately a 30x gap. Performance remains a substantial open goal.

## Inline RAM backend

Inline ordinary RAM and valid TLB hits now avoid C++ helper calls. The unit
suite passes 289133 checks, including hot-path call counts, RAM/bitmap host
relocation, changing segment bases, remapping, observer installation, warmed
permission failures, VM86 supervisor override, A20, split accesses, MMIO and
code bitmap boundaries. Windows 3.11 save/exit passes
(`build/win311-smoke-09dwtxs1`). The rtts guest restart and exact file survival
also pass (`build/rtts-inline-restart/result.json`). Independent OSR2 restart
and exact file survival also pass (`build/win95-inline-restart/result.json`),
using a new private disk `media/win95-inline.raw`.

Checked memory throughput improved to 1.66 billion guest instructions/s flat
and 0.84 billion/s paged; Rosetta measures 12.13 billion equivalent guest
instructions/s on the matching native pattern. The gap is smaller but remains
substantial. Broader instruction coverage and lower per-access guard costs
remain open performance work.

The rtts disk also reaches the full safe-to-turn-off screen on the inline
backend (`build/rtts-inline-shutdown.png`), with real CR0=0x10, IF clear and
the terminal 0959:0213/0214 spin. Its host process was then stopped cleanly.

The independent OSR2 installation also reaches the complete safe-to-turn-off
screen on this backend (`build/win95-inline-shutdown.png`), with CR0=0x10,
IF clear and its terminal real-mode spin. All three DOS reset paths pass
(`build/reset-smoke-37ugjcag`). Both Windows 95 host instances were then asked
to quit. The default `media/win95.raw` is now a private clone of the verified,
cleanly shut down rtts working image; its prior contents are preserved as
`media/win95-pre-inline.raw`. The immutable author disk remains unchanged.

Next performance work: reduce repeated per-access pointer materialization and
guard work, then expand lowering beyond MOV/register ALU/direct branches.
Full application performance near Rosetta remains unproven.

## Pointer caching and REP restart correctness

Cached RAM/bitmap pointers and compact CPU-relative TLB guards improve checked
throughput to 2.08 billion guest instructions/s flat and 0.92 billion/s paged.
REP fault handling now retains completed elements and restores comparison flags
across scheduling batches, verified against the downloaded Intel SDM. The final
suite passes 289431 checks. Windows 3.11 save/exit passes
(`build/win311-smoke-ofo0ri40`); the rtts restart/file-survival check passes
(`build/rtts-hoisted-restart/result.json`) and shutdown reaches its full safe
screen (`build/rtts-hoisted-shutdown.png`). All three DOS firmware reset paths
pass (`build/reset-smoke-4afxc797`).

A fresh OSR2 installer is copying files on `media/win95-fresh-hoisted.raw`,
PID 56657, using the pointer-caching build from before the REP fix. This run
starts from the untouched DOS baseline and will verify Setup's first automatic
firmware reboot, which the earlier installation could not exercise before reset
was implemented. `tools/check_setup_reboot.py` observes its copy phase and
acknowledges only the explicit Finish/restart screen; it leaves unknown states
or observation timeouts alive. Output: `build/win95-fresh-first-reboot`.
The completed independent OSR2 disk is separately testing the latest REP fix
with PID 57957 and `build/win95-hoisted-restart.log`.

Remaining performance work includes wider opcode lowering (notably 8-bit
register/memory operations and stack/control flow) and reducing repeated
translation guards. A host sample of Setup (`build/win95-setup-host-profile.txt`)
shows most CPU time still in the interpreter, and native coverage during Setup
is only about 4%; the memory microbenchmark therefore overstates whole-guest
coverage. Rosetta-level application performance remains open.

The fresh install now completes its first Setup-requested reboot automatically
through firmware in the same process: `build/win95-fresh-first-reboot/result.json`
records PID 56657, reset count 0 to 1, at 40,389,329,577 retired instructions.
The verifier acknowledged the explicit Finish screen. Phase two has started
and reached the normal missing-mouse warning; installation completion is not
yet claimed.

The completed independent OSR2 disk passes the pointer/REP backend's restart
and exact-file check (`build/win95-hoisted-restart/result.json`) and reaches
the full shutdown screen (`build/win95-hoisted-shutdown.png`), with verified
real-mode terminal spin and IF clear. Its host process was then stopped.

Interpreter RAM accesses also specialize 1/2/4-byte loads and stores, preserving
all existing permission/device/code-write checks and the uncommon-width path.
The checked interpreter loops improve to 68.6 million/s flat and 44.9 million/s
paged; JIT rates remain about 2.09 billion/s and 0.924 billion/s. Raw evidence:
`build/rosetta-fixed-interpreter-memory-benchmark.json`. The fresh installer was
resumed after measurement and is configuring Control Panel in phase two.

The interpreter fixed-width RAM change also passes Windows 3.11 save/exit
(`build/win311-smoke-_yas1fk7`), and the suite remains at 289431 checks with
no compiler warnings. Phase-two fresh Setup reached Date/Time Properties;
the selected timezone is (GMT+01:00) Berlin/Stockholm, with daylight saving
enabled, matching the user's host region.

## Fresh installation completion and snapshots

The fresh OSR2 run completed phase two, reached Explorer in the same process
(`build/win95-fresh-desktop/result.json`, reset count 1), ran Calculator 2+2=4
and saved exact C:\PCEMU95.TXT contents through Notepad. Evidence:
`build/win95-fresh-calculator.png`, `build/win95-fresh-saved.png`. It then
reached the safe-to-turn-off screen and a terminal real-mode spin
(`build/win95-fresh-shutdown.png`).

At the user's request, full restorable VM snapshots and an original compressed
COW disk format are implemented. zstd level 1 compresses zero-page-elided RAM
and changed 4 KiB disk blocks. Restores allocate only a new delta; obsolete
records compact and unchanged writes do not grow the file. APFS shares the
initial backing. Pending PIO reads/writes, CPU/device/queue round trips, stale
JIT rejection, corrupt input rejection and log compaction pass 2863 dedicated
checks, alongside 289431 CPU/device checks (292294 total).

Two saved points: `build/snapshots/win95-desktop` and
`build/snapshots/win95-calculator-zero`. Live save/restore is about 60/53 ms;
the latter RAM payload is 5.9 MiB and its overlay is 103 bytes. Ten independent
restores verified Calculator output (five JIT, five interpreter), while deleting
their disposable COW files. Recorded task fixture and runner are available.
The JIT task median is 119 ms versus 82 ms interpreted, including control and
capture overhead: a concrete remaining performance regression.

The snapshot-backed fresh guest also shut down cleanly. Its final compressed
disk delta was preserved as `media/win95-fresh-installed.pcd` (25 KiB before
subsequent boot), sharing the immutable checkpoint backing. Unneeded live
deltas and the 64 MiB diagnostic dump were removed. A cold boot of this COW
disk is now running as PID 61143, with `build/win95-cow-cold-desktop.log`.
The formatter's backing references are absolute paths and require the parent
checkpoint directories to remain in place. Overall Rosetta-level performance
is still not proven; the original broad goal stays active.

The compressed fresh-install disk cold-boots under the latest binary and
reaches Explorer (`build/win95-cow-cold-desktop/result.json`). Notepad opens
C:\PCEMU95.TXT with the exact original fixture text
(`build/win95-cow-cold-notepad.png`), verifying the COW backend through actual
firmware/Windows disk reads rather than only restored RAM. Snapshot temporary
deltas are cleaned after successful tasks. The full test total is 292294.

The COW cold-boot guest shut down cleanly again
(`build/win95-cow-cold-shutdown.png`), with CR0=0x10, IF clear and the terminal
real-mode loop. Its host process was stopped; the persistent compressed disk
is `media/win95-fresh-installed.pcd`. No test emulators remain running.

### Compressed checkpoints and byte JIT

Snapshots omit zero RAM pages and compress state with zstd level 1. Disk
checkpoints share immutable parents and append compressed 4 KiB COW records,
with automatic obsolete-record compaction. The Win95 Calculator checkpoint
uses 5.9 MiB of compressed state and a 103-byte initial disk overlay. Five
independent restores pass the recorded pixel-checked task. Byte JIT lowering
passes register, memory, fault, MMIO and SMC differential checks. The complete
test suite passes 474,861 checks. Whole-application Rosetta performance remains
unproven; Calculator's cold-JIT response is still slower than the interpreter.

### Office 97 installation and startup optimization

Office 97 Professional SR-1 installs normally on the independent OSR2 disk.
Word saves C:\WORD97.DOC and Excel evaluates =2+2 as 4 and saves C:\EXCEL97.XLS;
both files reopen after an actual Windows restart. `run-office97.sh` restores
its configured desktop, and `--setup` restores the pre-installation wizard.
The original archive is preserved; temporary raw staging and extracted ISO
copies were removed. Compressed COW checkpoints share immutable backing.

Separate Word/Excel post-boot launch fixtures have fresh native caches and
pixel-checked rendered status bars. Native LEA, register PUSH/POP and immediate
PUSH lower median startup from 2.522/1.184 s to 2.210/1.048 s (five samples per
application/configuration). Interpreter baselines are 2.214/0.979 s; broad
Rosetta-class performance remains unproven. COW index mapping lowers median
checkpoint readiness from about 1.06 s to 0.25 s. All 539,036 checks pass,
including stack faults, MMIO, mixed widths and code invalidation, plus Win3.11
save-file and all three DOS reset smoke paths. See docs/office97.md and
build/office97-startup-summary.json. Work remains in progress.

### Whole-application startup profile

Host samples and exact interpreter/JIT diagnostics cover both Office startups,
including the normal SDL frontend. Interpretation is about 45% of sampled
main-thread work, JIT dispatch about 16%, polling about 10%, disk/compression
under 1%. Worker waits and profiler attachment barriers are excluded from the
main-thread denominator; screenshot/capture overhead is reported separately.
Routine unsupported JIT decode no longer throws C++ exceptions, and PIC
polling avoids duplicate/empty priority scans without changing deadlines.
Five alternating unprofiled SDL pairs improve Word 2.524 → 2.209 s and Excel
1.210 → 1.023 s. All 541,340 checks, Win3.11 save-file and three DOS reset
smokes pass. docs/startup-profiling.md records evidence, limitations and next
priorities: interpreted dispatch/MMU, memory ALU lowering, calls/returns and
block chaining. Broad Rosetta-class performance remains unfinished.


## 24 October: input and Windows 95 display integration

Original PS/2 mouse/AUX controller, extended host keyboard sequences, focus-loss
release, DISPI/VBE linear/banked framebuffer and PCI/BIOS32 discovery are now
implemented. Windows 95 loads its built-in PS/2 mouse driver and the external
VMDisp9x direct display driver, with 1024×768/32-bit output. Word/Excel reopen
saved files, Word F12 opens Save As, and mouse left/right clicks work in Windows
and Excel. `run-office97.sh` prefers the new integrated checkpoint; the old VGA
benchmark fixtures remain untouched. Guest PIC mask writes execute in firmware
so Windows' V86 monitor sees mouse interrupt enable/disable.

The disk delta is 2,453,493 compressed bytes relative to the existing immutable
Office desktop and was compared over all 2 GB of logical content. State is about
18 MB with zstd. See [input/video integration](input-video.md). This does not
establish Rosetta-like performance; the new display workload has not replaced
the earlier VGA startup measurements.

## 24 October: original NE2000 and Netscape networking

Added ISA NE2000/DP8390 at 300h IRQ10 and an original unprivileged macOS
ARP/DHCP/DNS/UDP/TCP socket gateway. Windows95 built-in NE2000 driver obtains
10.0.2.15, DNS 10.0.2.3. Netscape3.04 installed and rendered gateway HTTP and
external example.com after cold boot. Snapshot v4 preserves device and wire queue,
closes host sockets on restore/reset. Existing v1–3 checkpoints still load.
The networked Office checkpoint uses a compressed delta and retains the previous
checkpoint. No new Rosetta-level performance claim. See docs/networking.md.


## 24 October: native read-only memory CMP/TEST

ARM64 lowering now includes 8/16/32-bit memory CMP and TEST in the forms used by
Office startup. Existing MMU/MMIO/SMC guards preserve precise replay and flags.
102,804 new differential checks bring the suite to 644,288 passing checks;
Win3.11's exact Notepad save and all DOS reset smokes pass. Five alternating SDL
pairs improve Word 2.391→2.242 s (6.2%) and Excel 1.080→0.989 s (8.4%) on the
unchanged VGA fixtures. A separate GUI emulator remained active throughout;
no build/test load overlapped the reported measurement. High-resolution startup
and broad Rosetta-level performance remain unproven. See docs/startup-profiling.md.


## 24 October: hot short comparison blocks and register TEST

Two-instruction CMP/TEST+Jcc blocks now pass production warmup. Added native
immediate register TEST (A8/A9 and F6/F7 /0), retaining partial/high-byte state.
651,977 checks pass, including 110,493 read/compare checks and production warmup;
Win3.11 save and all three DOS reset smokes pass. Separate five-pair SDL trials
show memory pair admission reducing Word/Excel startup by 5.2%/3.7%; register
pairs/TEST then reduce it by 1.3%/6.8%, with noisy first-run outliers retained.
An initial run's Start menu overlay led to explicit host-input suppression for
GUI automation, verified in both rebuilt comparison engines. Normal interactive
input remains enabled. Native-display startup and broad Rosetta performance
remain unfinished. See docs/startup-profiling.md and build/short-compare-validation.json.

## Current automated Win95 cold boot and restart

`tools/smoke_win95.py --restart` now creates a private compressed COW child of
`media/rtts-win95.img`, cold-boots through guest BIOS/VGA ROM execution, dismisses
known startup dialogs (including the DHCP failure notice), verifies unobscured
Explorer, requests Restart through the guest keyboard, and verifies exactly one
firmware reset followed by Explorer again. `--disk media/win95-fresh-installed.pcd`
runs the same check against the completed local OSR2 installation. No saved RAM
state or native virtualization backend is used in these runs.

Both sources passed independently with the current x86 interpreter/ARM64 JIT:

- rtts: `build/win95-smoke-msfls7t5/result.json`, with resets 0 -> 1.
- Installed OSR2: `build/win95-smoke-3u2s5byf/result.json`, with resets 0 -> 1.

Both runs removed their private compressed disk after success and retained
framebuffer/log/JSON evidence. These checks assert cold boot, reset delivery and
visible desktops; they do not assert file persistence, rerun the installer, or
establish Rosetta-level application performance. The optional `make smoke-win95`
target performs the shorter cold-boot-only variant using the original rtts disk.


## Fresh OSR2 installer with bounded joined blocks (24 October)

The current JIT's opt-in `--jit-follow-jumps` mode now passes a fresh full OSR2
installation from the original supplied DOS VHD, rather than an installed disk
or restored RAM state. `tools/prepare_win95.py` stages the extracted CD into a
compressed COW child, validates every source file by readback, and rejects
Windows-containing working parents. The untouched VHD converts to an 8.6 MiB
allocated sparse backing; initial compressed installer staging is about 80 MiB.
See [repeatable preparation](win95-setup.md).

The same emulator process completes file copying and its first Setup-requested
firmware reset (0 -> 1 at 28,815,782,943 guest instructions), then phase-two
configuration and its second requested reset. Unobscured Explorer is verified
at reset count 2 and 31,514,958,282 instructions. Calculator displays 4 for
2+2. Notepad saves C:\PCEMU95.TXT; after another guest restart (2 -> 3), stopped-
guest FAT readback matches all 45 expected bytes, including CRLF. A separate
private child cold-boots to Explorer and passes another guest restart (0 -> 1),
with no Setup launch command or RAM checkpoint.

Evidence: `build/win95-joined-installer/result.json`, `first-reboot/result.json`,
`desktop/result.json`, `restart/result.json`, `calculator.png`,
`notepad-saved.png` and `file-verification.json` within that directory;
`build/win95-smoke-qsm7rud0/result.json` records the independent cold boot.
The evidence records the emulator hash and preparation/source hashes. Passing
runs remove their temporary COW children and sparse DOS backing; original
archives and the existing installed system/checkpoints remain available.
No experiment guest remains running. This is installer correctness evidence,
not an installer performance comparison or proof of Rosetta-level speed.
