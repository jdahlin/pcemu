CXX := clang++
CPPFLAGS := -Isrc $(shell pkg-config --cflags libzstd)
SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
SDL_LIBS = $(shell pkg-config --libs sdl2)
CXXFLAGS := -std=c++20 -O3 -g -Wall -Wextra -Wpedantic
LDLIBS = $(SDL_LIBS) $(shell pkg-config --libs libzstd)
SESSION := src/pc_display.cpp src/session_diagnostics.cpp src/cpu_benchmarks.cpp
HEADERS := $(wildcard src/*.hpp)
PS2_TEST_SUPPORT := tests/ps2_test_support.hpp
JIT_CORE := src/jit.cpp src/jit_decode.cpp src/jit_flags.cpp \
	src/jit_runtime.cpp src/jit_arm64.cpp
CORE := src/cpu.cpp src/machine.cpp src/pc_at_io.cpp src/pc_firmware.cpp src/pc_bios.cpp src/pc_bios_video.cpp src/i8042.cpp src/pc_input.cpp src/cmos_rtc.cpp src/disk.cpp src/pc_bios_disk.cpp src/ata.cpp src/pic.cpp src/i8259_pair.cpp src/pit.cpp src/vga.cpp $(JIT_CORE) src/snapshot.cpp src/mouse.cpp src/pci.cpp src/vga_pci.cpp src/host_keyboard.cpp src/ne2000.cpp
.PHONY: firmware all test clean run dos bench app app95 smoke-win311 smoke-reset smoke-guest-perf
all: build/pcemu
build/pcemu-profile: $(CORE) $(SESSION) $(HEADERS) src/network.cpp src/frontend_sdl.cpp src/main.cpp src/profile.hpp src/cpu.hpp src/machine.hpp src/jit.hpp src/vga.hpp src/ata.hpp src/pic.hpp src/pit.hpp
	$(CXX) $(CPPFLAGS) $(SDL_CFLAGS) -DPCEMU_PROFILE $(CXXFLAGS) $(CORE) $(SESSION) src/main.cpp src/network.cpp src/frontend_sdl.cpp $(LDLIBS) -o $@
build/disk-tool: $(CORE) $(HEADERS) tools/disk_tool.cpp src/machine.hpp src/cpu.hpp src/profile.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/disk_tool.cpp $(shell pkg-config --libs libzstd) -o $@
build/pcemu: $(CORE) $(SESSION) $(HEADERS) src/network.cpp src/frontend_sdl.cpp src/main.cpp src/cpu.hpp src/machine.hpp src/jit.hpp src/vga.hpp src/ata.hpp src/pic.hpp src/pit.hpp src/profile.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(SDL_CFLAGS) $(CXXFLAGS) $(CORE) $(SESSION) src/main.cpp src/network.cpp src/frontend_sdl.cpp $(LDLIBS) -o $@
build/tests: $(CORE) $(HEADERS) $(PS2_TEST_SUPPORT) tests/test.cpp src/cpu.hpp src/machine.hpp src/jit.hpp src/vga.hpp src/ata.hpp src/pic.hpp src/pit.hpp src/profile.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/test.cpp $(shell pkg-config --libs libzstd) -o $@
build/snapshot-tests: $(CORE) $(HEADERS) $(PS2_TEST_SUPPORT) tests/snapshot_test.cpp src/cpu.hpp src/machine.hpp src/jit.hpp src/ata.hpp src/vga.hpp src/pic.hpp src/pit.hpp src/profile.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/snapshot_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/jit-byte-tests: $(CORE) $(HEADERS) tests/jit_bytes_test.cpp src/cpu.hpp src/machine.hpp src/jit.hpp src/ata.hpp src/vga.hpp src/pic.hpp src/pit.hpp src/profile.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/jit_bytes_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/jit-stack-tests: $(CORE) $(HEADERS) tests/jit_stack_test.cpp src/cpu.hpp src/machine.hpp src/jit.hpp src/ata.hpp src/vga.hpp src/pic.hpp src/pit.hpp src/profile.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/jit_stack_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/jit-flags-tests: $(CORE) $(HEADERS) tests/jit_flags_test.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/jit_flags_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/integration-tests: $(CORE) $(HEADERS) $(PS2_TEST_SUPPORT) tests/integration_test.cpp src/machine.hpp src/mouse.hpp src/vga.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/integration_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/network-tests: $(CORE) $(HEADERS) src/network.cpp tests/network_test.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) src/network.cpp tests/network_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/network-tests-sanitize: $(CORE) $(HEADERS) src/network.cpp tests/network_test.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(CORE) src/network.cpp tests/network_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/guest-bench: $(CORE) $(HEADERS) tools/guest_bench.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/guest_bench.cpp $(shell pkg-config --libs libzstd) -o $@
build/guest-bench-profile: $(CORE) $(HEADERS) tools/guest_bench.cpp
	$(CXX) $(CPPFLAGS) -DPCEMU_PROFILE $(CXXFLAGS) $(CORE) tools/guest_bench.cpp $(shell pkg-config --libs libzstd) -o $@
build/jit-jump-tests: $(CORE) $(HEADERS) tests/jit_jump_test.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/jit_jump_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/jit-rmw-tests: $(CORE) $(HEADERS) tests/jit_rmw_test.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/jit_rmw_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/jit-read-tests: $(CORE) $(HEADERS) tests/jit_read_test.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/jit_read_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/frontend-tests: src/frontend.hpp src/frontend_sdl.cpp src/host_key.hpp tests/frontend_test.cpp
	$(CXX) -Isrc $(SDL_CFLAGS) $(CXXFLAGS) src/frontend_sdl.cpp tests/frontend_test.cpp $(SDL_LIBS) -o $@
build/io-port-space-tests: src/io_port_space.hpp tests/io_port_space_test.cpp
	@mkdir -p build
	$(CXX) -Isrc $(CXXFLAGS) tests/io_port_space_test.cpp -o $@
test: build/jit-jump-tests build/jit-rmw-tests build/hardware-device-tests build/i8042-tests build/pc-component-tests build/io-port-space-tests build/frontend-tests build/jit-read-tests build/network-tests build/integration-tests build/tests build/snapshot-tests build/jit-byte-tests build/jit-stack-tests build/jit-flags-tests
	python3 -m unittest discover -s tests -p test_device_boundaries.py
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

ARM_BUS := src/mmio_space.cpp src/pci_ecam.cpp
ARM_BUS_HEADERS := src/mmio_space.hpp src/pci_ecam.hpp
ARM_DEVICES := src/arm_hyperv.cpp src/arm_pl011.cpp src/arm_pl031.cpp src/arm_nor_flash.cpp src/arm_psci.cpp src/arm_os_lock.cpp src/arm_mmio.cpp
ARM_DEVICE_HEADERS := src/arm_hyperv.hpp src/arm_pl011.hpp src/arm_pl031.hpp src/arm_nor_flash.hpp src/arm_psci.hpp src/arm_os_lock.hpp src/arm_mmio.hpp

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
build/pcemu-hvf: src/hvf_secondary.hpp src/virtio_rng.cpp src/virtio_rng.hpp src/nvme.cpp src/nvme.hpp src/network.cpp src/network.hpp src/ne2000.cpp src/virtio_net.cpp src/virtio_net.hpp src/vhdx.hpp src/virtio_pci.cpp src/virtio_pci.hpp src/disk.cpp src/disk.hpp src/virtio_blk.cpp src/virtio_blk.hpp src/arm_virt.cpp src/arm_virt_devices.cpp src/arm_virt.hpp src/arm_ramfb.cpp src/arm_ramfb.hpp $(ARM_BUS) $(ARM_BUS_HEADERS) src/arm_fw_cfg.cpp src/arm_fw_cfg.hpp $(ARM_DEVICES) $(ARM_DEVICE_HEADERS) src/hvf_machine.cpp src/hvf_machine.hpp src/machine_profile.hpp tools/hvf_main.cpp platform/macos/hypervisor.entitlements
	@test "$$(uname -s)-$$(uname -m)" = Darwin-arm64 || { echo 'HVF requires Apple Silicon macOS'; exit 1; }
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/hvf_machine.cpp src/arm_virt.cpp src/arm_virt_devices.cpp src/arm_ramfb.cpp $(ARM_BUS) src/arm_fw_cfg.cpp $(ARM_DEVICES) src/disk.cpp src/virtio_blk.cpp src/virtio_pci.cpp src/virtio_net.cpp src/virtio_rng.cpp src/network.cpp src/ne2000.cpp src/nvme.cpp tools/hvf_main.cpp $(shell pkg-config --libs libzstd) -framework Hypervisor -o $@
	codesign --force --sign - --entitlements platform/macos/hypervisor.entitlements $@

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

build/arm-hyperv-tests: src/arm_hyperv.cpp src/arm_hyperv.hpp tests/arm_hyperv_test.cpp
	@mkdir -p build
	$(CXX) -Isrc $(CXXFLAGS) src/arm_hyperv.cpp tests/arm_hyperv_test.cpp -o $@

build/arm-device-tests: src/arm_fw_cfg.cpp src/arm_fw_cfg.hpp $(ARM_DEVICES) $(ARM_DEVICE_HEADERS) tests/arm_devices_test.cpp
	@mkdir -p build
	$(CXX) -Isrc $(CXXFLAGS) src/arm_fw_cfg.cpp $(ARM_DEVICES) tests/arm_devices_test.cpp -o $@

build/virtio-tests: src/vhdx.hpp src/virtio_pci.cpp src/virtio_pci.hpp src/disk.cpp src/disk.hpp src/virtio_blk.cpp src/virtio_blk.hpp tests/virtio_blk_test.cpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/disk.cpp src/virtio_blk.cpp src/virtio_pci.cpp tests/virtio_blk_test.cpp $(shell pkg-config --libs libzstd) -o $@

.PHONY: smoke-virtio
smoke-virtio: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_virtio.py

.PHONY: test-vhdx
test-vhdx: build/vhdx-tests
	./build/vhdx-tests

build/vhdx-tests: src/disk.cpp src/disk.hpp src/vhdx.hpp tests/vhdx_test.cpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/disk.cpp tests/vhdx_test.cpp $(shell pkg-config --libs libzstd) -o $@

.PHONY: smoke-vhdx
smoke-vhdx: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_vhdx.py

.PHONY: test-virtio-net
build/virtio-net-tests: src/virtio_net.cpp src/virtio_net.hpp tests/virtio_net_test.cpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/virtio_net.cpp tests/virtio_net_test.cpp -o $@
test-virtio-net: build/virtio-net-tests
	./build/virtio-net-tests

.PHONY: test-nvme smoke-nvme
test-nvme: build/nvme-tests
	./build/nvme-tests
build/nvme-tests: src/nvme.cpp src/nvme.hpp src/disk.cpp src/disk.hpp src/vhdx.hpp tests/nvme_test.cpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/nvme.cpp src/disk.cpp tests/nvme_test.cpp $(shell pkg-config --libs libzstd) -o $@
smoke-nvme: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_vhdx.py --disk-controller nvme

build/virtio-rng-tests: src/disk.cpp src/disk.hpp src/virtio_blk.cpp src/virtio_blk.hpp src/virtio_rng.cpp src/virtio_rng.hpp src/virtio_pci.cpp src/virtio_pci.hpp tests/virtio_rng_test.cpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/disk.cpp src/virtio_blk.cpp src/virtio_rng.cpp src/virtio_pci.cpp tests/virtio_rng_test.cpp $(shell pkg-config --libs libzstd) -o $@

.PHONY: smoke-arm-integration
smoke-arm-integration: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_arm_integration.py

.PHONY: smoke-arm-reboot
smoke-arm-reboot: build/pcemu-hvf firmware-arm64
	python3 tools/smoke_arm_reboot.py

build/device-bus-tests: $(ARM_BUS) $(ARM_BUS_HEADERS) src/arm_ramfb.cpp src/arm_ramfb.hpp src/arm_fw_cfg.cpp src/arm_fw_cfg.hpp tests/device_bus_test.cpp
	@mkdir -p build
	$(CXX) -Isrc $(CXXFLAGS) $(ARM_BUS) src/arm_ramfb.cpp src/arm_fw_cfg.cpp tests/device_bus_test.cpp -o $@

build/pc-component-tests: $(CORE) $(HEADERS) src/pc_display.cpp tests/pc_components_test.cpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) src/pc_display.cpp tests/pc_components_test.cpp $(shell pkg-config --libs libzstd) -o $@

build/i8042-tests: src/i8042.cpp src/i8042.hpp src/pc_input.cpp src/pc_input.hpp src/mouse.cpp src/mouse.hpp tests/i8042_test.cpp
	@mkdir -p build
	$(CXX) -Isrc $(CXXFLAGS) src/i8042.cpp src/pc_input.cpp src/mouse.cpp tests/i8042_test.cpp -o $@

# This target intentionally does not link CPU, JIT, Machine, HVF or a frontend.
HARDWARE_MODELS := src/ata.cpp src/disk.cpp src/pic.cpp src/i8259_pair.cpp src/pit.cpp src/vga.cpp src/pci.cpp src/vga_pci.cpp src/ne2000.cpp src/network.cpp src/i8042.cpp src/mouse.cpp src/cmos_rtc.cpp $(ARM_BUS) $(ARM_DEVICES) src/arm_fw_cfg.cpp src/arm_ramfb.cpp src/virtio_blk.cpp src/virtio_net.cpp src/virtio_rng.cpp src/virtio_pci.cpp src/nvme.cpp
build/hardware-device-tests: $(HARDWARE_MODELS) $(HEADERS) tests/hardware_devices_test.cpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(HARDWARE_MODELS) tests/hardware_devices_test.cpp $(shell pkg-config --libs libzstd) -o $@
