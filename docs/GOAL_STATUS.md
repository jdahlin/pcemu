# Boot and performance status

Updated 2026-10-26. The full goal remains active. The sections below the current
status retain historical investigations; old failures and pending gates there
must not be read as the latest result.

## Current verified state

- **MS-DOS 6.22:** the recovered external QCOW2/PCD parent boots through its
  version and ready marker using the native PUSH candidate.
- **Windows 3.11:** its completed installation boots headless, saves in Notepad,
  returns to DOS and passes exact 25-byte file readback with the same candidate.
- **rtts/win95 and installed OSR2:** both cold boot, run Calculator 2+2, copy the
  result through the clipboard into Notepad, save a fresh file, restart through
  firmware and pass exact stopped-guest 38-byte file readback.
- **Fresh OSR2 Setup:** the native PUSH candidate completes clean-DOS
  installation, all first-boot configuration and application/restart/persistence
  in one guest process. Three resets, actual process exit status 0 and exact
  38-byte readback pass. Its private compressed installation child is removed.
  See [fresh installation](fresh-win95-validation.md#uninterrupted-run-with-native-ff-6-push-26-october).
- **Office 97:** existing compressed checkpoints are reused. Loaded Excel's
  4096-row dependent calculation and Word's warm launch reach the expected
  rendered results with the PUSH candidate. Earlier persistence checks cover
  Word text and Excel formulas across save/restart/reopen; this turn does not
  repeat those full document-persistence checks.
- **Snapshots/storage:** compressed machine snapshots and small compressed disk
  children are implemented. Restores do not preserve JIT code or its heat.
  Recovered parents and retained checkpoints live on the external volume.
  Successful smoke/task disks are removed; installation media, disks and builds
  stay out of Git. Raw RAM dumps remain diagnostic artifacts, not snapshots.

The emulator uses an original x86 core, AArch64 JIT, software TLB, ATA PIO,
8259 PICs, 8254 PIT, PS/2 input and guest VGA path; it does not reuse another
emulator's CPU or device implementation. Instruction and device coverage remain
incomplete. Native instruction coverage alone does not establish good application
performance.

## Performance evidence and open work

The user's practical stretch target is roughly another 2× on useful Win95 work;
Rosetta equivalence is not the current acceptance target. That twofold useful-work
improvement is **not demonstrated**. Historical percentages below describe
individual experiments and must not be multiplied into a cumulative claim.

[Current Office host profiles](office-current-profiling.md) separate main-thread
stack samples from actual process CPU accounting. Loaded Excel spends many
samples in dispatch/validation and device polling; compilation contributes under
1%. Word launch has much more interpreter and compilation work. Anonymous
emitted ARM64 frames now have diagnostic region attribution: [mapped Excel
profiles](emitted-code-profiling.md) identify block finishing as a substantial
bucket, while finer flags/commit/return separation remains open.

[Native FF /6 PUSH](native-memory-push.md) passes 8,682 focused memory checks,
43,600 stack checks, 354,948 core checks and 8,677 existing memory CALL checks.
Its deliberately PUSH-heavy RAM loop improves 2.63–2.85× across eight mode cases;
this is not a general Win95 or Office gain. Separate Excel counters reduce
interpreted FF /6 attempts from 1.33M to 60K. No Office-wide timing gain is
claimed for this change. Frozen source/binary identities and OS gates are in
[the measurement record](measurements/native-memory-push-2026-10-26.json).

[Global conditional traces](conditional-trace-evaluation.md) help loaded Excel
but do not consistently help Word launch. The subsequent [adaptive prototype](adaptive-traces.md)
is rejected: the disabled-path controls do not establish acceptable overhead,
and other workloads started during those controls. Production JIT policy remains
unchanged. Both favorable and unfavorable measurements are retained.

The remaining completion work includes continued useful-work performance
improvement with reproducible comparisons, and verification of any broader
supported behavior claimed for the final build. Latest-lowering fresh Setup is
now verified; historical pending statements below predate that result.

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


## 25 October: register NEG and current compatibility

Register NEG now executes natively at byte/word/dword widths. 237,334 flag
checks, 341,306 core checks, 156,510 read-JIT checks and 2,957,189 byte-JIT
checks pass. Exhaustive byte aliases, wider signed/carry boundaries and precise
fault/code-invalidation exits cover the new lowering. Current headless checks
save exact Notepad contents in Win3.11 and cold-boot/restart both retained
Win95 images to unobscured Explorer, removing their private compressed disks.
The short DOS profile verifies matching JIT/interpreter checksums and guest
instruction counts across all 64 microbenchmark intervals.

Five alternating pairs of uninstrumented 10,000-iteration DOS CoreMark trials
show a median paired throughput gain of 10.74% for native NEG. All runs pass
canonical CRCs and upstream minimum-duration validation, execute identical
3,975,092,643 guest instructions and return to DOS. The baseline differs only
in NEG decoding. See [correctness/profile evidence](measurements/native-neg-2026-10-25.json)
and [complete host timing results](measurements/native-neg-timing-2026-10-25.json).
This does not rerun fresh installation or prove Windows/Rosetta-level speed.
Historical paths earlier in this log may have been removed by storage cleanup;
these committed measurement records retain the latest results and provenance.


Register-destination ADD/OR/AND/SUB/XOR memory operands now execute through the
same guarded read path, with precise fault replay and register/flags commits
only after success. 243,295 read and 237,430 flag checks pass, as do core/byte
checks, DOS CRC/microbenchmark diagnostics, Win3.11 file-save and both Win95
cold-boot/restart checks. CoreMark interpreted attempts fall 186,285 -> 151,824
at identical guest work. This coverage result is not a host timing claim;
see [the detailed record](measurements/native-memory-alu-2026-10-25.json).


The memory-source ALU change now also has five matched, alternating host-timed
CoreMark pairs: median paired throughput improves 9.59%, with median intervals
13.6779 -> 12.5611 seconds. All ten 10,000-iteration runs pass canonical CRCs,
upstream minimum-duration validation and DOS return, with identical guest work.
This is incremental over NEG, not a Windows/Rosetta claim. [Complete timings
and provenance](measurements/native-memory-alu-timing-2026-10-25.json) are committed;
private overlays and temporary baseline/debug files were removed.


Fresh host call-tree profiles now cover both retained Win95 cold boots and a
separate rtts boot/restart. Interpreter work is about 51% of main-thread
samples, dispatch 17–18%, device polling 12–13% and disk/compression under 1%.
Software address translation alone is 11–13% in self samples. Named profiler
attachment waits are excluded, with raw call trees preserved as small zstd
archives. These windows include dialog/OCR observation and do not establish
isolated boot speed. `tools/smoke_win95.py --sample` reproduces collection;
see [the full host evidence](measurements/win95-host-startup-2026-10-25.json)
and [scope/method](startup-profiling.md). Both profiled cold boots, profiled
rtts restart and unprofiled OSR2 restart pass and remove private compressed disks.


## 25 October: live-state gate for idle PS/2 polling

The board now avoids controller/PIC-input polling when a live-state predicate
proves it cannot change state or raise an IRQ. 65,564 controller checks include
32,768 differential IRQ/state cases against unconditional polling; core,
snapshot/COW and input/video integration checks pass. Win3.11 exact save and
both Win95 boot/restart checks pass headless with successful compressed overlays
removed. Five matched, alternating DOS CoreMark pairs show a 5.16% median paired
throughput gain with identical guest/native work and validated CRCs/durations.
A fresh Win95 host profile remains dominated by interpreter/MMU and dispatch;
this does not establish broad Windows/Rosetta gains. [Complete evidence](measurements/ps2-poll-2026-10-25.json)
retains all samples, checks and a compressed raw call tree. No large VM fixture
was created.


## 25 October: demand-based paging privilege checks

The out-of-line MMU now computes CPL/system override only for restricted
permissions or fault reporting. 343,418 core checks (including 64 new nonpresent
VM86/system-access fault assertions), read/byte/flag suites, DOS CRC/micro checks,
Win3.11 file-save and both Win95 boot/restart checks pass. Five matched bounded
Win95 startup pairs show 2.67% median process throughput improvement at exactly
300M guest instructions, with all captured state equal except host timing/disk
path. RAM paged interpretation improves 2.73%, with a documented 1.23% flat
interpreter cost. This does not measure full Explorer readiness or establish
Rosetta speed. [Complete evidence](measurements/lazy-page-permissions-2026-10-25.json)
preserves samples and states; successful small compressed overlays and temporary
baseline files were removed.


## 25 October: current Win95 application and persistence proof

`tools/smoke_win95.py --applications` now exercises Calculator 2+2, copies its
result through the guest clipboard into Notepad, adds a fresh marker, saves a
unique file and requests a Windows restart. After exactly one reset and a
second unobscured Explorer desktop, stopped-guest compressed-FAT readback
matches all 38 expected bytes. Both retained rtts and installed OSR2 images
pass unattended on the current emulator; a profiled rtts application/restart
run and the default cold-boot regression pass too. No saved RAM state is used.
[Records, hashes and screenshots](measurements/win95-applications-2026-10-25.json)
are committed. Successful compressed disks and enlarged OCR scratch are removed.
This proves more than desktop readiness, while fresh installation and broad
Rosetta-level speed remain outside this check's evidence.


## 25 October: native memory INC/DEC

Guarded native byte/word/dword memory INC/DEC now preserve carry, precise replay,
device/watch ordering and code invalidation. 947,820 new RMW checks pass alongside
core/read/byte/flag suites. DOS CRC/profile diagnostics and Win3.11 save-file
validation pass; both Win95 images calculate 2+2, transfer the result through
the clipboard into Notepad, save a unique file, restart and match exact stopped-
guest FAT contents. Five alternating uninstrumented CoreMark pairs against the
complete pre-change baseline show 15.74% median paired throughput improvement,
with identical guest work and validated CRCs/durations. This remains a scoped
flat integer result, not broad Windows/Rosetta performance. [Full evidence](measurements/native-memory-incdec-2026-10-25.json)
is committed; successful compressed overlays and temporary baseline files are
removed. Fresh installer validation on this exact binary remains unproven.


## Native indirect jump validation (25 October)

Near FF /4 register/memory JMP now runs natively, including mixed operand and
address widths and guarded paged RAM. 17,861 differential checks include
precise read/fetch faults, MMIO replay, A20 and code-generation exit targets.
DOS kernels, Win3.11 save/DOS return and both Win95 Calculator/clipboard/
Notepad save/restart/exact file checks pass headless on the candidate binary.
Five complete-source baseline CoreMark pairs show median throughput +8.08%;
this is flat computation, not a measured Windows speedup.
Evidence: `docs/measurements/native-indirect-jump-2026-10-25.json`.
Indirect calls, far transfers, block linking and broader MMU/flags work remain.
The broad goal stays active; latest-binary fresh OSR2 installation and Rosetta
performance equivalence remain unproven.


## Memory performance workload (25 October)

Added a disk-light, headless dependent-load sweep with the same guest COM on
JIT, interpreter and QEMU. Twenty cases cover dense/sparse rings, flat RAM
and supervisor 4 KiB identity paging, with working sets above the software
TLB's capacity. Exact final pointers, engine-equivalent instruction counts
and rejection of a deliberately corrupted guest are verified.

Three paired JIT/QEMU runs show dense 16 KiB costs of 2.27 ns/load flat and
5.77 ns/load paged in our CPU-only harness, versus QEMU PC TCG 2.67 / 2.67.
These different harnesses do not establish overall emulator rankings. The
dense paged case has zero translation-helper calls and high native coverage,
providing an optimization target for inline MMU guard overhead. Sparse cases
above 1,024 pages exercise translation-helper pressure.
Evidence: `docs/measurements/guest-memory-sweep-2026-10-25.json`.
Streaming reads/writes/copies and user-mode memory workloads remain future
coverage; the broad boot/correctness/performance goal remains active.


## TLB-base hoist experiment rejected (25 October)

A measured attempt to cache the TLB-array address in a free host register
strongly regressed small paged pointer rings across three alternating pairs.
The emitter is restored; no production performance change is retained.
Exact guest/native/helper counts match, leaving the microarchitectural cause
unproven. Pressure tests remain and pass 255,535 read-JIT checks; restored
JIT/interpreter/QEMU memory smoke and corrupted-kernel rejection pass.
Evidence and rejected patch: `docs/measurements/rejected-tlb-base-hoist-2026-10-25.json`.
Future memory work needs to improve the complete lookup path and be measured
against both small dense rings and larger TLB-pressure cases.


## Cached CPU translation fast path (25 October)

Retained a stackless cached `CPU::physical` path with the original slow page
walk/fault/dirty-update behavior. Five paired startup-prefix runs on the exact
application-tested binary improve median process throughput by 3.6% and internal
execution throughput by 4.4%. The common paged interpreter RAM kernels improve,
but the largest interpreter TLB-thrashing case and some native kernels regress;
full paired data and limits are in
`docs/measurements/physical-translation-fast-path-2026-10-25.json`.

Current headless DOS benchmark validation, Win3.11 Notepad persistence, and both
rtts/installed-OSR2 application/restart/persistence checks pass. Existing core,
native-memory/flags/control-flow and snapshot suites pass. A fresh latest-binary
OSR2 installation and Rosetta performance equivalence remain unproven; the broad
goal remains active. Cold-miss guard duplication remains an optimization target.


## Slow translation context experiment rejected (25 October)

Passing the cached TLB slot and walk decision into the slow translator was
neutral on five rtts startup-prefix pairs (median throughput ratio 1.0033).
Three-pair interpreter RAM measurements improved common paged cases by roughly
2–6%, but the largest paged-stride case remained neutral. Native RAM results
were mixed, including a 0.927 ratio for one dense paged case. The experiment
is rejected and the previously validated CPU implementation is restored.
Evidence and exact patch: `docs/measurements/rejected-physical-slow-context-2026-10-25.json`
and the adjacent `.patch`. The broad goal remains active.


## Restored binary host profile (25 October)

The restored implementation passes another headless rtts Calculator/clipboard/
Notepad exact-file persistence check through one firmware restart. The private
compressed child was removed on success. Of 29,516 main-thread wall-stack
samples excluding profiler attachment waits, interpreter execution accounts for
51.1%, dispatch/cache/validation 18.1%, polling 11.1%, and native code/helpers
8.6%. These include OCR/control waits and are not startup benchmark timings.
`CPU::physical` self time is 8.26%; the next investigation should quantify
fallback instructions and transitions before another TLB hit-path experiment.
Evidence: `docs/measurements/restored-win95-profile-2026-10-25.json` and adjacent
compressed raw sample. Fresh installation on this binary remains unverified.


## Bounded Win95 interpreter coverage (25 October)

Added diagnostic execution-scoped `--guest-profile FILE` and a headless paired
normal/instrumented startup check. At 300 million retired instructions, captured
CPU/device state and all JIT counters match. The interval starts after VGA BIOS
initialization at instruction 294,669. There are 74,912,417 interpreter attempts,
including 1,391 native replays. Leading fallbacks include MOV segment (5.68M),
MOV r,rm (4.16M), far CALL immediate (3.52M), far RET (3.51M), and FF group
(3.43M). Frequency is not host execution cost; segment/control transitions need
investigation alongside memory lowering. Both successful compressed children
were removed. Evidence: `docs/measurements/win95-startup-opcodes-2026-10-25.json`.
The broad goal remains active, including current-binary fresh installer validation.


## Application-use profiling separate from startup (25 October)

Added optional diagnostic intervals after Calculator and Notepad have loaded.
Boot and application launch are excluded; Calculator arithmetic and Notepad
clipboard/edit/save execute inside separate intervals. Exact stopped-guest file
readback and firmware restart pass; the private compressed disk is removed.
Calculator's fallback mix includes MOV, FF group, XCHG, CLI and PUSHA/POPA,
rather than the segment-load/far-transfer-heavy startup ordering. Counts also
include message loops and input settling, and do not isolate computation cost
or establish Office performance. Keep startup, application use and kernels as
separate optimization workloads. Evidence:
`docs/measurements/win95-application-use-opcodes-2026-10-25.json`.


## Office installation restored on external storage (25 October)

All 5,781 ISO files (672,387,604 bytes) were staged and read back through a
compressed private child on `/Volumes/data`, without raw staging duplication.
Office SR-1 Setup reports successful completion. Word saves RESTORED.DOC with
the test text; Excel displays =2+2 as 4 and saves RESTORED.XLS. A guest restart
returns to Explorer. After clean emulator exit, offline FAT readback verifies
both compound-file headers/hashes and the exact Word text payload. Formula
persistence on reopening and representative steady-use Office profiles remain
next steps. Evidence: `docs/measurements/office97-restored-install-2026-10-25.json`
and adjacent setup/Word/Excel screenshots. The frozen runner hash is recorded.
The broad emulator goal remains active; this does not repeat Windows Setup.

## Excel use and recalculation coverage (25 October)

The restored Excel workbook reopens after the previous firmware restart with
A1 =2+2 and displayed value 4. Filling B1:B4096 with =ROW()*3+7 gives the
expected selected-range sum 25,200,640 and terminal value 12,295. A separate
post-load interval requests eight full recalculations. Opcode profiles show
byte arithmetic/test/shift and indirect control flow among common fallbacks;
these are frequency observations including idle/input settling, not host costs.
The saved PERF4096.XLS has a verified compound-file header/hash after clean
exit; its compressed child on /Volumes/data is 927,116 bytes. Raw ISO or large
fixture duplication was unnecessary. Evidence and exact control transcript:
`docs/measurements/office97-excel-steady-use-2026-10-25.json` and adjacent files.
Full saved formula-range parsing/reopening, fixed completion/timing boundaries,
and an idle baseline remain. No Rosetta-level or overall performance claim.

## Native byte MUL from Excel coverage (25 October)

Unsigned register byte multiply F6 /4 now runs natively, including legacy high
byte aliases, complete AX product and CF/OF semantics. Exhaustive differential
coverage and existing flag tests pass 11,771,766 checks; core passes 352,644.
Candidate Win3.11 persistence and rtts Win95 application/restart/persistence
checks pass headlessly with successful private children removed. Five paired
RAM-only MUL-loop runs show median 4.49x throughput versus old JIT source files,
with identical guest results and 50M native instructions versus 40M previously.
This is not a measured Office speedup. Evidence:
`docs/measurements/native-byte-multiply-2026-10-25.json`.
Fresh Windows Setup and representative Office host timing remain unverified.

## Excel recalculation completion gate (25 October)

A small external snapshot now separates loaded-application recalculation from
startup. Changing D1 from 3 to 4 updates 4,096 dependent formulas and a SUM
marker from 25,200,640 to 33,591,296. An independent snapshot restore reached
the exact new-total pixels; a no-input negative stayed at the exact old pixels
and failed as intended. Successful/negative temporary restore children were
removed. Added a validated restore-only timeout option for network storage;
it does not extend the task-completion timeout. One positive task observation
is 12.58s end-to-end / 11.04s execution batches, not a comparative speedup.
Evidence: `docs/measurements/excel-dependent-recalc-2026-10-25.json`.
Paired JIT timings and fresh Windows installation remain outstanding.

## Controlled Excel byte-MUL comparison (25 October)

Three alternating pairs from a shared frozen source tree verify exact dependent
SUM completion on both JITs. Median response throughput ratio is 0.998856 and
execution-batch ratio 0.991684: no demonstrated application speedup from byte
MUL, despite the focused kernel improvement. All six temporary restore disks
are removed. Separate task-scoped host/opcode profiling identifies ADC, SHRD CL,
bit tests, variable shifts and LAHF as substantial fallbacks; interpreter,
dispatch and polling account for about 28.5%, 23.8% and 16.0% of main-thread
samples. These differ from the earlier broad interaction profile. Evidence:
`docs/measurements/excel-byte-mul-comparison-2026-10-25.json` plus compressed
raw profiles. Further code lowering/block-boundary work and current-binary fresh
Windows installation remain outstanding; broad performance completion is unproven.

## Native SHRD and missing guest parents (25 October)

Register SHRD CL/immediate now runs natively with zero-count flag preservation,
full register-alias coverage and 16/32-bit behavior matching the interpreter.
Flag tests pass 17,014,646 checks (524,288 new SHRD cases), core passes 352,644,
and candidate Win3.11/Win95 application-persistence checks passed. Five paired
RAM-only kernels give median 3.85x throughput with identical guest results and
full native coverage. Evidence: `docs/measurements/native-shrd-2026-10-25.json`.

The attempted Excel baseline restore subsequently failed before boot: local
DOS, Win3.11, rtts and Office base/snapshot parents are now absent. Read-only
checks found no moved base under /Volumes/data/pcemu or live deleted-file handle.
The user has been asked for a moved location. External ISO/COW children and
recalculation snapshot remain, but cannot replace missing parent content.
Excel speedup and current runnability are not established. Continue RAM work or
recover complete guest chains; broad goal and fresh installer validation remain
active and incomplete.

## Original rtts image recovered independently (25 October)

Recovered the repository's 256 MiB hda.img from its documented download URL.
Its SHA-256 matches the Git LFS pointer (23158e95d754dd734f7d7dc2e40a4e53e2b31d3bb7e35db93bbb61dc69221161).
Imported to a 180,006,943-byte compressed disk and verified the complete logical
SHA-256 before removing the temporary raw download. Both disk and zero parent
live under /Volumes/data/pcemu/recovered; media/rtts-win95.img is only a symlink.
SMB reports the 256 MiB zero parent as allocated, so total storage is not just
the compressed image size. Headless Calculator/clipboard/Notepad exact file
persistence and one firmware restart back to Explorer pass; the successful
private child is removed. Evidence:
`docs/measurements/recovered-rtts-2026-10-25.json`.
Missing Office/OSR2/DOS/Win3.11 parents and fresh installer validation remain
unresolved; this recovery restores the original rtts requirement independently.

## DOS and Win3.11 restored on compact external chains (25 October)

Recovered the MS-DOS 6.22 with CD-support VHD archive, verified its published
SHA-512, and verified logical equality after offline QCOW2 conversion. The base
uses about 9 MiB for a roughly 2 GiB virtual disk. Our emulator prints MS-DOS
Version 6.22 and the ready marker. No raw multi-GiB staging copy was created.

Reworked prepare_win311.py to stage flat/floppy source files into a new
compressed DOS child and read every source/answer file back. All 460 source
files match. Original Setup /h using the checked-in answer file completes to
DOS; independent cold boot, Notepad save, DOS return and exact stopped-guest
file contents pass. Installed child is about 13 MiB on /Volumes/data. The local
DOS and Win3.11 media names are symlinks to complete external chains.
The rtts zero parent is now a 196,616-byte QCOW2 container; its complete logical
image hash remains unchanged. Evidence:
`docs/measurements/recovered-dos-win311-2026-10-25.json`.
Fresh Win95 Setup and recovery/replacement of the separate Office parent remain.

## SHRD flag-work experiment rejected (25 October)

Six balanced-order Excel pairs did not establish a gain from removing
unobserved SHRD flag calculations. Runtime code is restored to the verified
CL-shift baseline. Extended SHRD/precise-fault tests pass 91,745,234 checks,
and the core suite passes 352,644. All twelve application runs matched the
required result pixels. The complete performance goal remains active.
See `measurements/rejected-shrd-dead-flags-2026-10-25.json`.

## Near CALL fault ordering corrected (25 October)

Specification review for native indirect calls found a pre-existing missing
target-limit check. Direct and indirect near CALL now reject an out-of-limit
target before stack changes; the native direct path rechecks cached CS limits
at runtime. Stack/JIT checks (27,641), core checks (352,644), DOS and all three
Windows application validation paths pass. Native register-indirect CALL work
continues after this correctness prerequisite. See `near-call.md` and
`measurements/near-call-target-limit-2026-10-25.json`.


## Native register-indirect CALL (25 October)

FF /2 register targets now use native code with the existing guarded stack push.
The target is captured before SP/ESP changes, checked against the current CS
limit, and committed only after successful access. Memory operands still replay.
All 33,360 stack/JIT, 352,644 core, and 91,745,234 flag-liveness checks pass.
Application/performance validation is pending because the external /Volumes/data
volume disappeared before the matched Excel baseline loaded its snapshot. No
application timing result was produced. See `near-call.md` and
`measurements/native-register-call-2026-10-25.json`. The overall goal remains active.


## Disk-free register CALL measurement (25 October)

With /Volumes/data still unavailable, a RAM-only CALL/RET loop now supplies
matched-source evidence for the native register CALL path. Three alternating
pairs across 16/32-bit code and stacks, flat/paged RAM, check exact 50-million
instruction counts, destinations, checksums and restored stack pointers. The
per-case median speed ratios are 1.44–1.71x; whole-matrix median is 1.57x.
These are shared-host microbenchmark observations, not Windows/Excel results.
No guest disk was allocated. Source, commands, hashes and raw results are in
`measurements/native-register-call-ram-2026-10-25.json`. Windows integration
validation remains pending on the missing external volume.


## External media reconnected; native register CALL integration verified (25 October)

The SMB volume is back. Frozen native-register-CALL candidate boots DOS and
passes Win3.11 exact Notepad persistence, rtts Win95 Calculator/clipboard/Notepad
and restart, and the recovered OSR2 system's same application/restart checks.
All successful private disks are removed. This OSR2 check is an installed-system
regression check, not a fresh Setup run with the latest candidate.

Six Excel recalculation runs match expected pixels. Three matched pairs observe
median response/execution-wall ratios of 1.010/1.023; shared-host variability
prevents claiming a reliable application gain. Native coverage is 97.09% versus
97.69%; the diagnostic interpreter count is 32.6 million. Host wall-stack sampling
now attributes 28.5% to dispatch/cache checks, 17.9% native/helpers, 16.9% device
polling and 11.5% interpreter. The goal remains active; application speed and
fresh-installer validation for subsequent runtime changes remain separate gates.
See `measurements/excel-register-call-2026-10-25.json` and `near-call.md`.


## Inline cached instruction-fetch validation (25 October)

The dispatcher resolves current permitted paged TLB hits inline, retaining the
reference CPU path for misses and faults and all existing block identity guards.
A 192-case reference comparison adds 2,304 checks across permissions, privilege,
VM86/system access, A20, TLB misses, CR3/remapping and unusable CS. Core checks
(354,948), stack/JIT (33,360), DOS, Win3.11 persistence and both Win95 application/
restart/exact-file checks pass. Successful private disks are removed.

All twelve Excel tasks match expected pixels. Six pairs observe median response/
execution-wall speed ratios of 1.097/1.123, with large shared-host timing drift;
these are not general application performance guarantees. Native coverage is
unchanged. A diagnostic host profile shows dispatch/cache attribution at 21.4%
versus 28.5% previously, with fewer address/physical helper samples. The goal
remains active; the latest candidate has not rerun fresh Win95 Setup. See
`jit-fetch-validation.md` and `measurements/jit-fetch-tlb-2026-10-25.json`.


## Process CPU accounting separates scheduler wait from task CPU (25 October)

The snapshot task runner now reads macOS process user/system CPU counters around
the task-response interval, with Mach timebase conversion and explicit unavailable
status on unsupported/failed reads. A real sleeping/busy child validates units
and sleep exclusion; identity and counter-regression checks prevent false data.
Three accounting tests, syntax checks and six actual Excel restores pass.

Three matched pairs of pre/post fetch-TLB candidates observe lower process CPU
in all three pairs, with a median ratio of 1.045 (about 4.3% less CPU). Capture,
cache warmup and guest idle work remain in the interval, and the first baseline
has extra observed instructions; this is not isolated guest CPU time or a general
application claim. No emulator binary changed or large fixture was created.
The goal remains active. See `host-cpu-timing.md` and
`measurements/process-cpu-excel-2026-10-25.json`.


## Fresh OSR2 Setup with fetch-TLB candidate and first-boot automation (25 October)

The frozen fetch-TLB candidate now has fresh Setup evidence: all 56 source files
are staged/read back on a small compressed DOS child; file copying and two
firmware resets are observed. Network login, printer setup and the explicit
post-configuration restart are handled as known first-boot screens. The desktop
check retains both icon names and a clear-desktop pixel gate, adding a small
selected-label OCR fallback and full Recycle Bin crop. The observed Are/Ale
restart-question OCR variation is accepted with the actual restart-option text.

The original guest PID disappeared during the requested pause; cause is unknown.
After verifying it absent, a tiny child cold-boots the same installed disk and
runs Calculator/clipboard/Notepad. The restart check resumes after OCR correction;
exact stopped-guest readback matches 38 bytes. Both temporary install/resume
children are removed. The rtts applications/restart regression and syntax checks
pass. This is interrupted-run plus cold-boot evidence, not an uninterrupted
single-process install or a timing benchmark. The broader performance goal stays
active. See `fresh-win95-validation.md` and
`measurements/fresh-win95-setup-fetch-tlb-2026-10-25.json`.


## Empty-request PIC polling (25 October)

An inline zero-IRR path preserves cascade and compatibility mirrors, leaving
all pending requests on the original algorithm. Differential validation passes
2,172,864 checks; the frozen candidate passes 354,948 core checks, DOS, Win3.11
and both rtts/installed OSR2 application-save/restart/readback gates headlessly.
Successful private compressed disks are removed. Three alternating Excel pairs
observe 5.18% less process CPU at the median, with reductions ranging from
1.44% to 13.22%; cold-cache and host-load effects limit the exact claim.
This is a small validated improvement, not Rosetta parity. See
`pic-idle-polling.md` and `measurements/pic-idle-excel-2026-10-25.json`.
The broader performance goal remains active.


## Render complete VGA frames without per-pixel palette expansion (25 October)

The post-PIC task host profile points to capture/video/control (18.5%) as well
as dispatch (22.8%) and polling (16.6%). The frame renderer now snapshots the
DAC palette per frame and shares planar byte loads across eight pixels. GUI
and capture use it; packed non-8-bit formats keep the pixel reference path.
It matches 1,014,127 reference pixels, passes ASan/UBSan and 708 component
checks, and passes headless Win3.11 and both Win95 application/restart/readback
gates. Successful private disks are removed. A RAM-only rendering run records
4.43× planar, 4.28× indexed VGA and 8.00× VBE 8-bit speedups. Three whole-task
Excel CPU ratios improve but vary widely under host load; this establishes
cheaper presentation, not faster x86 execution or Rosetta parity. The goal
remains active. See `vga-frame-rendering.md` and its raw measurements.


## JIT cache-store pilot and proposed performance references (25 October)

A conditional fast-slot publication candidate retained all dispatch guards but
did not earn adoption: six of eight RAM CALL/RET case medians were below the
baseline/candidate ratio of one. The short shared-host pilot does not establish
a precise application effect. The JIT runtime is restored byte-identically,
with patch, commands, hashes and raw times retained in
`measurements/jit-cache-publish-rejected-2026-10-25.json`.

The user's performance-target question is documented in `performance-targets.md`:
proposed Pentium 133–200 experience and matched QEMU TCG system-emulation
comparisons, with no claimed historical CPU rating. The current eight-visit JIT
warmup is verified in main.cpp; higher thresholds need a workload experiment,
not analogy to JS. These proposed references do not replace the requested
compatibility or performance work. The broader goal stays active.


## Native memory-indirect near CALL (25 October)

The measured hot `FF /2` memory form now reads its pointer through the guarded
RAM helper, checks the target and uses the existing native stack-store path.
Old ESP addressing, pointer/return-slot aliases, CS-before-stack validation,
MMIO/fault replay and self-modifying stack writes retain restartable behavior.
8,677 new differential checks, 33,360 stack checks and 354,948 core checks pass.
Headless DOS, Win3.11 and both rtts/installed OSR2 application persistence and
restart gates pass; temporary disks are removed.

Three matched RAM-only pairs show 1.47–1.72× case-median CALL/RET kernel
speedups. Excel native coverage rises from about 97.69% to 98.41%. Six process
CPU pairs have four favorable and two unfavorable results, with wide shared-host
variation; the median ratio 1.068 is exploratory, not a stable general gain.
Fresh Setup was not repeated under this lowering and remains a final gate.
The full goal stays active. See `memory-indirect-call.md` and
`measurements/native-memory-call-2026-10-25.json`.


## Fresh installer gate closed for native memory CALL (25 October)

The frozen x86 candidate with `1c5a879` completes fresh OSR2 Setup and application
persistence without interrupting or replacing its guest process. Initial SMB
staging readback/close fails; after the tool exits, an independent read-only
reopen verifies all 56 sources before boot. Agent-driven wizard inputs and the
scripted finish chain observe firmware resets 0→1→2→3, Explorer, Calculator,
clipboard, Notepad save and a guest-requested restart. After quit, PID absence,
a normal log trailer and exact 38-byte read-only FAT content are verified. The
154.7 MB compressed test child is removed. No raw large fixture is created.

The detached launcher's exit code is unavailable and is not inferred. Timing
includes agent/wizard delays, so this is functional evidence only. The separate
2× useful-work stretch target is anchored in `performance-targets.md` but remains
unachieved. Concurrent ARM/HVF/frontend work is outside this frozen executable.
The broader performance goal stays active. See `fresh-win95-validation.md` and
`measurements/fresh-win95-memory-call-2026-10-25.json`.


## Current Office host and guest profiling (26 October)

Three uninstrumented host-stack runs each cover loaded Excel recalculation and
warm Word launch, with separate matched-source guest-counter runs. Expected
pixels pass throughout. Sample accounting excludes verified attachment waits,
keeps unresolved frames explicit, recognizes batched VGA presentation, and
retains per-run/source-site detail. Six analyzer tests and actual accounting
total checks pass; all raw traces/counters are zstd round-trip verified.

Excel's 30,726 task samples show dispatch 27.1%, generated code/helpers 23.4%,
polling 16.9%, interpreter 9.8%, capture/control 11.0%, compilation 0.8%. Word's
short warm launch has 2,470 samples: interpretation 28.9%, dispatch 22.9%,
compilation 9.8%. These are wall-stack shares with separate process CPU totals,
not exact per-function CPU durations or new speedup evidence. Generated ARM64
is not yet mapped back to guest instructions. Host/run spread and Word's short
shutdown margins are reported. About 3.87 guest instructions per Excel dispatch
support investigating dispatch/poll amortization before more isolated opcodes.

The new Word launch checkpoint retains only about 15 MiB compressed state plus
a 137 KiB child externally; no Office reinstall or large raw disk is made.
Per-run and probe overlays are removed. The 2× stretch target remains unproven.
See `office-current-profiling.md` and
`measurements/office-host-profile-2026-10-26.json`.


## Reject PS/2 predicate ordering shortcut (26 October)

The current host profile points to PS/2 due checking within frequent board polls.
Moving the no-motion/button-change test earlier preserves 480,000 frozen
predicate comparisons and 65,564 controller checks. Three-pair RAM gate loops
show an enabled-idle median ratio of 1.47, but a disabled-idle ratio of 0.83.
Three alternative condition orderings also fail to improve both cases. No
application gain is established, so mouse.hpp is restored byte-identically and
no redundant predicate-only test is retained in the production test suite.
The kernel, candidate patch, alternatives and all raw measurements remain in
`measurements/ps2-idle-ordering-rejected-2026-10-26.json`. This motivates testing
longer execution paths/event-driven polling instead of keeping a local ordering
change based on one favorable mode. The broader goal remains active.


## Conditional traces reduce loaded Excel dispatch cost (26 October)

The existing experimental trace mode is compared with FollowJumps using the same
frozen executable. Three alternating loaded Excel pairs all favor traces, with
median process CPU ratio 1.205 (about 17% less CPU), response ratio 1.214, and
verified output pixels. Separate counters show 251.24M versus 362.34M dispatches
for roughly 1.404B guest instructions: 30.7% fewer dispatches and 5.59 rather
than 3.88 guest instructions per dispatch. Translation attempts increase.

Three Word warm-launch pairs do not show a consistent gain (median CPU ratio
0.979), so no default policy is changed. Trace mode passes 17,861 jump checks
and headless DOS, Win3.11, rtts and installed OSR2 application persistence/restart
gates. rtts verification resumes in the same live guest after a real Calculator
OCR miss; exact 38-byte readback is verified. The title fallback reads actual
blue-band glyphs and retains literal expected tokens. All owned test children
and index sidecars are removed. Fresh Setup is not yet tested in trace mode.

Evidence supports investigating selective hot-block trace promotion, rather
than globally using large traces or retaining the rejected PS/2 ordering change.
The 2× target remains unproven and the broader goal stays active. See
`conditional-trace-evaluation.md` and
`measurements/conditional-traces-2026-10-26.json`.


## Adaptive trace prototype rejected after controls (26 October)

A selective 1024-native-entry conditional trace prototype passes 30,185
differential checks, the 354,948-check core suite and 17,861 jump checks. Final
Excel opt-in pairs favor it (median CPU ratio 1.134), while Word warm launch
does not (0.981). Disabled-feature Excel controls are unfavorable in all three
pairs. Other chats started ARM/HVF and x86 workloads during those controls;
the measurements do not isolate implementation overhead, but cannot establish
an acceptable legacy path. All proposed runtime, analyzer and test-target
changes are restored. The complete rejected patch and all final/intermediate
measurements remain in `adaptive-traces.md` and its measurement record. No
adaptive OS smoke or fresh Setup gate is claimed. Six production sample
analyzer tests pass. The twofold goal remains unachieved.


## Native FF /6 PUSH lowering validated (26 October)

The current Excel guest profile identifies 1.33M interpreted FF /6 operations.
Register aliases now reuse guarded PUSH, and memory forms read their source with
old SP/ESP through the RAM helper before the existing guarded stack write.
8,682 memory PUSH, 43,600 stack, 354,948 core and 8,677 memory CALL checks pass.
A RAM-only 50M-instruction PUSH/POP/checksum loop across eight code/stack/paging
cases has three-pair median ratios of 2.63–2.85; this is not an application gain.
Excel and Word render the expected results. Separate Excel counters show FF /6
interpreter attempts down to 60,228; no application timing gain is claimed.

The frozen matched-source executable passes DOS 6.22, Win3.11 exact save/readback,
rtts and installed OSR2 applications, restart and exact 38-byte file readback.
Temporary compressed smoke disks and orphan indexes are removed. Fresh Setup
is not repeated for this lowering and remains a completion gate. The twofold
useful-work target remains unproven. See `native-memory-push.md` and its complete
measurement record.


## Fresh OSR2 Setup passes with native PUSH (26 October)

The exact frozen native PUSH executable installs OSR2 from clean DOS with 56
verified source files and network-adapter detection enabled. The original guest
PID survives copy/restart, first-boot configuration, Calculator/clipboard/Notepad
and a further guest restart: resets 0→1→2→3. It then exits normally with actual
status 0. Independent read-only FAT inspection verifies all 38 saved bytes.
The private compressed child and orphan index are removed. Evidence and
screenshots are in `fresh-win95-validation.md` and
`measurements/fresh-win95-memory-push-2026-10-26.json`. This closes the latest
lowering's installer gate; no broader performance completion is claimed.


## Emitted ARM64 region attribution collected (26 October)

Three headless loaded Excel profiles use PID-tagged code maps and a diagnostic
128 MiB arena to avoid address reuse. Every output pixel gate passes; maps and
quit trailers show no recycles. Of 19,023 main-thread task samples, 4,596 map to
emitted regions. Block finishing alone contains 1,732 self samples (9.10% of
main-thread task samples), versus ALU regions' 129 (0.68%). Finishing includes
flags, branch decisions, register/IP commit and host return, so this does not
isolate flag cost. Roughly 24 MB of emitted code per process also motivates a
separate cache-capacity experiment. Diagnostic timings do not establish a
production speedup. Twelve analyzer tests pass; maps/stacks are compressed with
verified round trips and duplicate raw maps removed. Production runtime remains
unchanged. See `emitted-code-profiling.md` and its full measurement record.


## JIT capacity experiment keeps the default (26 October)

Uninstrumented arena-only 32 versus 16 MiB pairs confirm substantially fewer
compiled blocks in Excel and Word. All expected pixels pass. Excel's three CPU
ratios are 1.036/0.975/0.933; Word's are 1.026/1.041/1.027. Shared-host noise and
mixed Excel outcomes do not justify a default change. Production stays at 16
MiB, and no capacity option or other runtime change is retained. Source identity,
drivers and all six pairs are in `jit-cache-capacity.md` and its measurement.
The emitted finishing bucket remains the next profiling lead; no broad speedup
or goal completion is claimed.
