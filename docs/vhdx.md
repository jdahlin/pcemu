# Native VHDX storage

The shared disk backend reads and writes parentless fixed and dynamic VHDX
containers directly. Recognition uses the file signature, not the extension.
There is no RAW conversion or mandatory COW layer.

```
build/pcemu-hvf --machine arm64-uefi --disk IMAGE.vhdx \
  --vars build/my-arm-vm/vars.bin --until-text 'Shell> '
```

This modifies IMAGE.vhdx. Supplying `--disk-overlay PRIVATE.pcd` explicitly
selects the existing compressed COW backend instead. Advisory inode locks reject
concurrent writers, including aliases of the same container.

Existing payload blocks are updated in place. Dynamic allocation appends blocks
as needed, leaves untouched space sparse, and avoids allocating zero-only writes
to holes. BAT changes use the native VHDX journal with ordered flushes; payload
data itself is not journaled. Header GUIDs, sequence numbers and CRC32C checksums
are maintained. No full disk copy or separate journal file is created.

Supported images use VHDX version 1, 512- or 4096-byte logical sectors and
1–256 MiB payload blocks. The backend validates both headers and region tables,
required metadata, BAT bounds, reserved bits and overlapping extents. The current
implementation bounds metadata to 16 MiB and the BAT to 256 MiB.

Recovery supports the single self-contained data-log entry produced by this
writer. Read-only opens reject dirty logs. Complex foreign log sequences,
wrapped logs and zero-descriptor logs are rejected before modification rather
than replayed partially. Differencing images, resize, discard and compaction
are not implemented. Successful fixture recovery does not establish exhaustive
power-loss correctness on every host filesystem.

## Validation

`make test-vhdx` runs 115 checks using original synthetic sparse fixtures:
fixed/dynamic layouts, both sector sizes, BAT chunk boundaries, direct writes,
allocation, reopen persistence, journal replay, locks and malformed containers.
The same suite passes AddressSanitizer and UndefinedBehaviorSanitizer.

`make smoke-vhdx` boots real EDK II through Hypervisor.framework, writes a FAT
file directly into VHDX, and reads it back in a fresh VM. It asserts that no COW
file was created. The verified fixture has 64 MiB logical capacity and occupies
6 MiB after the two boots. This validates firmware storage access. The original Microsoft VHDX also boots
Windows Validation OS to its command prompt through [NVMe](nvme.md).

Format reference: Microsoft's
[MS-VHDX specification](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-vhdx/83e061f8-f6e2-4de1-91bd-5d518a43d477).
