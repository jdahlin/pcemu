# Full Windows 11 ARM64 bring-up

The next guest is the full Windows 11 ARM64 installation ISO, replacing
Validation OS as the primary Windows bring-up target. Keep the verified
Validation OS/NVMe boot as a small regression fixture.

The official English (United States) download selected on 2026-10-24 is
Windows 11 26H2 build 26300.9457:

- Filename: `Windows11_Client_arm64_en-us_26300_9457.iso`
- Bytes: 8,528,523,264.
- Microsoft-published SHA-256:
  `8992f2d2cfd2fa647e579e9eb31fc21f34da8333c29b6170fde17d2ed6623e9c`.
- Requested archive destination on caixapreta: `/srv/apps/pc/os`, mounted on
  this Mac at `/Volumes/srv/apps/pc/os`.

The download tool accepts a fresh official URL in an ignored JSON request file,
checks available space, uses ETag-protected exact HTTP ranges, resumes a
validated partial prefix, and checks the full ISO against the published hash.
Signed query parameters stay out of tracked documentation and manifests.

```sh
python3 tools/fetch_win11_arm64.py --request build/win11-download/request.json \
  --destination /Volumes/srv/apps/pc/os \
  --local-link media/windows11-arm64/Windows11_Client_arm64_en-us_26300_9457.iso \
  --remove-local-prefix
python3 tools/inspect_win11_iso.py \
  /Volumes/srv/apps/pc/os/Windows11_Client_arm64_en-us_26300_9457.iso
```

Inspection checks the real EFI boot catalog, ARM64 EFI loader and architecture /
version metadata in both boot.wim and install.wim without expanding the images.
Metadata inspection is not a successful Windows Setup boot or installation.
The ISO has a 630,231,583-byte boot.wim containing ARM64 Windows PE/Setup and
7,612,941,295-byte install.wim containing Home, Home Single Language and Pro.
The PE/Setup images report base build 26100; installed editions report 26300.

## First milestones

1. Boot Windows PE/Setup from the ISO through suitable boot-media presentation.
   The existing guest hardware is a single CPU, 2 GiB RAM and an NVMe disk;
   full Windows installation requirements, installer input, TPM/Secure Boot
   support and boot-media presentation still need implementation/validation.
2. Use the full Windows servicing tools to install the staged ARM64 NetKVM
   package into a private image. Preserve the original ISO.
3. Attach virtio-net alongside NVMe, connect the host network gateway, and
   prove DHCP, reachability, DNS and a TCP exchange from Windows.

## Hyper-V compatibility after boot and networking

The user requested relevant Hyper-V extensions once the preceding milestones
work. This is a future implementation stage, not a claim of current support.
Use the ARM64 TLFS ABI and the capabilities actually useful to this guest.

| Area | Implementation decision / verification |
|---|---|
| Discovery and ABI | ARM64 SMCCC discovery, hypercall conventions and synthetic feature/version registers. Preserve PSCI dispatch. Require probe coverage and Windows discovery evidence. |
| Guest/VP identity | Guest OS identity and VP identity/register operations needed by the advertised interface. Record accepted guest writes/queries. |
| Time and interrupts | Evaluate reference-time, synthetic timers and interrupt requirements against the existing framework GIC/timer backend. Advertise only complete semantics; test monotonic time, wakeups and delivered interrupts. |
| Scheduling and memory | Evaluate yield, VP scheduling and TLB-flush facilities as SMP/guest demand justifies them. Check real invocation counters and precise state effects. |
| Crash diagnostics | If exposed, capture guest crash notifications and parameters; test intentional synthetic notifications independently of a healthy boot. |
| VMBus and synthetic I/O | Treat VMBus, netvsc and storvsc as separate device protocols. Implement only if desired beyond working NVMe/VirtIO; discovery alone does not provide those devices. |

For every feature distinguish **implemented**, **advertised**, **invoked by
Windows**, and **effective**. Add per-operation counters/last errors and a
reproducible guest observation report. Unsupported calls must fail truthfully.
Compare enabled/disabled runs for correctness and any performance benefit.
Do not substitute a synthetic probe for proof that Windows uses a feature.

ARM64 uses synthetic register queries and HVC conventions, rather than x86
CPUID/MSR discovery. x86-only APIC, TSC, VMX/SVM and nested-virtualization flags
are not automatically relevant to this platform. A complete Hyper-V host or
nested Hyper-V is not implied by guest enlightenment support.

