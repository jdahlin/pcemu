# Compatibility aliases. Compilation and dependency tracking are handled by Ninja.
.DEFAULT_GOAL := all
# Ninja owns parallelism; never start competing Ninja writers via make -j.
.NOTPARALLEL:
.PHONY: FORCE
FORCE:
build/%: FORCE
	@./tools/build.sh $*

.PHONY: firmware all test clean run dos bench app app95 smoke-win311 smoke-reset smoke-guest-perf smoke-guest-memory
all: build/pcemu
smoke-guest-memory: build/memory-bench
	python3 tools/smoke_guest_memory.py

test: build/jit-jump-tests build/jit-rmw-tests build/hardware-device-tests build/i8042-tests build/pc-component-tests build/io-port-space-tests build/frontend-tests build/jit-read-tests build/network-tests build/integration-tests build/tests build/snapshot-tests build/jit-byte-tests build/jit-stack-tests build/jit-flags-tests
	python3 -m unittest discover -s tests -p test_guest_memory.py
	python3 -m unittest discover -s tests -p test_device_boundaries.py
	python3 -m unittest discover -s tests -p test_display_modes.py
	./build/hardware-device-tests
	./build/i8042-tests
	./build/pc-component-tests
	./build/io-port-space-tests
	./build/frontend-tests
	./build/jit-read-tests
	./build/jit-jump-tests
	./build/jit-rmw-tests
	./build/network-tests
	./build/integration-tests
	./build/tests
	./build/snapshot-tests
	./build/jit-byte-tests
	./build/jit-stack-tests
	./build/jit-flags-tests
run: build/pcemu
	./run.sh
dos: build/pcemu
	./build/pcemu --disk media/dos.raw
app: build/pcemu
	python3 tools/build_app.py
app95: build/pcemu
	python3 tools/build_app.py --windows95
smoke-win311: build/pcemu
	python3 tools/smoke_win311.py
smoke-reset: build/pcemu
	python3 tools/smoke_reset.py
bench: build/pcemu
	./build/pcemu --benchmark
clean:
	rm -rf build

firmware:
	python3 tools/build_firmware.py

smoke-guest-perf: build/guest-bench build/disk-tool
	python3 tools/smoke_guest_perf.py

.PHONY: smoke-guest-profile
smoke-guest-profile: build/guest-bench-profile build/guest-bench build/disk-tool
	python3 tools/smoke_guest_profile.py

.PHONY: smoke-win95
smoke-win95: build/pcemu build/disk-tool
	python3 tools/smoke_win95.py


# Optional ARM64 hypervisor runner. No GUI or x86 core; shared COW storage.
# Ad-hoc signing grants the entitlement required by Hypervisor.framework.
.PHONY: test-hvf
test-hvf: build/arm-hyperv-tests build/device-bus-tests build/virtio-rng-tests build/nvme-tests build/virtio-net-tests acpi-arm64 build/pcemu-hvf build/arm-device-tests build/virtio-tests build/vhdx-tests
	./build/device-bus-tests
	./build/arm-hyperv-tests
	./build/arm-device-tests
	./build/virtio-tests
	./build/virtio-net-tests
	./build/virtio-rng-tests
	./build/vhdx-tests
	./build/nvme-tests
	./build/pcemu-hvf --self-test
	python3 -m unittest discover -s tests -p test_hvf_cli.py
	python3 -m unittest discover -s tests -p test_arm_acpi.py
	python3 -m unittest discover -s tests -p test_arm_platform.py
	python3 -m unittest discover -s tests -p test_windows_hyperv.py
.PHONY: firmware-arm64 smoke-uefi
firmware-arm64: acpi-arm64 build/arm64-fw/pcemu-virt.dtb
	python3 tools/fetch_arm64_uefi.py

.PHONY: acpi-arm64
acpi-arm64:
	python3 tools/build_arm_acpi.py
build/arm64-fw/pcemu-virt.dtb: platform/arm64/pcemu-virt.dts
	@mkdir -p build/arm64-fw
	dtc -I dts -O dtb -o $@ $<
smoke-uefi: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_uefi.py

.PHONY: smoke-arm-acpi
smoke-arm-acpi: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_arm_acpi.py

.PHONY: smoke-windows-loader
smoke-windows-loader: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_windows_loader.py

.PHONY: smoke-arm-hyperv
smoke-arm-hyperv: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_arm_hyperv.py

.PHONY: smoke-virtio
smoke-virtio: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_virtio.py

.PHONY: test-vhdx
test-vhdx: build/vhdx-tests
	./build/vhdx-tests

.PHONY: smoke-vhdx
smoke-vhdx: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_vhdx.py

.PHONY: test-virtio-net
test-virtio-net: build/virtio-net-tests
	./build/virtio-net-tests

.PHONY: test-nvme smoke-nvme
test-nvme: build/nvme-tests
	./build/nvme-tests
smoke-nvme: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_vhdx.py --disk-controller nvme

