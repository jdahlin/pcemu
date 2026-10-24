CXX := clang++
CPPFLAGS := -Isrc $(shell /opt/homebrew/bin/sdl2-config --cflags) $(shell pkg-config --cflags libzstd)
CXXFLAGS := -std=c++20 -O3 -g -Wall -Wextra -Wpedantic
LDLIBS := $(shell /opt/homebrew/bin/sdl2-config --libs) $(shell pkg-config --libs libzstd)
HEADERS := $(wildcard src/*.hpp)
CORE := src/cpu.cpp src/machine.cpp src/disk.cpp src/ata.cpp src/pic.cpp src/pit.cpp src/vga.cpp src/jit.cpp src/snapshot.cpp src/mouse.cpp src/pci.cpp src/host_keyboard.cpp src/ne2000.cpp
.PHONY: firmware all test clean run dos bench app app95 smoke-win311 smoke-reset
all: build/pcemu
build/pcemu-profile: $(CORE) $(HEADERS) src/network.cpp src/main.cpp src/profile.hpp src/cpu.hpp src/machine.hpp src/jit.hpp src/vga.hpp src/ata.hpp src/pic.hpp src/pit.hpp
	$(CXX) $(CPPFLAGS) -DPCEMU_PROFILE $(CXXFLAGS) $(CORE) src/main.cpp src/network.cpp $(LDLIBS) -o $@
build/disk-tool: $(CORE) $(HEADERS) tools/disk_tool.cpp src/machine.hpp src/cpu.hpp src/profile.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/disk_tool.cpp $(shell pkg-config --libs libzstd) -o $@
build/pcemu: $(CORE) $(HEADERS) src/network.cpp src/main.cpp src/cpu.hpp src/machine.hpp src/jit.hpp src/vga.hpp src/ata.hpp src/pic.hpp src/pit.hpp src/profile.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) src/main.cpp src/network.cpp $(LDLIBS) -o $@
build/tests: $(CORE) $(HEADERS) tests/test.cpp src/cpu.hpp src/machine.hpp src/jit.hpp src/vga.hpp src/ata.hpp src/pic.hpp src/pit.hpp src/profile.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/test.cpp $(shell pkg-config --libs libzstd) -o $@
build/snapshot-tests: $(CORE) $(HEADERS) tests/snapshot_test.cpp src/cpu.hpp src/machine.hpp src/jit.hpp src/ata.hpp src/vga.hpp src/pic.hpp src/pit.hpp src/profile.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/snapshot_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/jit-byte-tests: $(CORE) $(HEADERS) tests/jit_bytes_test.cpp src/cpu.hpp src/machine.hpp src/jit.hpp src/ata.hpp src/vga.hpp src/pic.hpp src/pit.hpp src/profile.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/jit_bytes_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/jit-stack-tests: $(CORE) $(HEADERS) tests/jit_stack_test.cpp src/cpu.hpp src/machine.hpp src/jit.hpp src/ata.hpp src/vga.hpp src/pic.hpp src/pit.hpp src/profile.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/jit_stack_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/integration-tests: $(CORE) $(HEADERS) tests/integration_test.cpp src/machine.hpp src/mouse.hpp src/vga.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/integration_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/network-tests: $(CORE) $(HEADERS) src/network.cpp tests/network_test.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) src/network.cpp tests/network_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/network-tests-sanitize: $(CORE) $(HEADERS) src/network.cpp tests/network_test.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(CORE) src/network.cpp tests/network_test.cpp $(shell pkg-config --libs libzstd) -o $@
build/jit-read-tests: $(CORE) $(HEADERS) tests/jit_read_test.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/jit_read_test.cpp $(shell pkg-config --libs libzstd) -o $@
test: build/jit-read-tests build/network-tests build/integration-tests build/tests build/snapshot-tests build/jit-byte-tests build/jit-stack-tests
	./build/jit-read-tests
	./build/network-tests
	./build/integration-tests
	./build/tests
	./build/snapshot-tests
	./build/jit-byte-tests
	./build/jit-stack-tests
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
