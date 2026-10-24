# Repeating a clean Windows 95 installation

The original Windows 95 CD files must already be extracted under
`media/win95-install/win95`. Preparation uses the supplied DOS VHD, rather than
`media/dos.raw`, because the latter working disk may already contain Windows.

```sh
python3 tools/prepare_win95.py --output build/win95-install/disk.pcd
build/pcemu --headless --machine win95 --ram-mb 64 --execute-vgabios \
  --disk build/win95-install/disk.pcd --steps 0 \
  --type 'cd win95
setup /is
' --control-file build/win95-install/control.txt \
  --frame build/win95-install/frame.ppm \
  --state-json build/win95-install/state.json
```

Same-page jump joining is enabled by default; add `--jit-basic-blocks` for a
comparison with the earlier mode. Setup is
interactive; `tools/guest_control.py` supplies guest keyboard input and captures
its framebuffer. `/is` skips ScanDisk, as documented in the CD's `SETUP.TXT`.
The installation must still complete file copying, its own firmware restart,
phase-two configuration and a subsequent desktop boot before claiming success.

The preparation command creates a sparse `.dos.raw` backing file and a
compressed `.pcd` child. Keep both while using the installation; moving or
removing the backing breaks its child. A fresh trial allocated 8.6 MiB for the
DOS backing and about 80 MiB for staged installer data, rather than copying a
2 GiB raw disk. The parent is never attached writable to the emulator. Existing
destinations/backings are rejected. Failed preparation is retained for diagnosis.
`--dos PATH` can select an existing clean raw/PCD parent instead of converting
the VHD. Parents containing a WINDOWS or WIN95 directory are rejected.

Every source filename is checked for 8.3 compatibility, and every staged file
is read back byte-for-byte. The adjacent `.pcd.json` records source hashes,
parent/source paths and compressed size. This verifies preparation only.

For batch input, pass `--batch-inf /local/path/MSBATCH.INF`; it is staged in
C:\WIN95. Start `setup /is msbatch.inf` inside that directory. The tool does not
generate or supply a product key. Microsoft's [Resource Kit sample](https://techshelps.github.io/MSDN/DNWIN95/HTML/S7376.HTM)
describes the batch sections. Microsoft [KB263281](https://ftp.zx.net.nz/pub/Patches/ftp.microsoft.com/MISC/KB/en-us/263/281.HTM)
notes that OEM Windows 95 can still require manual re-entry of the key even
when it appears in MSBATCH.INF. Keep any key-bearing batch input out of Git.


## Validate the retained installations

```sh
python3 tools/smoke_win95.py --applications
python3 tools/smoke_win95.py --applications --disk media/win95-fresh-installed.pcd
```

Both run headless and use a private compressed COW child. `--applications`
implies a guest-requested restart. After Explorer is unobscured, the test opens
Calculator, evaluates 2+2, copies the result to the guest clipboard, pastes it
into Notepad and appends a new random marker. It saves a unique 8.3 filename,
closes the applications, restarts Windows through its shutdown dialog, and
verifies exactly one firmware reset and another unobscured desktop. Only after
the emulator exits and flushes the disk does the parent read the FAT file through
`CowFile`, without expanding a raw disk. All bytes must match the Calculator
result plus marker; the digit 4 is never typed into Notepad by the harness.
Classic Calculator supplies a leading sign-space for positive 4, retained in
the exact 38-byte expected file. The bundled Notepad uses File -> Save.

Both retained images pass with the current emulator. A separate rtts run also
passes with `--sample`, and the ordinary cold-boot-only path still passes.
Successful private disks and enlarged application OCR scratch are removed;
PNG screenshots and manifests remain. [Committed verification records](measurements/win95-applications-2026-10-25.json)
retain source/emulator hashes, per-file content hashes and screenshots from
both images, plus a compressed raw host call tree for the profiling combination.
These checks establish application/clipboard/keyboard/file-persistence behavior
for the installed images, not a fresh installer run, high-resolution display
driver validation or application startup performance.
