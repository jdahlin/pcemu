# Validation OS ARM64 networking inventory

External networking is **not verified** in the current Windows ARM64 VM. The
ARM platform exposes storage and a UART but no network PCI function, and it does
not connect to the host socket gateway. `VirtioNet` exists as a portable model;
its queue tests are not evidence of packets passing through Windows.

## What the supplied images contain

Read-only inspection of both `winvos_install` (image 1) and `winvos_boot`
(image 2) in the original ARM64 Validation OS 26100.9549 WIM found the same
network components. Selected present binaries were checked as ARM64 PE images.
This inventories the WIM, not the live VM or any later changes to its VHDX.

| Component | Files | Meaning |
|---|---|---|
| Network stack | `ndis.sys`, `netio.sys`, `tcpip.sys`, `tcpipreg.sys`, `afd.sys`, `nsiproxy.sys`, `fwpkclnt.sys`, `msquic.sys` | Protocol/socket/filter support; not a hardware NIC driver |
| Driver frameworks and access | `netadaptercx.sys`, `netcxrd.sys`, `ndisuio.sys` | Adapter framework/helper components; no supported Ethernet device implied |
| Debug adapters | `kdnic.sys`, `kdnic_legacy.sys` | Microsoft Kernel Debug Network Adapter, enumerated as `root\kdnic` / `kdnic\legacy` |
| Network tools | `ipconfig.exe`, `ping.exe`, `netsh.exe` | Present at `Windows\System32` |
| VirtIO network | `netkvm.sys` | Absent |
| Hyper-V network | `netvsc.sys` | Absent |
| USB network candidates | `rndismp.sys`, `usb8023.sys`, `usb8023x.sys`, `usbncm.sys` | Absent |

All `Windows/INF/*.inf` files were examined. The network-related packages are
`c_net.inf`, `c_netclient.inf`, `c_netservice.inf`, `c_nettrans.inf`, `kdnic.inf`,
`kdnic_legacy.inf`, `ndisuio.inf`, `netcxrd.inf` and `wfplwfs.inf`. No network
INF in either image advertises a PCI, USB or VMBus hardware ID. The `c_net*`
files register device classes; they do not add Ethernet hardware support.
`netcxrd.inf` describes a WDF Network Adapter Extension Device (class Generic).
KDNIC is a debug-transport adapter, not a ready-to-use VirtIO adapter.

`pnputil.exe`, `curl.exe`, `powershell.exe` and `dism.exe` are absent from the
standard `Windows/System32` locations checked. PowerShell module directories
and DISM provider DLLs do exist; their presence does not establish executable
availability elsewhere or an installed PowerShell/DISM command environment.

## Reproduce

```sh
python3 tools/inspect_validation_network.py
python3 -m unittest discover -s tests -p test_validation_network.py
clang++ -Isrc -std=c++20 -O2 src/virtio_net.cpp tests/virtio_net_test.cpp \
  -o build/virtio-net-tests
build/virtio-net-tests
```

The inventory extracts INF text and selected binaries into memory, without
mounting, expanding, or modifying either Windows image. Its JSON report is
ignored at `build/validation-network/inventory.json`; it includes binary hashes,
PE architectures, parsed INF metadata and the complete top-level driver list.
`guestNetworkVerified` stays false. Both images were inspected successfully;
the parser/PE tests and all 36 existing virtio-net model checks passed.

## Required for an end-to-end test

1. Attach the virtio-net PCI function alongside NVMe, with an ACPI interrupt
   route and connection to the host socket gateway.
2. Supply a signed ARM64 NetKVM package compatible with this build and install
   it in a private test image. General Windows VirtIO support does not imply
   that this minimal Validation OS image includes the driver.
3. Provide a way to run and capture commands in Windows; the present runner's
   command injection targets the UEFI serial shell, not the graphical `cmd.exe`.
4. Verify adapter/driver enumeration, DHCP address/gateway, reachability, DNS
   and a TCP request to a controlled host endpoint; correlate guest output with
   packet capture. Loopback or model queue tests alone are insufficient.

No driver package was installed and no networking changes were made to the
working Windows VHDX during this inventory.

Microsoft references: [Validation OS customization and driver packages](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/validation-os-customize),
[NetAdapterCx framework](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/),
and [KDNIC's relationship to KDNET](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/setting-up-a-kdnet-usb-connection#no_kdnic-setting-for-enhanced-performance-and-reliablity).

## Staged ARM64 NetKVM package

`python3 tools/fetch_netkvm_arm64.py` extracts only the Windows 11 ARM64 driver
package from upstream virtio-win 0.1.302 (driver version 100.103.104.30200), using
verified HTTP ranges rather than retaining the 837 MiB ISO. The local package
is ignored at `media/netkvm-arm64-0.1.302`; its manifest records source ETag and
file SHA-256 hashes. SYS/helper EXEs are checked as ARM64 and the INF matches
`PCI\VEN_1AF4&DEV_1041`. Catalog presence does not establish Windows signature
acceptance. This package has been downloaded and inspected, not installed.
The current follow-up is full Windows 11 ARM64 installation media, whose WinPE
servicing tools can provide a Windows-based driver-installation environment.
