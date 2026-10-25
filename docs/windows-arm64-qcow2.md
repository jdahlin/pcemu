# Windows 11 ARM64 QCOW2 restoration

The user authorized recreating Windows with QCOW2 after the intentional RAW
cleanup on 2026-10-25. Guest disks stay on the Mac SSD; the source ISO remains
at `/Volumes/srv/apps/pc/os/Windows11_Client_arm64_en-us_26300_9457.iso`.
Remount the `srv` SMB share if the local ISO symlink is unavailable.

## Disk layout

The current backend reads standalone QCOW2 v2/v3 bases and writes through native
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

At the initial restoration checkpoint, WinPE has booted, network transport works,
the target was partitioned and the installation WIM download has started.
Installed Windows boot and the final flattened QCOW2 are not yet verified.
