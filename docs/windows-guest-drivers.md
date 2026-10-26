# Windows ARM64 guest drivers and device coverage

The 2026-10-26 Windows inventory and installation used the signed ARM64 packages
from the [official stable VirtIO 0.1.302 ISO](https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/latest-virtio/).
The working viostor 0.1.285 boot package was retained for the current INTx-only
controller; the later package has a known interrupt setup incompatibility.

The earlier driver inventory validated all twelve catalogs as Microsoft Windows Hardware
Compatibility Publisher signatures. All twelve driver families were in that inventory
Driver Store. The fresh compact installation contains its required drivers;
serial, balloon and GPU were subsequently validated on this local image. Five families were newly added: viogpudo, pvpanic, viofs, viomem
and viosock. Packages without matching hardware are staged, not active.

| Family | Guest package | Host device / guest state at installation |
| --- | --- | --- |
| Block | viostor 0.1.285 | PCI 1AF4:1042; signed, running |
| Network | NetKVM 0.1.302 | PCI 1AF4:1041; signed, running; DNS and HTTP verified |
| Entropy | viorng 0.1.302 | PCI 1AF4:1044; signed; device problem code 0 |
| SCSI | vioscsi 0.1.302 | Staged; separate virtio-scsi controller absent |
| Serial | vioserial 0.1.302 | Named agent port implemented; signed ARM64 installed-Windows binary I/O verified |
| Input | vioinput 0.1.302 | Staged; current input uses working USB HID |
| Balloon | Balloon 0.1.302 | Inflate/deflate verified with explicit PFN64 compatibility; see guest-tool device notes |
| Display | viogpudo 0.1.302 | Signed, running VirtIO GPU 2D; 1280×720 scanout and RGB transfers verified |
| Shared filesystem | viofs 0.1.302 | Staged; host filesystem device/backend absent |
| Memory hotplug | viomem 0.1.302 | Staged; host memory-hotplug device absent |
| Guest sockets | viosock 0.1.302 | Staged; host vsock device/backend absent |
| Panic notification | pvpanic 0.1.302 | Staged; panic device absent |

Microsoft's inbox VMBus, heartbeat, time synchronization, guest shutdown and data
exchange drivers were already installed. The four corresponding integration
services were running. Current keyboard/mouse use Microsoft's inbox USB drivers.
Installing additional driver packages alone does not accelerate these devices.

The stable ISO supplies x86/x64 QEMU guest-agent MSI files and x86/x64 combined
guest-tools installers. It has no native ARM64 agent MSI. More importantly,
the [VirtIO serial agent channel](virtio-guest-tools.md) is now implemented. A
compatible ARM64 guest-agent service and SPICE server remain absent, so no
unused guest-agent/balloon/display service was started. VirtIO-GPU 2D display also does not imply 3D acceleration.

One pre-existing unknown device remains: ACPI QEMU0002 (fw_cfg), problem code 28.
The stable fwcfg and qemufwcfg INF models cover x86/x64 only; they were not forced
onto ARM64 and no signature requirements were relaxed.

## Reproduce preparation and import

```sh
python3 tools/fetch_virtio_arm64.py --all-drivers --destination media/virtio-arm64-complete-0.1.302
python3 tools/fetch_virtio_arm64.py --viostor-intx --destination media/viostor-arm64-intx-0.1.285
python3 tools/build_windows_driver_bundle.py --output build/windows-drivers.zip
```

Existing destination directories are intentionally not overwritten. Transfer and
extract the ZIP to `C:\PCEMU\VirtIO-ARM64`, then run
`tools/windows/install_guest_drivers.ps1` in an elevated native ARM64 PowerShell.
It checks manifest hashes and catalog trust before importing individual INFs,
preserves an already-running viostor boot driver, rescans hardware, and records
both active drivers and the Driver Store. It does not install optional services.
PnPUtil return 259 is recorded as no device update (no matching device or an
existing preferred driver), consistent with [Microsoft's return-value documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-return-values).

The native run completed without a reboot requirement. Post-installation checks
confirmed unchanged working disk/network/RNG devices, loaded NetKVM and viostor,
DNS and HTTP download/upload, and no new device problem codes. No performance
improvement is claimed from staging unused packages. Serial, balloon and GPU now have native installed-Windows validation and are
enabled by `run11.sh`; see [guest-tool devices](virtio-guest-tools.md). Optional
SCSI, shared filesystem, input, memory hotplug, vsock and panic devices remain
outside this implementation.
