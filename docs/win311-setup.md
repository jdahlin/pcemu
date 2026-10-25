# Windows 3.11 preparation and unattended installation

The recovered DOS parent is `/Volumes/data/pcemu/recovered/msdos622/dos.pcd`;
its immutable read-only QCOW2 base is about 9 MiB for a roughly 2 GiB logical
disk. The downloaded WinWorld DOS archive SHA-512 and VHD/QCOW2 logical comparison
were verified. Original DOS 6.22 boots to the checked ready marker in our emulator.

Stage the existing flat installer files into a new compressed child:

```sh
python3 tools/prepare_win311.py \
  --dos /Volumes/data/pcemu/recovered/msdos622/dos.pcd \
  --source media/install-files --answer-file tools/windows311.shh \
  --output /Volumes/data/pcemu/recovered/new-win311.pcd
```

The tool validates filenames, rejects existing output and installed/installer
parents, and verifies every staged source file and optional answer file through
FAT readback. No full raw staging copy is created. Successful preparation writes
an adjacent manifest; preparation alone does not establish installation success.

Boot this private disk headlessly and run from DOS:

```text
CD WINSETUP
SETUP /H:C:\PCEMU\INSTALL.SHH
```

The supplied answer file selects VGA, PS/2 mouse, no network, installs default
components including Notepad, and exits to DOS after Setup. The `/h:` system
settings mechanism is documented in Microsoft's [Windows 3.1 Resource Kit,
pages 75–82](https://www.bitsavers.org/pdf/microsoft/resource_kits/0030-31645_Windows_3.1_Resource_Kit_199202.pdf).
The installer media's SETUP.SHH template and SETUP.INF supply the device names.

The recovered installed disk is `/Volumes/data/pcemu/recovered/win311-final.pcd`;
`media/win311.raw` is a symlink to it. It completed unattended Setup, then passed
an independent cold-boot/Notepad save/DOS return/exact file readback check:

```sh
python3 tools/smoke_win311.py
```

Evidence: `measurements/recovered-dos-win311-2026-10-25.json`. Keep the full
external parent chain. QEMU image utilities were used only for offline VHD to
QCOW2 conversion with SMB file locking disabled; our original disk backend and
CPU/device implementations perform guest execution.
