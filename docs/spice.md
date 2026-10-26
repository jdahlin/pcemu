# SPICE display, input and guest integration

PCEmu uses upstream `spice-server` for the real SPICE protocol. The ARM64 runner
exports its software framebuffer through the server's QXL display interface;
the guest still uses VirtIO GPU, rather than an emulated QXL PCI adapter. USB
HID keyboard and absolute tablet input work before a guest agent is installed.

## Run

```sh
brew install spice-server spice-protocol spice-gtk
cmake -S . -B build -G Ninja
./run11.sh --spice
# Connect a SPICE client to spice://127.0.0.1:5930
```

`PCEMU_SPICE_PORT=5931 ./run11.sh --spice` chooses another port. Direct runner
options are `--spice-port PORT` and optional `--spice-password-file FILE`.
The password file contains one line, 1..255 bytes; it is never printed. The
server binds only IPv4 loopback. Without a password file it uses no ticket
authentication. The connection uses local TCP, without TLS. `--gui` and
`--spice-port` select different frontends and cannot be combined.

The normal `./run11.sh` continues to use SDL. Both frontends use the absolute
USB tablet and a native host cursor with the installed GPU profile. Windows
shutdown remains the normal way to stop the VM; SIGUSR2 requests it through
the existing Hyper-V service.

CMake detects SPICE optionally. A build without the library retains SDL and
reports an explicit error if SPICE is requested. The bundled local app uses
the installed Homebrew libraries; it is not a self-contained distribution.

## Display and cursor

Display updates own their pixels until the server releases each command. The
worker never reads live guest DMA memory. Full-frame copies, the latest pending
frame and retained resources are bounded; dimensions are limited to 4096×4096.
The worker is flushed before snapshots accumulate, and primary surfaces are
recreated on resolution changes. Reconnecting clients receive the current
surface. This is software 2D display; no GL streaming or 3D acceleration is
advertised.

The signed viogpudo package defaults its supported `HWCursor` setting to zero.
`tools/windows/install_spice_agent.ps1` enables it on the matching VirtIO GPU
adapter, without changing the INF, catalog or driver binary. Restart Windows
after applying the setting. GPU cursor images, alpha, hotspot and visibility
are then separate from display pixels. SDL creates a native color cursor;
SPICE sends it on its cursor channel, so the client moves it locally. Frontend
frames omit the guest cursor to avoid a duplicate. Diagnostic framebuffer
captures can still include the cursor. Cursor visibility follows the guest;
it is not replaced with a permanently visible arrow.

SPICE Set-1 keyboard fragments are translated into USB usages, including
extended modifiers, Windows keys, Print Screen and Pause. The absolute tablet
maps client coordinates to the full guest image; button changes and wheel
motion retain position. Main-channel disconnect releases guest keys and
buttons without powering off Windows.

## Guest agent and clipboard

SPICE adds `com.redhat.spice.0` as serial port one alongside
`org.qemu.guest_agent.0` on port zero. Both use the signed vioserial driver.
Buffers, open state and backpressure remain independent. The server's vdagent
character interface forwards the real agent stream, including reconnect and
flow control. Clipboard messages are handled by upstream SPICE and the native
ARM64 Windows agent. Clipboard sharing is enabled; file transfer is disabled.
Audio, USB redirection and WebDAV interfaces are not exposed by this backend.

The stable VirtIO ISO lacks the ARM64 user agent. UTM's guest-tools release
contains native ARM64 `vdservice.exe` and `vdagent.exe`. Fetch only those files:

```sh
python3 tools/fetch_spice_agent_arm64.py
```

This verifies the pinned release installer SHA-256 and each extracted binary,
checks ARM64 PE headers, and extracts with `7zz`. It does not install UTM's
other drivers. Transfer the `ARM64` directory into the guest, then run the
following in native elevated PowerShell (already completed on the local image):

```powershell
& C:\PCEMU\install_spice_agent.ps1 -Source C:\PCEMU\SPICE-ARM64
```

The script rechecks hashes, installs the service under Program Files, enables
the supported GPU hardware-cursor setting and starts the service. The guest
agent package is from UTM; this does not imply that it is a Microsoft-signed
kernel driver. The existing signed VirtIO kernel packages remain in use.

## Verification

```sh
make test-spice
make test-virtio-serial test-virtio-gpu
./tools/build.sh usb-xhci-tests frontend-tests
./build/usb-xhci-tests
./build/frontend-tests
python3 -m unittest discover -s tests -p test_hvf_cli.py
```

The SPICE tests connect a real `spice-client-glib` client over loopback, without
a guest or GUI. They verify pixels after sustained pre-connection updates,
resolution changes, native cursor alpha/hotspot/hide, ordinary and extended
keys, Windows keys, absolute pointer movement, clicks, disconnect release,
reconnect, and accepted/rejected ticket passwords. AddressSanitizer and
UndefinedBehaviorSanitizer cover the host backend. GStreamer plugin discovery
is disabled for these raw-image tests; no video decoder is required. The
external client's coroutine stack can produce ASan's documented stack-switch
warning; the checks still execute.

`build/spice-probe PORT ACTION_FILE` is a headless real-client diagnostic tool.
Actions include `wait MS`, `key HEX`, `chord HEX...`, `text TEXT`,
`type-file PRIVATE_FILE`, `move X Y`, `click`, `capture OUTPUT.ppm`,
`clipboard-put TEXT` and `clipboard-expect TEXT`.
`tools/windows/check_spice.ps1` is the installed-guest clipboard probe, using
fixed test markers and posting its report with the existing HTTP helper.
Typing files are not printed.
It does not bypass the Windows logon screen or UAC.

The installed Windows run on 2026-10-26 verified a normal password logon,
1280×720 desktop, 64×64 cursor shapes on the separate cursor channel, zero GPU
command errors, an absolute double-click opening the requested icon, and exact
28-byte clipboard transfers in both directions. The ARM64 agent and service
were running. Windows then powered off through its guest service. The promoted
`media/windows11-arm64/system-spice-live.qcow2` was safely rebased onto the
immutable system base, checked, and compared byte-for-byte with the stopped
validated chain. Older live disks remain for recovery.
[Retained evidence](measurements/windows-spice-2026-10-26.json).

References: [SPICE project](https://www.spice-space.org/),
[server API and tests](https://gitlab.freedesktop.org/spice/spice),
[UTM ARM64 guest tools](https://docs.getutm.app/guest-support/windows/),
[upstream GPU driver settings](https://github.com/virtio-win/kvm-guest-drivers-windows/blob/master/viogpu/viogpudo/viogpudo.cpp).
