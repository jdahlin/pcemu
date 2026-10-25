# Windows 11 ARM64 QCOW2 restoration

The user authorized recreating Windows with QCOW2 after the intentional RAW
cleanup on 2026-10-25. Guest disks stay on the Mac SSD; the source ISO remains
at `/Volumes/srv/apps/pc/os/Windows11_Client_arm64_en-us_26300_9457.iso`.
Remount the `srv` SMB share if the local ISO symlink is unavailable.

## Deployment disk layout (temporary; removed after verification)

The backend reads standalone QCOW2 v2/v3 bases and writes through native
compressed `.pcd` overlays. It does not write directly into QCOW2 files.

- `media/windows11-arm64/system-empty.qcow2`: empty 64 GiB logical base.
- `media/windows11-arm64/system.pcd`: writable installation overlay.
- `build/windows11-qcow2/installer.qcow2`: WinPE bootstrap, NVMe device 0.
- `build/windows11-qcow2/installer.pcd`: private bootstrap writes.
- `build/windows11-qcow2/vars.bin`: fresh firmware variables.

The system target is VirtIO block device 1 during deployment. The existing
read-only installation guard verifies its 64 GiB size and unpartitioned
boundaries before DiskPart. The installer downloads the WIM over the user
network, applies Windows Pro index 3 with CompactOS, injects ARM64 VirtIO
drivers and runs the existing finalization script for local-account setup and
the fallback EFI loader. Credentials remain in ignored private files.

The VM uses 8 GiB RAM and two CPUs. Deployment starts with the basic Hyper-V
profile. VMBus experiments follow only after installed Windows boot is verified.

## Evidence and finalization

`build/windows11-qcow2/launch.json` records the source commit, launch handles and
disk paths. Recheck the PID and reports; their existence alone does not prove
completion. Serial, host, HTTP and phase logs are in the same directory.
The isolated executable passed 281 native checks before deployment.

After `PCEMU_FINALIZE_OK` and VM shutdown, flatten the stopped system overlay:

```sh
python3 tools/convert_qcow2.py media/windows11-arm64/system.pcd \
  media/windows11-arm64/system.qcow2 --size 68719476736
```

The converter performs `qemu-img check` and a complete logical byte comparison
before publishing the QCOW2 output. Keep the source overlay and its empty parent
until conversion succeeds. Create a fresh `.pcd` overlay on the published base
for subsequent guest writes. No full-size RAW intermediate is needed.

Boot the system disk without installation media, using the VirtIO PCI controller,
and verify the completed first-logon report with `tools/verify_windows_boot.py`.
That check requires installed Windows identity, local-account logon, active
VirtIO devices, a DHCP lease, HTTP traffic and successful VirtIO RNG I/O.

## Verified result

Restoration completed on 2026-10-25. The deployed Windows 11 Pro build is
26300.9457. `qemu-img check` found no errors and the full logical comparison
reported identical images. The standalone base occupies 18,444,255,232 bytes
(about 17.2 GiB), with a 64 GiB logical capacity.

Active files are `media/windows11-arm64/system.qcow2` and its writable
`media/windows11-arm64/system-live.pcd` overlay. Keep both. Firmware variables
are `build/windows11-qcow2/installed-vars.bin`. The temporary installation COW,
empty base and both bootstrap disks were removed after verification; their
paths and sizes remain in `build/windows11-qcow2/cleanup.json`. No persistent
RAW disk was created. A small FAT bootstrap staging file was converted and
removed before launch. Approximately 38 GiB remained free after cleanup.

`build/windows11-qcow2/boot-verification.json` passed all checks: installed
Windows identity, local PCEmu account logon, started VirtIO storage/network/RNG,
DHCP address 10.0.2.15, successful HTTP request and explicit VirtIO RNG I/O.
The complete report is `reports/report-1792939157448507000.log` under that
directory, SHA256
`0f9051b057dba443dea7b8188254093ec242145431f4d756bfd7c883d8a92d60`.
`desktop.png` is the guest-uploaded desktop capture, visually checked.

The executable is isolated from concurrent GUI work, built from committed
source d69f09a. It passed 281 native checks; QCOW2 model and QEMU interoperability
tests also passed. The initial continuation wrapper used unavailable WinPE
`findstr`, so finalization was completed using a separate non-formatting WinPE
boot. This did not repeat image application.

To start this installed disk after stopping any existing instance:

```sh
build/windows11-qcow2/source/build/pcemu-hvf \
  --machine arm64-uefi --ram-mb 8192 --cpus 2 --hyperv basic \
  --network user --disk-controller virtio --virtio-transport pci \
  --disk media/windows11-arm64/system.qcow2 \
  --disk-overlay media/windows11-arm64/system-live.pcd \
  --vars build/windows11-qcow2/installed-vars.bin --run-forever \
  --framebuffer-ppm build/windows11-qcow2/installed.ppm \
  --hyperv-stats build/windows11-qcow2/installed-hyperv.json
```

The verification HTTP service is `python3 tools/windows_media_server.py
--reports build/windows11-qcow2/reports`. It is needed for the diagnostic HTTP
probe, not for ordinary installed Windows boot. This restoration uses the
basic Hyper-V profile; it does not establish VMBus channel/service support.