.PHONY: smoke-arm-integration
smoke-arm-integration: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_arm_integration.py

.PHONY: smoke-arm-reboot
smoke-arm-reboot: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_arm_reboot.py

# This target intentionally does not link CPU, JIT, Machine, HVF or a frontend.
.PHONY: test-qcow2
test-qcow2: build/qcow2-tests
	./build/qcow2-tests

# Keep the format checks in both regular and HVF validation suites.
test test-hvf: test-qcow2

.PHONY: test-qcow2-interop
test-qcow2-interop: build/qcow2-tests
	python3 tests/test_qcow2_interop.py


# VMBus guest-memory descriptors are portable and require no disk or hypervisor.
.PHONY: test-arm-gpadl
test-arm-gpadl: build/arm-gpadl-tests
	./build/arm-gpadl-tests

test test-hvf: test-arm-gpadl

# VMBus ring transport over validated GPADLs; no guest image is required.
.PHONY: test-arm-vmbus-ring
test-arm-vmbus-ring: build/arm-vmbus-ring-tests
	./build/arm-vmbus-ring-tests

test test-hvf: test-arm-vmbus-ring

# Registered VMBus channel lifecycle and real Hyper-V/SynIC dispatch, diskless.
.PHONY: test-arm-vmbus-channel
test-arm-vmbus-channel: build/arm-vmbus-channel-tests
	./build/arm-vmbus-channel-tests

test test-hvf: test-arm-vmbus-channel

.PHONY: test-arm-hyperv-heartbeat
test-arm-hyperv-heartbeat: build/arm-hyperv-heartbeat-tests
	./build/arm-hyperv-heartbeat-tests

test test-hvf: test-arm-hyperv-heartbeat

.PHONY: test-arm-gic-ns test-arm-hvf-gic
test-arm-gic-ns: build/arm-gic-ns-tests
	./build/arm-gic-ns-tests
test test-hvf: test-arm-gic-ns

test-arm-hvf-gic: build/arm-hvf-gic-tests
	./build/arm-hvf-gic-tests
test-hvf: test-arm-hvf-gic

.PHONY: test-arm-hyperv-timesync
test-arm-hyperv-timesync: build/arm-hyperv-timesync-tests
	./build/arm-hyperv-timesync-tests
test test-hvf: test-arm-hyperv-timesync

.PHONY: test-arm-hyperv-timesync-integration
test-arm-hyperv-timesync-integration: build/arm-hyperv-timesync-integration-tests
	./build/arm-hyperv-timesync-integration-tests
test test-hvf: test-arm-hyperv-timesync-integration

.PHONY: test-arm-hyperv-shutdown
test-arm-hyperv-shutdown: build/arm-hyperv-shutdown-tests
	./build/arm-hyperv-shutdown-tests
test test-hvf: test-arm-hyperv-shutdown

.PHONY: test-arm-hyperv-shutdown-integration
test-arm-hyperv-shutdown-integration: build/arm-hyperv-shutdown-integration-tests
	./build/arm-hyperv-shutdown-integration-tests
test test-hvf: test-arm-hyperv-shutdown-integration

# Portable reference-clock page contract; native advertisement is separate.
.PHONY: test-arm-hyperv-reference-clock
test-arm-hyperv-reference-clock: build/arm-hyperv-reference-clock-tests
	./build/arm-hyperv-reference-clock-tests
test test-hvf: test-arm-hyperv-reference-clock

.PHONY: test-arm-hvf-reference-counter
test-arm-hvf-reference-counter: build/arm-hvf-reference-counter-tests
	./build/arm-hvf-reference-counter-tests
test-hvf: test-arm-hvf-reference-counter

.PHONY: test-arm-hvf-reference-page
test-arm-hvf-reference-page: build/arm-hvf-reference-page-tests
	./build/arm-hvf-reference-page-tests
test-hvf: test-arm-hvf-reference-page

.PHONY: test-arm-hyperv-kvp-codec
test-arm-hyperv-kvp-codec: build/arm-hyperv-kvp-codec-tests
	./build/arm-hyperv-kvp-codec-tests
test test-hvf: test-arm-hyperv-kvp-codec

.PHONY: test-arm-hyperv-kvp
test-arm-hyperv-kvp: build/arm-hyperv-kvp-tests
	./build/arm-hyperv-kvp-tests
test test-hvf: test-arm-hyperv-kvp

.PHONY: test-arm-hyperv-kvp-integration
test-arm-hyperv-kvp-integration: build/arm-hyperv-kvp-integration-tests
	./build/arm-hyperv-kvp-integration-tests
test test-hvf: test-arm-hyperv-kvp-integration

.PHONY: test-hyperv-kvp-roundtrip
test-hyperv-kvp-roundtrip: build/hyperv-kvp-roundtrip-tests
	./build/hyperv-kvp-roundtrip-tests
test test-hvf: test-hyperv-kvp-roundtrip
