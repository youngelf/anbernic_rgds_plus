# Makefile for Anbernic RG DS Plus Dual-Screen Night Clock (1024x768x2 aarch64 & x86_64 Host)

# --- Anbernic RG DS Plus (Rockchip RK3568 Cortex-A55 aarch64) Configuration ---
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

SDK_DIR  = ../sdk
SDK_INC  = $(SDK_DIR)/include
SDK_SRCS = $(SDK_DIR)/src/rgds_sdk.c

CFLAGS  = -O2 -Wall -Wextra -std=gnu99 -mcpu=cortex-a55 -D_GNU_SOURCE=1 -I$(SDK_INC) $(SYSROOT_FLAGS)
LDFLAGS = -lm -lpthread -ldl

# --- Host Linux (x86_64 X11 Dual-Screen Window + PulseAudio/ALSA dlopen) Configuration ---
HOST_CC      ?= gcc
HOST_CFLAGS  = -O2 -Wall -Wextra -std=gnu99 -DHOST_X11 -D_GNU_SOURCE=1 -I$(SDK_INC)
HOST_LDFLAGS = -lX11 -lm -lpthread -ldl

# --- Targets ---
APP_NAME    = Night_Clock
TARGET      = night_clock
HOST_TARGET = night_clock_linux
SRCS        = night_clock.c

.PHONY: all rgds linux host run-linux package clean

# Default builds the Anbernic RG DS Plus (aarch64) binary and SD card package
all: $(TARGET) package

rgds: $(TARGET)

# Build x86_64 Linux binary for local dual-screen testing/debugging
linux: $(HOST_TARGET)

host: $(HOST_TARGET)

$(TARGET): $(SRCS) $(SDK_SRCS) $(SDK_INC)/rgds_sdk.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(SDK_SRCS) $(LDFLAGS)
	$(STRIP) $(TARGET)

$(HOST_TARGET): $(SRCS) $(SDK_SRCS) $(SDK_INC)/rgds_sdk.h
	$(HOST_CC) $(HOST_CFLAGS) -o $(HOST_TARGET) $(SRCS) $(SDK_SRCS) $(HOST_LDFLAGS)

run-linux: $(HOST_TARGET)
	./$(HOST_TARGET)

package: $(TARGET) night_clock.png Night_Clock.sh
	rm -rf sdcard_package
	mkdir -p sdcard_package/Roms/APPS/$(TARGET)
	mkdir -p sdcard_package/Roms/APPS/Imgs
	mkdir -p sdcard_package/Ports/$(APP_NAME)
	cp $(TARGET) night_clock.png sdcard_package/Roms/APPS/$(TARGET)/
	cp night_clock.png sdcard_package/Roms/APPS/Imgs/$(APP_NAME).png
	cp Night_Clock.sh sdcard_package/Roms/APPS/$(APP_NAME).sh
	cp $(TARGET) night_clock.png sdcard_package/Ports/$(APP_NAME)/
	cp night_clock.png sdcard_package/Ports/$(APP_NAME)/icon.png
	cp Night_Clock.sh sdcard_package/Ports/$(APP_NAME)/launch.sh
	chmod +x sdcard_package/Roms/APPS/$(APP_NAME).sh sdcard_package/Roms/APPS/$(TARGET)/$(TARGET)
	chmod +x sdcard_package/Ports/$(APP_NAME)/launch.sh sdcard_package/Ports/$(APP_NAME)/$(TARGET)

clean:
	rm -f $(TARGET) $(HOST_TARGET)
	rm -rf sdcard_package
