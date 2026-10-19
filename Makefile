# Compatible with the internal CMake 3.15.2 toolchain and newer WSL versions.
# Interactive aliases are not inherited by make's non-interactive shell.
INTERNAL_CMAKE := /home/xshare/scripts/bin/cmake-3.15.2/bin/cmake
CMAKE ?= $(if $(wildcard $(INTERNAL_CMAKE)),$(INTERNAL_CMAKE),cmake)
PARALLEL_JOBS ?= 32
BUILD_DIR ?= build
BUILD_TYPE ?= Release
GUI ?= AUTO
CMAKE_ARGS ?=
SOURCE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_PATH := $(abspath $(BUILD_DIR))

.DEFAULT_GOAL := all
# Never clean/configure the same tree concurrently, even with make -j clean all.
.NOTPARALLEL:
.PHONY: all release debug clean help

all:
	@"$(CMAKE)" -S "$(SOURCE_DIR)" -B "$(BUILD_PATH)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DGCRP_BUILD_QT_GUI=$(GUI) $(CMAKE_ARGS)
	@"$(CMAKE)" --build "$(BUILD_PATH)" --parallel $(PARALLEL_JOBS)

release: BUILD_TYPE = Release
release: all

debug: BUILD_TYPE = Debug
debug: all

# Keep the cache; do not recursively delete arbitrary user-supplied paths.
clean:
	@if [ -f "$(BUILD_PATH)/CMakeCache.txt" ]; then \
		"$(CMAKE)" --build "$(BUILD_PATH)" --target clean; \
	else \
		printf '%s\n' 'Nothing to clean: $(BUILD_PATH) is not configured.'; \
	fi

help:
	@echo "Targets: make / make release / make debug / make clean"
	@echo "clean removes generated targets, retaining the cache and source files."
	@echo "CMAKE: internal 3.15.2 path if present, otherwise cmake from PATH"
	@echo "BUILD_DIR: build; BUILD_TYPE: Release; PARALLEL_JOBS: 32"
	@echo "GUI: AUTO (build if Qt5 is installed); make GUI=ON requires Qt5; GUI=OFF skips GUI"
	@echo "CMAKE_ARGS: extra configure arguments"
