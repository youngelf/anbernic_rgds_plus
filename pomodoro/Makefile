# ==============================================================================
# Makefile - Dual-Screen Pomodoro Timer for Anbernic RG DS Plus (1024x768 x 2)
#            Built with the RG DS Plus C SDK (../rg_ds_plus_sdk)
# ==============================================================================
# Targets:
#   make               -> Cross-compiles `pomodoro` (aarch64) + builds `sdcard_package/`
#   make linux         -> Compiles `pomodoro_linux` (x86_64 X11 dual-screen simulator)
#   make run-linux     -> Builds and launches the x86_64 dual-screen simulator
#   make package       -> Populates `sdcard_package/` for Roms/APPS and Ports
#   make deploy-sd     -> Copies `sdcard_package/` to an inserted microSD card
#   make deploy-ssh    -> Pushes `sdcard_package/` over Wi-Fi SSH to root@anbernic.local
#   make deploy-img    -> Injects `sdcard_package/` into an unmounted .IMG firmware file
#   make setup-host    -> Installs required Linux host dependencies via setup_host.sh
#   make clean         -> Removes build artifacts and `sdcard_package/`
# ==============================================================================

APP_NAME := Pomodoro
APP_BIN  := pomodoro
APP_SRCS := pomodoro.c

# Locate the RG DS Plus SDK
ifneq ($(wildcard ../rg_ds_plus_sdk/include/rgds_sdk.h),)
    SDK_DIR ?= ../rg_ds_plus_sdk
else
    SDK_DIR ?= $(HOME)/xkdr/rg_ds_plus_sdk
endif

SDK_INC  := $(SDK_DIR)/include
SDK_SRCS := $(SDK_DIR)/src/rgds_sdk.c

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

# --- 2. Host Linux Desktop Simulator (x86_64 X11 Dual-Screen Window) ---
HOST_CC      ?= gcc
HOST_CFLAGS  = -O2 -g -Wall -Wextra -std=gnu99 -DHOST_X11 -D_GNU_SOURCE=1 -I$(SDK_INC)
HOST_LDFLAGS = -lX11 -lm -lpthread -ldl

HOST_BIN := $(APP_BIN)_linux

.PHONY: all rgds linux host run-linux package deploy-sd deploy-ssh deploy-img setup-host clean

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

package: $(APP_BIN) Pomodoro.sh pomodoro.png
	rm -rf sdcard_package
	mkdir -p sdcard_package/Roms/APPS/$(APP_BIN)
	mkdir -p sdcard_package/Roms/APPS/Imgs
	mkdir -p sdcard_package/Ports/$(APP_NAME)
	cp $(APP_BIN) pomodoro.png sdcard_package/Roms/APPS/$(APP_BIN)/
	cp pomodoro.png sdcard_package/Roms/APPS/Imgs/$(APP_NAME).png
	cp Pomodoro.sh sdcard_package/Roms/APPS/$(APP_NAME).sh
	cp $(APP_BIN) pomodoro.png sdcard_package/Ports/$(APP_NAME)/
	cp pomodoro.png sdcard_package/Ports/$(APP_NAME)/icon.png
	cp Pomodoro.sh sdcard_package/Ports/$(APP_NAME)/launch.sh
	chmod +x sdcard_package/Roms/APPS/$(APP_NAME).sh sdcard_package/Roms/APPS/$(APP_BIN)/$(APP_BIN)
	chmod +x sdcard_package/Ports/$(APP_NAME)/launch.sh sdcard_package/Ports/$(APP_NAME)/$(APP_BIN)

deploy-sd: package
	$(SDK_DIR)/scripts/deploy_to_sdcard.sh $(SDCARD_PATH)

deploy-ssh: package
	$(SDK_DIR)/scripts/deploy_over_ssh.sh $(RGDS_HOST)

deploy-img: package
	$(SDK_DIR)/scripts/deploy_to_img.sh $(IMG_PATH)

setup-host:
	$(SDK_DIR)/scripts/setup_host.sh

clean:
	rm -f $(APP_BIN) $(HOST_BIN)
	rm -rf sdcard_package
