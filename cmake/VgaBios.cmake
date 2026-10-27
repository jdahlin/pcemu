# Standalone guest firmware; NASM and Python are needed only for these targets.
find_program(PCEMU_VGA_NASM nasm)
find_package(Python3 QUIET COMPONENTS Interpreter)
if(PCEMU_VGA_NASM AND Python3_Interpreter_FOUND)
  file(GLOB VGA_FONT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/firmware/vga/vendor/font8x8/*")
  file(GLOB VGA_SERVICE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/firmware/vga/*.inc")
  set(VGA_ROM "${CMAKE_BINARY_DIR}/firmware/pcemu-vga.bin")
  add_custom_command(OUTPUT "${VGA_ROM}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/build_vga_bios.py"
      --output "${VGA_ROM}"
    DEPENDS firmware/vga/vga.asm tools/build_vga_bios.py tools/vga_modes.py ${VGA_FONT_SOURCES} ${VGA_SERVICE_SOURCES}
    VERBATIM)
  add_custom_target(vga-bios DEPENDS "${VGA_ROM}")
  pcemu_program(vga-bios-tests ZSTD SOURCES ${CORE} src/pc_display.cpp tests/vga_bios_test.cpp)
  add_dependencies(vga-bios-tests vga-bios)
  target_compile_definitions(obj_tests_vga_bios_test_cpp_FALSE_FALSE PRIVATE
    PCEMU_VGA_TEST_ROM="${VGA_ROM}")
  add_test(NAME vga-bios-build COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${CMAKE_SOURCE_DIR}/tests" -p test_vga_bios_build.py)
  set_tests_properties(vga-bios-tests vga-bios-build PROPERTIES TIMEOUT 30)
endif()
if(TARGET vga-bios)
  add_custom_target(vga-bios-smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/smoke_vga_bios.py"
      --runner "$<TARGET_FILE:pcemu>" --rom "${VGA_ROM}"
    DEPENDS pcemu vga-bios USES_TERMINAL VERBATIM)
endif()