References: [Microsoft ARM64 ISO download](https://www.microsoft.com/en-us/software-download/windows11arm64),
[ARM64 discovery](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/feature-discovery),
[Hypercall ABI](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface),
[Virtual interrupts](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/virtual-interrupts).


## Media location policy

Only the Windows 11 installation ISO is being moved to caixapreta. Writable VM
disks (`.raw`, `.vhdx`, `.pcd`), snapshots and active guest state stay local.
No bulk media/disk migration has been applied. The archive target is exactly
`/Volumes/srv/apps/pc/os/Windows11_Client_arm64_en-us_26300_9457.iso`, corresponding
to `/srv/apps/pc/os` on caixapreta.

The transfer remains a `.iso.part` until its full Microsoft SHA-256 is verified.
After verification the downloader publishes a local ISO symlink under ignored
`media/windows11-arm64/` and removes only a matching task-owned earlier local
partial plus its metadata. The server copy remains authoritative; mount the
`srv` share before using that symlink. Transfer progress is recorded locally in
ignored `build/win11-download/transfer.log`, with the final result in the
server-side `download.json`. File presence with a `.part` suffix is not completion.

## Windows PE/Setup boot verified

The full ISO's ARM64 Windows PE/Setup now boots through the original NVMe
controller to the **Windows 11 Setup** graphical window. The captured page is
**Install driver to show hardware**. This verifies a live WinPE/Setup userspace,
not an installed Windows 11 desktop or a completed installation.

The initial direct ISO attachment exposed its El Torito EFI filesystem but
returned to the UEFI shell after the DVD boot timeout. For the verified boot,
`tools/prepare_win11_boot.py` stages four unmodified ISO payloads on a private
1 GiB sparse FAT16 boot disk:

- `/efi/boot/bootaa64.efi` → `EFI/BOOT/BOOTAA64.EFI` (EFI application).
- `/efi/microsoft/boot/bcd` → `EFI/MICROSOFT/BOOT/BCD`.
- `/boot/boot.sdi` → `BOOT/BOOT.SDI`.
- `/sources/boot.wim` → `SOURCES/BOOT.WIM` (630,231,583 bytes).

The ISO's root `bootmgr.efi` is a Windows boot application and cannot replace
the EFI application. The builder validates ARM64 and the EFI subsystem before
creating media, streams the WIM without expanding it, hashes every payload,
and emits the `MICROSOFT` VFAT long directory name with its short-name checksum.
The CLI publishes its derived boot disk only after staging completes and
refuses an existing output. Five synthetic tests cover file/BCD/WIM round trips,
long directory names, PE architecture/subsystem, truncated streams and overwrite
protection; no proprietary image is needed for those tests.

```sh
python3 tools/prepare_win11_boot.py
mkdir -p build/windows11-setup-efi
build/pcemu-hvf --machine arm64-uefi --disk-controller nvme \
  --disk build/windows11-media/setup-efi-boot.raw \
  --vars build/windows11-setup-efi/vars.bin --timeout-ms 60000 --cpu-state \
  --framebuffer-ppm build/windows11-setup-efi/console.ppm \
  > build/windows11-setup-efi/serial.log 2> build/windows11-setup-efi/host.log
python3 -m unittest discover -s tests -p test_win11_boot_media.py
```

The verified observation is `build/windows11-setup-efi/console.png`, accompanied
by `host.log`, `serial.log` and `verified-boot.json`. The measured run reported
5,050 NVMe commands, two controller initializations and no controller fatal
status. Unsupported feature-query errors are recorded separately; the process
exit code alone is not the boot success criterion.

The boot disk (`setup-efi-boot.raw`) and its private writable COW
(`build/windows11-setup-efi/vars.bin.pcd`) are **local on the Mac's internal SSD**.
Only the source ISO is on caixapreta. CPU execution uses Hypervisor.framework;
NVMe/PCI/platform devices are implemented by this project. Windows PE runs its
system from the WIM-backed RAM disk. No installed Windows system disk has been
created or installed in this step.

Only boot payloads were staged: the 7.6 GB `install.wim` remains in the original
ISO. The current disk is therefore not complete installation media, and Setup's
media/driver page is not evidence that installation-source access works. The
next storage milestone is to expose the full installation source and a separate
local destination disk to WinPE, followed by installer input and network driver
provisioning. Existing guest disks and the server ISO were not modified.

### WinPE VirtIO network verified

A private copy of boot WIM image 2 now launches a diagnostic CMD script through
`Windows/System32/winpeshl.ini`. The source ISO remains unchanged. Pass this
private WIM explicitly with `tools/prepare_win11_boot.py --wim PATH --output PATH`.
`tools/build_windows_guest.py` builds the original ARM64 `guest-http.exe` with
LLVM clang, llvm-dlltool and lld-link, without Windows SDK headers or a CRT.
It downloads a new file or posts a diagnostic log through WinHTTP.

Run `python3 tools/windows_media_server.py` on the host before the guest probe.
It listens only on 127.0.0.1:8080; user networking maps guest 10.0.2.2 to that
loopback address. `/probe` returns a fixed marker, `/install.wim` streams directly
from the server ISO, and `/report` accepts logs up to 8 MiB into the ignored build
folder. No local copy of the complete installation image is required on the host.
The helper refuses to overwrite downloads and verifies the declared byte count.

Observed evidence in `build/windows11-install/reports/report-1792875895811015000.log`:
NetKVM `drvload` returned 0, PCI VEN_1AF4/DEV_1041 was Started, DHCP assigned
10.0.2.15 with gateway 10.0.2.2 and DNS 10.0.2.3, and an HTTP GET returned the
23-byte probe marker. The log itself arrived through a successful HTTP POST.
NVMe also appeared Started using the inbox stornvme driver. This verifies TCP
traffic through the VirtIO NIC to the host; external DNS/Internet access and an
installed Windows system are still unverified.

The private diagnostic script loads `X:\PCEMU\NETKVM\netkvm.inf`, initializes
networking, captures `ipconfig /all`, `pnputil /enum-devices /class Net` and
`/class SCSIAdapter`, and runs `diskpart` with only `list disk`. It then runs
`HTTP.EXE get /probe X:\PCEMU\HTTP.TXT` and posts the captured log using
`HTTP.EXE post X:\PCEMU\PROBE.LOG`. Only the 1 GiB local boot disk was attached.
The next step is a dedicated local installation disk with ARM64 viostor; existing
VM disks must not be selected or modified by installation automation.

`python3 tools/fetch_virtio_arm64.py` stages viostor, vioscsi, viorng,
vioserial, vioinput and Balloon from the same pinned upstream 0.1.302 ISO.
It uses bounded HTTP ranges, checks the source length/ETag, rejects unexpected
sizes or non-ARM64 PE payloads, retains catalogs and the upstream license, and
records SHA256 for each file in ignored `media/virtio-arm64-0.1.302/download.json`.
These packages are staged for offline provisioning; staging is not proof that
Windows loaded a driver or that the corresponding virtual device exists.

The dedicated installation target is now a new sparse 64 GiB local
`media/windows11-arm64/system-base.raw` with writable `system.pcd` beside it.
The immutable zero-filled base consumes no 64 GiB allocation; the overlay grows
as Windows writes. This disk is distinct from the boot-media disk and all
existing guests. It has not yet been partitioned or installed. The Mac currently
has about 20 GiB free, so installation must use compact application and track
actual free space rather than relying on the target's logical capacity.

The first extra-disk probe is recorded in
`build/windows11-install/reports/report-1792876012739305000.log`.
Windows accepted both viostor and viorng packages (`drvload` return 0), but the
VirtIO block controller reported Problem Code 10 / status C0000001. DiskPart
therefore saw only the 1 GiB NVMe boot disk. No partitioning was attempted and
no installation was applied. Driver loading alone does not establish a working
VirtIO storage controller. Debugging can enable bounded host register-write
tracing with `PCEMU_TRACE_VIRTIO_BLK=1` (at most 4096 entries per process), including
queue/feature/status writes and device reset reasons. The block unit suite
passes 575 checks with tracing disabled.

The completed trace is `build/windows11-install/storage-trace/host.log`.
Windows reset the block device, negotiated features 0x100000246 and reached
status 0x0b (ACKNOWLEDGE, DRIVER, FEATURES_OK). It then selected queue 0 twice
and set FAILED (status 0x8b), without writing queue DMA addresses or enabling
the queue. This narrows the next investigation to queue allocation/Storport
initialization before disk I/O, rather than network or disk partitioning.
The 24 counted extra-disk requests came from firmware before this driver start.

### Driver-level storage diagnosis

`tools/build_windows_guest.py` also builds `trace-viostor.exe`, an original
ARM64 ETW controller. Run it with `start` before drvload and `stop` afterwards;
it captures the upstream WPP provider B17FA150-8C45-482E-9EB8-29611A862BF3 to
`X:\PCEMU\VIOSTOR.ETL` (4 MiB limit). The ETL can be posted using the HTTP helper.
`tools/inspect_viostor_trace.py --etl PATH --pdb MATCHING_PDB` correlates event
GUID/message IDs with WPP format annotations from the upstream PDB using LLVM
llvm-pdbutil and optional dissect.etl. It emits formats plus raw argument bytes,
without pretending to fully render arbitrary WPP argument types.

The 0.1.302 trace is `build/windows11-install/viostor-wpp.jsonl`, derived from
`reports/report-1792876500706644000.log` and the matching upstream viostor.pdb.
It proves the uncached allocation succeeded, zero MSI-X vectors were assigned,
and virtio_find_queues returned 0x80000011 (STATUS_DEVICE_BUSY). Upstream commit
[c0fe9d0451c8](https://github.com/virtio-win/kvm-guest-drivers-windows/commit/c0fe9d0451c8)
changed viostor's config-vector callback to return vector zero even with MSI-X
disabled. Our INTx-only device returns NO_VECTOR, causing queue setup to abort
before DMA setup. No synthetic MSI-X success is advertised to work around this.

For an INTx compatibility test, `tools/fetch_virtio_arm64.py --viostor-intx
--destination media/viostor-arm64-intx-0.1.285` stages only the earlier signed
upstream ARM64 viostor package (ISO size 789645312 bytes, published September
2025). NetKVM and the other packages remain at 0.1.302. Loading and actual I/O
must still be verified before using this package for the installation target.

The INTx test succeeded: report `reports/report-1792876680161916000.log`
shows viostor 100.101.104.28500 Started and DiskPart lists the 64 GiB target as
Disk 1 beside the 1 GiB NVMe boot disk. NetKVM and viorng remain 0.1.302 and both
are Started. This preserves the actual INTx interrupt path.

`tools/windows/install.cmd` automates Windows Pro image 3 deployment to that
specific two-disk VM. Before DiskPart, the original ARM64 `verify-install-disk.exe`
opens PhysicalDrive1 read-only, requires exactly 64 GiB and checks both 1 MiB
boundaries for emptiness. It permits only the four-byte MBR disk signature that
Windows automatically assigned to the otherwise empty target; all partition
entries, boot signatures and GPT data must still be zero. Twelve native WinAPI
fixture cases cover mismatched sizes, failed/truncated reads, nonempty boundaries
and the permitted signature. The guest guard passed in
`reports/report-1792877035768591000.log`. It will refuse an already-partitioned
target, so the deployment script is not an automatic reinstall/resume tool.

After the guard, the script creates GPT ESP/MSR/NTFS partitions, downloads the
installation WIM onto the local target and applies it with DISM `/Compact`,
`/CheckIntegrity` and `/Verify`. It injects the driver packages, provisions a
specialize-pass report script and runs bcdboot. The HTTP helper can now post a
fixed-size snapshot of a log while CMD still holds it open for writing; this was
verified in the guard probe. Phase reports identify download, image application,
driver injection and boot configuration separately. On any checked failure,
the script posts its log and stops. Installation completion and boot must still
be observed; preparing these scripts alone is not proof of either.

Deployment was launched with `build/windows11-install/install-boot.raw` as the
NVMe boot disk and the dedicated local target as `--extra-disk` with
`--extra-disk-overlay media/windows11-arm64/system.pcd`. The live installation
uses `--run-forever`; variables and logs are in `build/windows11-install/install`.
Its `run.json` records process/session handles for follow-up, but a live process
must always be rechecked before treating it as running.

Report `reports/report-1792877127371203000.log` proves the guard passed, DiskPart
successfully created/formatted GPT ESP/MSR/NTFS partitions, and the pagefile was
created. The host endpoint accepted `/install.wim` and the target overlay grew
past 200 MiB during the guest download. Image application, offline driver
injection and installed Windows boot are still pending at this checkpoint.
Superseded generated WinPE boot disks were removed to recover local space;
reports and media hashes remain, and the current installer boot disk is retained.

The guest tooling also builds `check-rng.exe` for the installed-system check.
It explicitly selects the INF-registered `QEMU VirtIO RNG Provider` through CNG,
requires two completed 64-byte requests and rejects identical buffers. It prints
only an outcome marker, never entropy bytes. This distinguishes a loaded viorng
PnP device from successful I/O through its own CNG provider. Native fixtures
cover provider-open, either request, close failures and unchanged output.
The probe has been built and its ARM64 imports checked; it has not yet run in
installed Windows and is not part of the already-running install WIM snapshot.

The CNG probe was also run in a separate WinPE instance without attaching the
installation target. Report `reports/report-1792877830130962000.log` shows
viorng Started, its provider registry entries present and viorngum.dll copied,
but BCryptOpenAlgorithmProvider returned STATUS_NOT_FOUND (0xC0000225). No
entropy request was completed in that probe. The checker now reports the failed
stage and NTSTATUS to distinguish provider discovery from device I/O failure.
This WinPE result does not establish installed Windows CNG behavior.

`tools/windows/finalize.cmd` is a non-formatting WinPE follow-up for the same
known two-disk installation VM. `finalize_mount.txt` mounts the target's GPT
partitions; the script requires both the installed kernel and this deployment's
PCEMU first-boot script before copying the RNG checker and updated diagnostic
script. It refreshes bcdboot and ensures the ARM64 fallback EFI loader exists.
It contains no clean, format, partition-creation or driver-replacement commands.
Use it only after the initial deployment reports completion and the installation
VM has stopped. This follow-up has been prepared, not yet run against the target.

`tools/verify_windows_boot.py REPORT` is the installed-guest report gate. It
requires the expected Windows build in both ver and registry output, C: as the
system/BCD partition with the EFI OS loader, one Started device for each of
VirtIO block/net/RNG, the DHCP lease and successful HTTP response, explicit CNG
provider I/O, and both report boundary markers. It rejects WinPE reports,
ambiguous devices, device problems, failed I/O and truncated reports. Original
fixtures cover those failures. This gate covers the guest report; host disk
placement, source-media identity and the actual VM launch configuration remain
separate completion-audit evidence. No installed guest has passed it yet.

Deployment completed successfully in
`reports/report-1792878866107369000.log`: DISM reached 100%, all six storage/system
VirtIO packages plus NetKVM were injected, and bcdboot reported success. The
installation VM was then stopped before any follow-up attached its target.
Its host summary counted 280878 target-disk requests and 5576000 received network
frames. The installed image is Windows 11 Pro 10.0.26300.9457; installed boot and
user logon remain separate, unproven milestones.

`tools/prepare_windows_account.py` generates a private answer file for a local
PCEmu administrator account, preserving the specialize diagnostics. Its random
password is retained only in ignored `media/windows11-arm64/local-account.json`
(mode 0600); the generated answer file is also mode 0600 and ignored. Neither
is printed or committed. The account has one automatic logon; FIRSTLOGON.CMD
then disables automatic logon and removes the Winlogon password value. Regional
settings match the English source media, online-account screens are hidden and
Express settings are declined. It uses the documented OOBE settings rather than
SkipMachineOOBE ([Microsoft reference](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/automate-oobe?view=windows-11)).

The finalization pass copies this private answer file, the RNG checker and the
updated scripts. A background first-logon script records the actual user and
requests a screenshot after Explorer starts, using inbox Windows PowerShell and
System.Drawing. The guest posts the screenshot and a separate result log to the
same loopback endpoint. The boot-report verifier additionally requires the local
account's first-logon marker. Account-generation tests cover escaping, preserved
specialize settings, one logon, private/exclusive writes and invalid account
names. Actual account provisioning, desktop rendering and CNG behavior still
require the first installed-system run; they are not implied by those tests.

## Installed Windows boot checkpoint (2026-10-25)

The installed system has now booted without installation media attached, using
the local SSD's `media/windows11-arm64/system-base.raw` and writable
`system.pcd` overlay. The specialize report
`build/windows11-install/reports/report-1792879433890354000.log` has SHA256
`11d6a3df07b48e6f827b2e8c312ecf93b2c23478da1a322bc170f08095cc7882`.
It records Windows 10.0.26300.9457, C: as both the system and BCD OS partition,
and the EFI Windows loader. VirtIO block, NetKVM and RNG PCI devices are Started.
The explicit QEMU VirtIO RNG CNG provider returned 128 bytes successfully;
DHCP and the HTTP probe also succeeded from installed Windows. The earlier
WinPE CNG failure therefore does not recur in the installed system.

This proves installed kernel boot and actual I/O through all three currently
exposed VirtIO models. The additional staged balloon, input, SCSI and serial
packages do not establish device use: their models are not exposed by this VM.
The first local-user logon and desktop capture are still pending. Running the
boot-report verifier on the report above rejects only the missing local-user
logon marker; the overall completion gate remains unsatisfied.

The running instance has one vCPU and 2 GiB guest RAM. It has requested two
resets and continues executing guest code and issuing disk requests. These
observations do not identify the current Windows setup stage or prove that
user provisioning will finish. Framebuffer files are captured at reset/exit,
so their black reset image is not a live desktop observation. Generated media
containing the private answer file, writable system overlay and variable store
are restricted to mode 0600; the private staging directory is mode 0700.
