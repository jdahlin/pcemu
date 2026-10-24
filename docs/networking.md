# NE2000 networking

The original ISA NE2000 model lives in `src/ne2000.cpp`, at I/O **0300–031F,
IRQ 10**, with 16 KiB packet RAM at card addresses 4000–7FFF. It implements the
DP8390 register pages, station PROM, remote DMA, transmit/receive rings, multicast
hash filtering, loopback, overflow and interrupt status/masking. No emulator core
or external network-stack library is used.

`src/network.cpp` is an original unprivileged IPv4 socket gateway. It supplies
ARP, DHCP, DNS forwarding, UDP and TCP forwarding through ordinary nonblocking
macOS sockets. TCP uses a bounded receive queue and one outstanding segment per
connection, cumulative acknowledgements and retransmission. Network servicing is
outside the instruction/device hot loop, including when the CPU is halted.

| Setting | Value |
|---|---|
| Guest address, via DHCP | 10.0.2.15 |
| Subnet mask | 255.255.255.0 |
| Default gateway | 10.0.2.2 |
| DNS | 10.0.2.3, forwarded to the first IPv4 resolver in `/etc/resolv.conf` (fallback 1.1.1.1) |
| Built-in HTTP test | http://10.0.2.2/ |
| Host services | 10.0.2.2 maps to host loopback; TCP port 80 is the built-in test |

This is a single-guest socket proxy, without LAN bridging, inbound port forwarding,
IPv6, IP fragment reassembly or external ICMP forwarding. The gateway itself
answers ping. It does not terminate TLS: Netscape 3's old TLS support limits access
to present-day HTTPS sites. Plain HTTP remains useful for the browser test.

## Windows 95 installation

Run `./run-office97.sh`; it prefers `build/snapshots/office97-networked`.
The checkpoint contains Windows 95 OSR2, Office 97, the display/mouse drivers,
NE2000/TCP-IP and Netscape 3.04. Open Netscape's desktop icon; its home page is
http://10.0.2.2/ . Java and JavaScript are disabled for predictable basic HTML tests.
`./run-office97.sh --integrated` selects the earlier display/input checkpoint.

Cold boot was verified with MAC 02-50-43-45-4D-01, DHCP address 10.0.2.15,
DNS 10.0.2.3, and Netscape rendering the built-in test and external example.com.
`build/win95-network/` contains screenshots, PCAP evidence and test logs.
The final disk delta is flattened directly against `office97-integrated/disk.raw`;
all 2 GiB logical bytes were compared with zero differences before scratch cleanup.
For a fresh installation, use the original Windows 95 files already in `C:\WIN95`:

1. Run `control netcpl.cpl`, Add, Adapter, Novell/Anthem, **NE2000 Compatible**.
2. Properties, Resources, Basic Configuration 0: **IRQ 10**, **300 - 31F**.
   Windows 95 edit controls do not support Ctrl+A; use Home, Shift+End before
   replacing an existing value. Preserve the complete I/O range text.
3. Add Protocol, Microsoft, TCP/IP. Use automatic IP configuration. Remove
   IPX/SPX and NetBEUI if they were added automatically. Windows Logon suffices.
4. Accept and restart. `winipcfg` should show 10.0.2.15 and gateway 10.0.2.2.
5. Run the Netscape installer and open http://10.0.2.2/ .

To stage Netscape on a new compressed disk branch:

```sh
python3 tools/stage_network.py build/snapshots/office97-integrated/disk.raw build/network.pcd
./build/pcemu --disk build/network.pcd --ram-mb 64 --execute-vgabios
```

Cold boot the staged disk; do not pair modified disk contents with the parent's
saved RAM. The stager verifies the complete guest installer after writing it.
Netscape Navigator 3.04 is downloaded from the [Devili historical browser archive](https://www.devili.iki.fi/pub/browsers/Netscape/win32/),
SHA-256 `593bde09d6f59e8668a19852388b007186edecffd04b8a356b8776a3be6fc3ef`.
The downloaded installer is approximately 5.5 MiB and the staged disk is a zstd
compressed delta; no extra 2 GiB raw disk is created.

## Diagnostics and snapshots

Networking is enabled by default. `--no-network` disconnects the socket gateway
while retaining the emulated card. Set `PCEMU_NET_PCAP=build/network.pcap` to record
Ethernet traffic from both directions in standard PCAP format. The capture can grow
with traffic, so enable it only while diagnosing. State JSON includes packet
counters, ring pointers, command, ISR and IMR.

Snapshot version 4 adds card registers, packet RAM and the bounded wire queue.
Versions 1–3 remain loadable with a reset card. Live host sockets are not saved:
restore and guest reset close the old flows. An application must reconnect after
restoring a checkpoint containing an established TCP connection; stale guest TCP
traffic receives a reset. DHCP and DNS work again without host configuration.

Collision/backoff timing, DP8390 remote "send packet" DMA and page-2 diagnostic
writes are not implemented. The supported DMA/register paths have been exercised
by Windows 95's own NE2000 driver. Java/JavaScript can be re-enabled in Netscape's
Options, Network Preferences, Languages panel.

## Sources and checks

Hardware reference: National Semiconductor's [DP8390D/NS32490D datasheet](https://media.digikey.com/pdf/Data%20Sheets/Texas%20Instruments%20PDFs/DP8390D%2CNS32490D.pdf),
saved under `docs/specs/dp8390d.pdf`. Protocol references are saved alongside it:
[RFC 791](https://www.rfc-editor.org/rfc/rfc791),
[RFC 768](https://www.rfc-editor.org/rfc/rfc768),
[RFC 826](https://www.rfc-editor.org/rfc/rfc826),
[RFC 2131](https://www.rfc-editor.org/rfc/rfc2131) and
[RFC 9293](https://www.rfc-editor.org/rfc/rfc9293).

`make test` includes `build/network-tests`: PROM reads and WTS stride, remote DMA
completion/count/wrap, ISR/IMR, receive filtering, ring wrap/overflow, transmission,
IRQ 10 delivery, card/wire snapshot roundtrip, ARP, DHCP, ICMP, HTTP TCP handshake,
retransmission/FIN, actual host TCP/UDP socket forwarding and malformed packet
rejection. The socket tests use local listeners and need no external service.

The host gateway also has a device-independent packet queue/receive callback
interface for modern virtio-net. `Network::forwardTCP(hostPort, guestPort)` binds
only `127.0.0.1`; port zero allocates an ephemeral host port. An accepted host
socket opens a guest-side TCP connection from the gateway address, with normal
sequence/checksum validation, retransmission and close handling. Existing
NE2000 callers use a wrapper around the same gateway. Portable network checks
exercise a real loopback forward in both directions through the callback API.
