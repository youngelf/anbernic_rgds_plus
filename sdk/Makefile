# ==============================================================================
# Makefile - Anbernic RG DS Plus C SDK & Sample "Hello World" Application
# ==============================================================================
# Targets:
#   make               -> Cross-compiles `hello_rgds` (aarch64) + builds `sdcard_package/`
#   make linux         -> Compiles `hello_rgds_linux` (x86_64 X11 dual-screen simulator)
#   make run-linux     -> Builds and launches the x86_64 dual-screen simulator
#   make deploy-sd     -> Copies `sdcard_package/` to an inserted SD card
#   make deploy-ssh    -> Copies `sdcard_package/` over SSH to root@anbernic.local
#   make clean         -> Removes build artifacts and `sdcard_package/`
#
# To build your own single-file or multi-file C program with this SDK:
#   make APP_NAME=My_App APP_BIN=my_app APP_SRCS=examples/my_app.c
# ==============================================================================

APP_NAME ?= Hello_RGDS
APP_BIN  ?= hello_rgds
APP_SRCS ?= examples/hello_rgds.c

SDK_INC  = include
SDK_SRCS = src/rgds_sdk.c

# --- 1. Cross-Compiler Detection (Prefer ~/.cache/aarch64_toolchain gcc-14 if present) ---
AARCH64_ROOT ?= $(HOME)/.cache/aarch64_toolchain
CACHED_GCC   := $(wildcard $(AARCH64_ROOT)/usr/bin/aarch64-linux-gnu-gcc-14)

ifneq ($(CACHED_GCC),)
    AARCH64_PATH   = $(AARCH64_ROOT)/usr/bin:$(PATH)
    AARCH64_LD_LIB = $(AARCH64_ROOT)/usr/lib/x86_64-linux-gnu:$(AARCH64_ROOT)/usr/lib
    CC    = PATH="$(AARCH64_PATH)" LD_LIBRARY_PATH="$(AARCH64_LD_LIB)" aarch64-linux-gnu-gcc-14
    STRIP = PATH="$(AARCH64_PATH)" LD_LIBRARY_PATH="$(AARCH64_LD_LIB)" aarch64-linux-gnu-strip
    SYSROOT_FLAGS = --sysroot="$(AARCH64_ROOT)" \
        -isystem "$(AARCH64_ROOT)/usr/aarch64-linux-gnu/include" \
        -B"$(AARCH64_ROOT)/usr/aarch64-linux-gnu/lib" \
        -B"$(AARCH64_ROOT)/usr/lib/gcc-cross/aarch64-linux-gnu/14" \
        -L"$(AARCH64_ROOT)/usr/aarch64-linux-gnu/lib"
else
    CC    = aarch64-linux-gnu-gcc
    STRIP = aarch64-linux-gnu-strip
    SYSROOT_FLAGS =
endif

CFLAGS  = -O2 -Wall -Wextra -std=gnu99 -mcpu=cortex-a55 -D_GNU_SOURCE=1 -I$(SDK_INC) $(SYSROOT_FLAGS)
LDFLAGS = -lm -lpthread -ldl

# --- 2. Host Linux Desktop Simulator (x86_64 X11) ---
HOST_CC      ?= gcc
HOST_CFLAGS  = -O2 -Wall -Wextra -std=gnu99 -DHOST_X11 -D_GNU_SOURCE=1 -I$(SDK_INC)
HOST_LDFLAGS = -lX11 -lm -lpthread -ldl

HOST_BIN = $(APP_BIN)_linux

.PHONY: all rgds linux host run-linux package deploy-sd deploy-ssh setup-host clean

all: $(APP_BIN) package

rgds: $(APP_BIN)

linux: $(HOST_BIN)

host: $(HOST_BIN)

$(APP_BIN): $(APP_SRCS) $(SDK_SRCS) $(SDK_INC)/rgds_sdk.h
	$(CC) $(CFLAGS) -o $(APP_BIN) $(APP_SRCS) $(SDK_SRCS) $(LDFLAGS)
	$(STRIP) $(APP_BIN)

$(HOST_BIN): $(APP_SRCS) $(SDK_SRCS) $(SDK_INC)/rgds_sdk.h
	$(HOST_CC) $(HOST_CFLAGS) -o $(HOST_BIN) $(APP_SRCS) $(SDK_SRCS) $(HOST_LDFLAGS)

run-linux: $(HOST_BIN)
	./$(HOST_BIN)

package: $(APP_BIN)
	rm -rf sdcard_package
	mkdir -p sdcard_package/Roms/APPS/$(APP_BIN)
	mkdir -p sdcard_package/Roms/APPS/Imgs
	mkdir -p sdcard_package/Ports/$(APP_NAME)
	cp $(APP_BIN) assets/icon.png sdcard_package/Roms/APPS/$(APP_BIN)/
	cp assets/icon.png sdcard_package/Roms/APPS/Imgs/$(APP_NAME).png
	sed "s/__APP_BIN__/$(APP_BIN)/g" scripts/App_Template.sh > sdcard_package/Roms/APPS/$(APP_NAME).sh
	cp $(APP_BIN) assets/icon.png sdcard_package/Ports/$(APP_NAME)/
	sed "s/__APP_BIN__/$(APP_BIN)/g" scripts/App_Template.sh > sdcard_package/Ports/$(APP_NAME)/launch.sh
	chmod +x sdcard_package/Roms/APPS/$(APP_NAME).sh sdcard_package/Roms/APPS/$(APP_BIN)/$(APP_BIN)
	chmod +x sdcard_package/Ports/$(APP_NAME)/launch.sh sdcard_package/Ports/$(APP_NAME)/$(APP_BIN)

deploy-sd: package
	./scripts/deploy_to_sdcard.sh $(SDCARD_PATH)

deploy-ssh: package
	./scripts/deploy_over_ssh.sh $(RGDS_HOST)

setup-host:
	./scripts/setup_host.sh

clean:
	rm -f $(APP_BIN) $(HOST_BIN)
	rm -rf sdcard_package
