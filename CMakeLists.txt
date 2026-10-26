cmake_minimum_required(VERSION 3.20)
# Fast development builds; Release remains an explicit -O3 opt-in.
set(CMAKE_CXX_FLAGS_DEBUG_INIT "-O1")
project(pcemu LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(PkgConfig REQUIRED)
find_package(ZLIB REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
pkg_check_modules(SPICE QUIET IMPORTED_TARGET spice-server glib-2.0)
set_property(GLOBAL PROPERTY JOB_POOLS compile_pool=4 link_pool=2)
set(CMAKE_JOB_POOL_COMPILE compile_pool)
set(CMAKE_JOB_POOL_LINK link_pool)
include(CTest)

set(CORE
  src/cpu.cpp
  src/machine.cpp
  src/pc_at_io.cpp
  src/pc_firmware.cpp
  src/pc_bios.cpp
  src/pc_bios_video.cpp
  src/i8042.cpp
  src/pc_input.cpp
  src/cmos_rtc.cpp
  src/disk.cpp
  src/pc_bios_disk.cpp
  src/ata.cpp
  src/pic.cpp
  src/i8259_pair.cpp
  src/pit.cpp
  src/vga.cpp
  src/jit.cpp
  src/jit_decode.cpp
  src/jit_flags.cpp
  src/jit_runtime.cpp
  src/jit_arm64.cpp
  src/snapshot.cpp
  src/mouse.cpp
  src/pci.cpp
  src/vga_pci.cpp
  src/host_keyboard.cpp
  src/ne2000.cpp
)
set(SESSION
  src/pc_display.cpp
  src/session_diagnostics.cpp
  src/cpu_benchmarks.cpp
)
set(ARM_BUS
  src/mmio_space.cpp
  src/pci_ecam.cpp
)
set(ARM_DEVICES
  src/arm_gic_ns.cpp
  src/arm_gpadl.cpp
  src/arm_vmbus_ring.cpp
  src/arm_vmbus.cpp
  src/arm_hyperv_timers.cpp
  src/arm_synic.cpp
  src/arm_hyperv_heartbeat.cpp
  src/arm_hyperv_timesync.cpp
  src/arm_hyperv_shutdown.cpp
  src/arm_hyperv_reference_clock.cpp
  src/arm_hyperv_kvp_codec.cpp
  src/arm_hyperv_kvp.cpp
  src/arm_hyperv.cpp
  src/arm_pl011.cpp
  src/arm_pl031.cpp
  src/arm_nor_flash.cpp
  src/arm_psci.cpp
  src/arm_os_lock.cpp
  src/arm_mmio.cpp
)
set(HARDWARE_MODELS
  src/ata.cpp
  src/disk.cpp
  src/pic.cpp
  src/i8259_pair.cpp
  src/pit.cpp
  src/vga.cpp
  src/pci.cpp
  src/vga_pci.cpp
  src/ne2000.cpp
  src/network.cpp
  src/i8042.cpp
  src/mouse.cpp
  src/cmos_rtc.cpp
  src/mmio_space.cpp
  src/pci_ecam.cpp
  src/arm_gic_ns.cpp
  src/arm_gpadl.cpp
  src/arm_vmbus_ring.cpp
  src/arm_vmbus.cpp
  src/arm_hyperv_timers.cpp
  src/arm_synic.cpp
  src/arm_hyperv_heartbeat.cpp
  src/arm_hyperv_timesync.cpp
  src/arm_hyperv_shutdown.cpp
  src/arm_hyperv_reference_clock.cpp
  src/arm_hyperv_kvp_codec.cpp
  src/arm_hyperv_kvp.cpp
  src/arm_hyperv.cpp
  src/arm_pl011.cpp
  src/arm_pl031.cpp
  src/arm_nor_flash.cpp
  src/arm_psci.cpp
  src/arm_os_lock.cpp
  src/arm_mmio.cpp
  src/arm_fw_cfg.cpp
  src/arm_ramfb.cpp
  src/virtio_blk.cpp
  src/virtio_net.cpp
  src/virtio_rng.cpp
  src/virtio_device.cpp
  src/virtio_serial.cpp
  src/virtio_balloon.cpp
  src/virtio_gpu.cpp
  src/guest_agent_socket.cpp
  src/virtio_pci.cpp
  src/nvme.cpp
)

# Share objects across executables with identical compilation settings.
function(pcemu_program name)
  cmake_parse_arguments(P "PROFILE;SANITIZE;SDL;ZSTD;HVF" "" "SOURCES" ${ARGN})
  if(P_HVF AND NOT (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64"))
    return()
  endif()
  set(objects)
  foreach(source IN LISTS P_SOURCES)
    string(MAKE_C_IDENTIFIER "${source}_${P_PROFILE}_${P_SANITIZE}" object)
    set(object "obj_${object}")
    if(NOT TARGET ${object})
      add_library(${object} OBJECT EXCLUDE_FROM_ALL ${source})
      target_include_directories(${object} PRIVATE src)
      target_link_libraries(${object} PRIVATE PkgConfig::SDL2 PkgConfig::ZSTD)
      target_compile_options(${object} PRIVATE -Wall -Wextra -Wpedantic)
      if(source STREQUAL "src/frontend_spice.cpp" AND SPICE_FOUND)
        target_compile_definitions(${object} PRIVATE PCEMU_HAVE_SPICE)
        target_link_libraries(${object} PRIVATE PkgConfig::SPICE)
      endif()
      if(P_PROFILE)
        target_compile_definitions(${object} PRIVATE PCEMU_PROFILE)
      endif()
      if(P_SANITIZE)
        target_compile_options(${object} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      endif()
    endif()
    list(APPEND objects $<TARGET_OBJECTS:${object}>)
  endforeach()
  add_executable(${name} EXCLUDE_FROM_ALL ${objects})
  if(name STREQUAL "pcemu")
    set_target_properties(${name} PROPERTIES EXCLUDE_FROM_ALL FALSE)
  endif()
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
  if(SPICE_FOUND AND "src/frontend_spice.cpp" IN_LIST P_SOURCES)
    target_link_libraries(${name} PRIVATE PkgConfig::SPICE)
  endif()
  if(P_SDL)
    target_link_libraries(${name} PRIVATE PkgConfig::SDL2)
  endif()
  if(P_ZSTD)
    target_link_libraries(${name} PRIVATE PkgConfig::ZSTD ZLIB::ZLIB)
  endif()
  if(P_SANITIZE)
    target_link_options(${name} PRIVATE -fsanitize=address,undefined)
  endif()
  if(P_HVF)
    target_link_libraries(${name} PRIVATE "-framework Hypervisor")
    add_custom_command(TARGET ${name} POST_BUILD COMMAND codesign --force --sign -
      --entitlements ${PROJECT_SOURCE_DIR}/platform/macos/hypervisor.entitlements $<TARGET_FILE:${name}>)
    set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS
      ${PROJECT_SOURCE_DIR}/platform/macos/hypervisor.entitlements)
  elseif(name MATCHES "tests$")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  endif()
endfunction()

pcemu_program(pcemu-profile PROFILE SDL ZSTD
  SOURCES
    ${CORE}
    ${SESSION}
    src/main.cpp
    src/network.cpp
    src/frontend_sdl.cpp
)

pcemu_program(disk-tool ZSTD
  SOURCES
    ${CORE}
    tools/disk_tool.cpp
)

pcemu_program(pcemu SDL ZSTD
  SOURCES
    ${CORE}
    ${SESSION}
    src/main.cpp
    src/network.cpp
    src/frontend_sdl.cpp
)

pcemu_program(tests ZSTD
  SOURCES
    ${CORE}
    tests/test.cpp
)

pcemu_program(snapshot-tests ZSTD
  SOURCES
    ${CORE}
    tests/snapshot_test.cpp
)

pcemu_program(jit-byte-tests ZSTD
  SOURCES
    ${CORE}
    tests/jit_bytes_test.cpp
)

pcemu_program(jit-stack-tests ZSTD
  SOURCES
    ${CORE}
    tests/jit_stack_test.cpp
)

pcemu_program(jit-batch-tests ZSTD
  SOURCES
    ${CORE}
    tests/jit_batch_test.cpp
)

pcemu_program(jit-cached-batch-tests
  SOURCES
    src/cpu.cpp
    src/jit.cpp
    src/jit_decode.cpp
    src/jit_flags.cpp
    src/jit_runtime.cpp
    src/jit_arm64.cpp
    tests/jit_cached_batch_test.cpp
)

pcemu_program(jit-memory-push-tests ZSTD
  SOURCES
    ${CORE}
    tests/jit_memory_push_test.cpp
)

pcemu_program(jit-flags-tests ZSTD
  SOURCES
    ${CORE}
    tests/jit_flags_test.cpp
)

pcemu_program(integration-tests ZSTD
  SOURCES
    ${CORE}
    tests/integration_test.cpp
)

pcemu_program(network-tests ZSTD
  SOURCES
    ${CORE}
    src/network.cpp
    tests/network_test.cpp
)

pcemu_program(network-tests-sanitize SANITIZE ZSTD
  SOURCES
    ${CORE}
    src/network.cpp
    tests/network_test.cpp
)

pcemu_program(memory-bench ZSTD
  SOURCES
    ${CORE}
    tools/memory_bench.cpp
)

pcemu_program(memory-chase-bench
  SOURCES
    src/cpu.cpp
    src/jit.cpp
    src/jit_decode.cpp
    src/jit_flags.cpp
    src/jit_runtime.cpp
    src/jit_arm64.cpp
    tools/memory_chase_bench.cpp
)
add_test(NAME memory-chase-interpreter
  COMMAND memory-chase-bench --backend interpreter --loops 1000)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  add_test(NAME memory-chase-jit
    COMMAND memory-chase-bench --backend jit --loops 1000)
endif()

pcemu_program(guest-bench ZSTD
  SOURCES
    ${CORE}
    tools/guest_bench.cpp
)

pcemu_program(guest-bench-profile PROFILE ZSTD
  SOURCES
    ${CORE}
    tools/guest_bench.cpp
)

pcemu_program(jit-jump-tests ZSTD
  SOURCES
    ${CORE}
    tests/jit_jump_test.cpp
)

pcemu_program(jit-rmw-tests ZSTD
  SOURCES
    ${CORE}
    tests/jit_rmw_test.cpp
)

pcemu_program(jit-read-tests ZSTD
  SOURCES
    ${CORE}
    tests/jit_read_test.cpp
)

pcemu_program(frontend-tests SDL
  SOURCES
    src/frontend_sdl.cpp
    tests/frontend_test.cpp
)

pcemu_program(io-port-space-tests
  SOURCES
    tests/io_port_space_test.cpp
)

if(EXISTS "${PROJECT_SOURCE_DIR}/src/usb_xhci.cpp")
  set(HVF_USB_SOURCE src/usb_xhci.cpp)
endif()
pcemu_program(pcemu-hvf SDL ZSTD HVF
  SOURCES
    src/frontend_spice.cpp
    src/host_keyboard.cpp
    ${HVF_USB_SOURCE}
    src/frontend_sdl.cpp
    src/hvf_machine.cpp
    src/arm_virt.cpp
    src/arm_virt_devices.cpp
    src/arm_ramfb.cpp
    ${ARM_BUS}
    src/arm_fw_cfg.cpp
    ${ARM_DEVICES}
    src/disk.cpp
    src/virtio_blk.cpp
    src/virtio_pci.cpp
    src/virtio_net.cpp
    src/virtio_rng.cpp
    src/virtio_device.cpp
    src/virtio_serial.cpp
    src/virtio_balloon.cpp
    src/virtio_gpu.cpp
    src/guest_agent_socket.cpp
    src/network.cpp
    src/ne2000.cpp
    src/nvme.cpp
    tools/hvf_main.cpp
)

pcemu_program(arm-hyperv-tests
  SOURCES
    src/arm_gpadl.cpp
    src/arm_vmbus_ring.cpp
    src/arm_vmbus.cpp
    src/arm_hyperv_timers.cpp
    src/arm_synic.cpp
    src/arm_hyperv_heartbeat.cpp
    src/arm_hyperv_timesync.cpp
    src/arm_hyperv_shutdown.cpp
    src/arm_hyperv_reference_clock.cpp
    src/arm_hyperv_kvp_codec.cpp
    src/arm_hyperv_kvp.cpp
    src/arm_hyperv.cpp
    tests/arm_hyperv_test.cpp
)

pcemu_program(arm-device-tests
  SOURCES
    src/arm_fw_cfg.cpp
    ${ARM_DEVICES}
    tests/arm_devices_test.cpp
)

pcemu_program(virtio-tests ZSTD
  SOURCES
    src/disk.cpp
    src/virtio_blk.cpp
    src/virtio_pci.cpp
    tests/virtio_blk_test.cpp
)

pcemu_program(vhdx-tests ZSTD
  SOURCES
    src/disk.cpp
    tests/vhdx_test.cpp
)

pcemu_program(virtio-net-tests
  SOURCES
    src/virtio_net.cpp
    tests/virtio_net_test.cpp
)

pcemu_program(nvme-tests ZSTD
  SOURCES
    src/nvme.cpp
    src/disk.cpp
    tests/nvme_test.cpp
)

pcemu_program(virtio-rng-tests ZSTD
  SOURCES
    src/disk.cpp
    src/virtio_blk.cpp
    src/virtio_rng.cpp
    src/virtio_pci.cpp
    tests/virtio_rng_test.cpp
)

pcemu_program(device-bus-tests
  SOURCES
    ${ARM_BUS}
    src/arm_ramfb.cpp
    src/arm_fw_cfg.cpp
    tests/device_bus_test.cpp
)

pcemu_program(pc-component-tests ZSTD
  SOURCES
    ${CORE}
    src/pc_display.cpp
    tests/pc_components_test.cpp
)

pcemu_program(i8042-tests
  SOURCES
    src/i8042.cpp
    src/pc_input.cpp
    src/mouse.cpp
    tests/i8042_test.cpp
)

pcemu_program(hardware-device-tests ZSTD
  SOURCES
    ${HARDWARE_MODELS}
    tests/hardware_devices_test.cpp
)

if(EXISTS "${PROJECT_SOURCE_DIR}/src/usb_xhci.cpp")
pcemu_program(usb-xhci-tests
  SOURCES
    src/usb_xhci.cpp
    tests/usb_xhci_test.cpp
)
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/src/frontend_input.hpp")
pcemu_program(frontend-input-tests
  SOURCES
    src/host_keyboard.cpp
    tests/frontend_input_test.cpp
)
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/tests/disk_progress_test.cpp")
pcemu_program(disk-progress-tests ZSTD
  SOURCES
    src/disk.cpp
    tests/disk_progress_test.cpp
)
endif()

pcemu_program(qcow2-tests ZSTD
  SOURCES
    src/disk.cpp
    tests/qcow2_test.cpp
)

pcemu_program(arm-gpadl-tests
  SOURCES
    src/arm_gpadl.cpp
    tests/arm_gpadl_test.cpp
)

pcemu_program(arm-vmbus-ring-tests
  SOURCES
    src/arm_gpadl.cpp
    src/arm_vmbus_ring.cpp
    tests/arm_vmbus_ring_test.cpp
)

pcemu_program(arm-vmbus-channel-tests
  SOURCES
    src/arm_gpadl.cpp
    src/arm_vmbus_ring.cpp
    src/arm_vmbus.cpp
    src/arm_hyperv_timers.cpp
    src/arm_synic.cpp
    src/arm_hyperv_heartbeat.cpp
    src/arm_hyperv_timesync.cpp
    src/arm_hyperv_shutdown.cpp
    src/arm_hyperv_reference_clock.cpp
    src/arm_hyperv_kvp_codec.cpp
    src/arm_hyperv_kvp.cpp
    src/arm_hyperv.cpp
    tests/arm_vmbus_channel_test.cpp
)

pcemu_program(arm-hyperv-heartbeat-tests
  SOURCES
    ${ARM_DEVICES}
    tests/arm_hyperv_heartbeat_test.cpp
)

pcemu_program(arm-gic-ns-tests
  SOURCES
    src/arm_gic_ns.cpp
    tests/arm_gic_ns_test.cpp
)

pcemu_program(arm-hvf-gic-tests HVF
  SOURCES
    src/hvf_machine.cpp
    src/arm_gic_ns.cpp
    tests/arm_hvf_gic_test.cpp
)

pcemu_program(arm-hyperv-timesync-tests
  SOURCES
    src/arm_hyperv_timesync.cpp
    tests/arm_hyperv_timesync_test.cpp
)

pcemu_program(arm-hyperv-timesync-integration-tests
  SOURCES
    ${ARM_DEVICES}
    tests/arm_hyperv_timesync_integration_test.cpp
)

pcemu_program(arm-hyperv-shutdown-tests
  SOURCES
    src/arm_hyperv_shutdown.cpp
    tests/arm_hyperv_shutdown_test.cpp
)

pcemu_program(arm-hyperv-shutdown-integration-tests
  SOURCES
    ${ARM_DEVICES}
    tests/arm_hyperv_shutdown_integration_test.cpp
)

pcemu_program(arm-hyperv-reference-clock-tests
  SOURCES
    src/arm_hyperv_reference_clock.cpp
    tests/arm_hyperv_reference_clock_test.cpp
)

pcemu_program(arm-hvf-reference-counter-tests HVF
  SOURCES
    src/hvf_machine.cpp
    src/arm_gic_ns.cpp
    tests/arm_hvf_reference_counter_test.cpp
)

pcemu_program(arm-hvf-reference-page-tests HVF
  SOURCES
    src/hvf_machine.cpp
    src/arm_gic_ns.cpp
    src/arm_hyperv_reference_clock.cpp
    tests/arm_hvf_reference_page_test.cpp
)

pcemu_program(arm-hyperv-kvp-codec-tests
  SOURCES
    src/arm_hyperv_kvp_codec.cpp
    tests/arm_hyperv_kvp_codec_test.cpp
)

pcemu_program(arm-hyperv-kvp-tests
  SOURCES
    src/arm_hyperv_kvp_codec.cpp
    src/arm_hyperv_kvp.cpp
    tests/arm_hyperv_kvp_test.cpp
)

pcemu_program(arm-hyperv-kvp-integration-tests
  SOURCES
    ${ARM_DEVICES}
    tests/arm_hyperv_kvp_integration_test.cpp
)

pcemu_program(hyperv-kvp-roundtrip-tests
  SOURCES
    tests/hyperv_kvp_roundtrip_test.cpp
)

set_directory_properties(PROPERTIES VS_STARTUP_PROJECT pcemu)

pcemu_program(virtio-serial-tests ZSTD
  SOURCES
    src/disk.cpp
    src/virtio_blk.cpp
    src/virtio_pci.cpp
    src/virtio_device.cpp
    src/virtio_serial.cpp
    src/guest_agent_socket.cpp
    tests/virtio_serial_test.cpp
)

pcemu_program(virtio-serial-sanitized-tests SANITIZE ZSTD
  SOURCES
    src/disk.cpp
    src/virtio_blk.cpp
    src/virtio_pci.cpp
    src/virtio_device.cpp
    src/virtio_serial.cpp
    src/guest_agent_socket.cpp
    tests/virtio_serial_test.cpp
)

pcemu_program(virtio-balloon-tests ZSTD
  SOURCES src/disk.cpp src/virtio_blk.cpp src/virtio_pci.cpp
    src/virtio_device.cpp src/virtio_balloon.cpp tests/virtio_balloon_test.cpp
)
pcemu_program(virtio-balloon-sanitized-tests ZSTD SANITIZE
  SOURCES src/disk.cpp src/virtio_blk.cpp src/virtio_pci.cpp
    src/virtio_device.cpp src/virtio_balloon.cpp tests/virtio_balloon_test.cpp
)

pcemu_program(virtio-gpu-tests
  SOURCES src/virtio_device.cpp src/virtio_gpu.cpp tests/virtio_gpu_test.cpp
)
pcemu_program(virtio-gpu-sanitized-tests SANITIZE
  SOURCES src/virtio_device.cpp src/virtio_gpu.cpp tests/virtio_gpu_test.cpp
)

pkg_check_modules(SPICE_CLIENT QUIET IMPORTED_TARGET spice-client-glib-2.0)
if(SPICE_FOUND AND SPICE_CLIENT_FOUND)
  pcemu_program(spice-tests SOURCES src/frontend_spice.cpp src/host_keyboard.cpp
    src/virtio_device.cpp src/virtio_serial.cpp tests/spice_test.cpp)
  target_link_libraries(spice-tests PRIVATE PkgConfig::SPICE_CLIENT)
  target_link_libraries(obj_tests_spice_test_cpp_FALSE_FALSE PRIVATE PkgConfig::SPICE_CLIENT)
endif()
if(SPICE_CLIENT_FOUND)
  pcemu_program(spice-probe SOURCES tools/spice_probe.cpp)
  target_link_libraries(spice-probe PRIVATE PkgConfig::SPICE_CLIENT)
  target_link_libraries(obj_tools_spice_probe_cpp_FALSE_FALSE PRIVATE PkgConfig::SPICE_CLIENT)
endif()
if(SPICE_FOUND AND SPICE_CLIENT_FOUND)
  pcemu_program(spice-sanitized-tests SANITIZE SOURCES src/frontend_spice.cpp src/host_keyboard.cpp
    src/virtio_device.cpp src/virtio_serial.cpp tests/spice_test.cpp)
  target_link_libraries(spice-sanitized-tests PRIVATE PkgConfig::SPICE_CLIENT)
  target_link_libraries(obj_tests_spice_test_cpp_FALSE_TRUE PRIVATE PkgConfig::SPICE_CLIENT)
  set_tests_properties(spice-tests spice-sanitized-tests PROPERTIES
    ENVIRONMENT "GST_PLUGIN_SYSTEM_PATH_1_0=;GST_PLUGIN_PATH_1_0=" TIMEOUT 45)
endif()
