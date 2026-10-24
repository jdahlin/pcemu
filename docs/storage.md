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

The Win3.11 and DOS reset smoke scripts now remove their private disks after
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
