# Local storage and cleanup

The October 24 cleanup reduced the checkout's reported allocation from about
15 GiB to 3.4 GiB. Filesystem free space increased by 4.65 GiB; APFS clones share
physical extents, so summing per-file allocations overstates physical storage.
The cleanup removed 148 scratch/duplicate files and losslessly compressed 164
older diagnostics (about 1.67 GiB reduced to 33 MiB).

Preserved:

- Original downloaded installation archives and `media/rtts-win95.img`.
- Current `media/dos.raw`, `media/win311.raw`, `media/win95.raw` boot disks.
- All saved checkpoint directories and every compressed-disk ancestor, including
  `media/office97-base/disk.raw` and the absolute parent paths within snapshots.
- Saved Word/Excel guest files, drivers, installer checkpoints and startup tasks.
- Small state JSON/logs, screenshots and profiling results. Large older logs,
  RAM dumps without state metadata and PPM captures are retained as `.zst` files.

Superseded `rtts-*.raw`, historical Windows installation/debug branches, completed
smoke-test disks and old RAM dumps with retained CPU-state JSON were removed.
`media/office97-win95.pcd` was SHA-256 identical to the retained Office desktop
disk. The extracted Windows 95 ISO was SHA-256 identical to its member in the
retained original `.7z` archive. These duplicate copies were removed.
Historical documentation may name these retired test disks; they are not active
boot paths. New installation disks can be recreated from the original media.

## Repeat safely

```sh
# Report only; does not delete or compress files.
python3 tools/clean_workspace.py
# Also consider named old debug media and verified duplicate media.
python3 tools/clean_workspace.py --obsolete-media
# Apply after shutting down emulator instances.
python3 tools/clean_workspace.py --obsolete-media --apply
```

The script protects tracked files, boot disks, snapshots, symlinks/targets and
all discovered COW ancestors. Broken parent chains stop cleanup. Applying refuses
while a `pcemu` executable or `disk-tool` is running. Compression is checked before
its original is removed. No original archive is deleted. Review the report in
`build/storage-cleanup/plan.json`; the applied audit is `applied.json`.

Recover a compressed diagnostic with `zstd -d PATH.zst`. This temporarily
requires space for its uncompressed contents.

## Avoid repeated growth

The Win3.11 smoke test creates a compressed COW overlay and reads the saved
FAT file directly through the compressed disk reader, without a raw staging
image. Its result manifest records the emulator hash and overlay size. A
headless standard-mode boot, Notepad save and return to DOS on 2026-10-25
passed with an 8 KiB allocated overlay, which was removed after verification.

The Win3.11 and DOS reset smoke scripts remove their private disks after
successful verification and write small state JSON instead of full RAM dumps.
They retain failing disks for investigation. Pass `--keep-artifacts` explicitly
when full successful disks/RAM dumps are needed:

```sh
python3 tools/smoke_win311.py --keep-artifacts
python3 tools/smoke_reset.py --keep-artifacts
```

Cleanup was followed by restoring all eight saved checkpoints, checking every COW
parent, saving exact text through Win3.11 Notepad and rebooting DOS through each
of its three reset paths. Evidence is in `build/storage-cleanup/`,
`build/win311-smoke-cuhbrg9h/` and `build/reset-smoke-n_q3y6z0/`.

Safety checks: `python3 -m unittest discover -s tests -p test_cleanup.py`.

## Windows 11 focused cleanup and legacy disk compaction

A later cleanup removed Debian VM data and unrelated build artifacts. The Office
checkpoint state was removed; `run-office97.sh` requires restoring or recreating
an Office checkpoint before it can run. The remaining Win95 backing disks and
Office base were retained. Restore Office checkpoints separately when needed.

The following local images now use PCEmu's compressed COW format despite retaining
their existing `.raw`/`.img` names. Format detection uses the file header:

- `media/win95.raw`
- `media/rtts-win95.img`
- `media/office97-base/disk.raw`
- `build/snapshots/win95-desktop/disk.raw`

Each image uses an immutable sparse zero parent in `media/compact-bases/`, named
`zero-268435456.raw` or `zero-2147483648.raw` for its logical byte size. Keep those
parents with the images: the COW headers contain absolute parent paths. Existing
child overlays retain their original backing paths and guest-visible contents.
These images can be used directly by PCEmu; external raw-disk tools must not treat
them as flat raw images.

Conversion used `disk-tool import-diff` against the zero parents. Every guest byte
was read back through `disk-tool serve` and its SHA-256 compared with the original
raw image, including verification after replacement. The images retain their
original permissions. Reported allocation fell by approximately 280 MiB; APFS
sharing means actual free-space savings can differ. The local audit is
`build/storage-cleanup/legacy-compaction.json`. Windows 11 disks were not converted.


## Legacy CHS geometry in compressed imports

An imported image over a sparse zero parent must derive BIOS disk geometry
from its effective partition boot sector, rather than the zero parent's default
16 heads/63 sectors. Byte-for-byte conversion alone does not preserve that
host-visible property: the compacted OSR2 backing specifies 128 heads, and
inheriting 16 heads caused its loader to stop with a disk I/O error.

Compressed disks now inspect their effective MBR and first partition BPB after
index construction, using the same heads/sectors bounds as raw-image detection.
Nested child changes take precedence over parent geometry. Invalid geometry
retains the parent's values; floppy detection remains based on logical size.
No on-disk format or guest data changes are required.

Six regression checks cover zero-parent imports, read-only reopen, nested BPB
overrides, floppy behavior and preservation of the parent. All 2,896 snapshot/COW
checks pass. The affected installed OSR2 image subsequently cold-boots and
restarts to Explorer; rtts Win95 also passes both operations. Local evidence is
`build/win95-smoke-3do0uj0t/result.json` and
`build/win95-smoke-09aqnuex/result.json`. The earlier failed private run is
retained under `build/win95-smoke-drgc45ac/`.

## Removal of RAW disks and dependent installations (2026-10-25)

At the user's explicit request, all remaining `.raw` files in this checkout
were deleted together with every discovered COW descendant. This intentionally
removed the installed Windows 11 ARM64 system and Hyper-V test branches, legacy
DOS/Windows boot disks and the Win95 desktop/calculator snapshots. The running
Windows 11 test instance was stopped before deletion. Earlier boot commands and
snapshot references in this documentation are historical and require recreated
media before use.

The interrupted QCOW2 conversion was discarded, not retained as a replacement
installation. Its incomplete output freed about 17 GiB. Subsequent RAW/dependent
cleanup removed 51 disk files plus affected snapshot directories and freed
another 36.57 GiB, leaving 59.37 GiB free at completion. No `.raw` files remained.
APFS sharing means summed file allocations greatly exceeded reclaimed space.
Original installer archives, the Windows ISO link, source code and diagnostic
reports were retained. The ignored deletion audit is
`build/storage-cleanup/remove-all-raw-applied.json`.

## External rtts recovery (25 October)

The original rtts image is now `/Volumes/data/pcemu/recovered/rtts-win95.pcd`.
Its parent is `/Volumes/data/pcemu/recovered/zero-268435456.raw`; retain both.
The workspace's media/rtts-win95.img is a symlink, so deleting the local link
does not remove the external image. All logical bytes were hash-verified against
the repository's Git LFS SHA-256; the temporary raw download was removed.
The compressed disk is 180,006,943 bytes. SMB reports the 256 MiB zero parent
as allocated; storage accounting must include it rather than assuming sparse
allocation. Missing Office parent data cannot be reconstructed from COW children
alone; the Office ISO and dependent recalculation snapshot are not substitutes.
