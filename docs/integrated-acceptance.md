# Integrated x86 acceptance, 26 October 2026

One frozen current executable passes the headless functional gates below. This
checks the integrated build after the recent private optimization experiments;
none of the rejected native-loop or TLB-capacity policies is enabled.

| Guest | Verified behavior |
| --- | --- |
| MS-DOS 6.22 | Cold boot through VGA ROM, ready marker, VER output and real-mode state |
| DOS reset paths | Warm far jump, keyboard-controller reset and port 92 reset each reach a second DOS boot with exactly one reset |
| Windows 3.11 | Standard-mode boot, Notepad save, return to DOS and exact 25-byte FAT readback |
| Installed OSR2 | Cold Explorer boot, Calculator 2+2, clipboard transfer, Notepad save, guest restart and exact 38-byte stopped-guest readback |
| rtts/win95 | Same application/restart/persistence checks as installed OSR2 |
| Fresh OSR2 Setup | Clean-DOS installation, first-boot configuration, Calculator/Notepad and application restart in the original process; three resets, actual exit 0 and exact 38-byte readback |

The fresh installer stages and verifies 56 existing source files into an
81,999,324-byte compressed child on the external volume. Its final size is
154,532,550 bytes; it and its index are removed after successful stopped-guest
readback. All other successful test children are also removed. No large raw
image or Office fixture is created. The DOS reset tool now uses compressed
children directly, including when its source path names a compressed parent.

The full fresh install remains unattended after launch: the driver recognizes
known wizard screens, uses the previously authorized OEM fields from private
local input, handles configuration/restarts, stops the original process and
checks its actual exit status before inspecting the disk. Product-key values
and control files are not included in the committed evidence.

[Manifest, exact hashes, commands, test results, driver source and compressed logs](measurements/integrated-acceptance-2026-10-26.json).
Representative images include the [Setup finish screen](measurements/integrated-acceptance-2026-10-26.setup-finish.png),
[fresh installed desktop after restart](measurements/integrated-acceptance-2026-10-26.setup-desktop.png)
and [rtts Notepad result](measurements/integrated-acceptance-2026-10-26.rtts-notepad.png).
Compressed raw logs have verified byte-for-byte zstd round trips.

This is the default CMake Debug `-O1` build, not an optimization benchmark.
Existing unrelated Makefile/documentation/frontend edits were present when the
executable was frozen; source and executable hashes identify the tested state.
This does not claim a pristine commit build, complete x86/device coverage,
networking validation or another twofold useful-work speedup. Those performance
and coverage questions remain open despite the completed functional gates.
