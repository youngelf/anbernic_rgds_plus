# Makefile for Anbernic RG DS Plus Custom Boot Logo Changer (SDK-based)

SDK_DIR    := ../sdk
SDK_INC    := $(SDK_DIR)/include
SDK_SRC    := $(SDK_DIR)/src/rgds_sdk.c

TOOLCHAIN_ROOT := $(HOME)/.cache/aarch64_toolchain
ifneq ($(wildcard $(TOOLCHAIN_ROOT)/usr/bin/aarch64-linux-gnu-gcc-14),)
  export PATH := $(TOOLCHAIN_ROOT)/usr/bin:$(PATH)
  export LD_LIBRARY_PATH := $(TOOLCHAIN_ROOT)/usr/lib/x86_64-linux-gnu:$(TOOLCHAIN_ROOT)/usr/lib
  CC      := aarch64-linux-gnu-gcc-14
  STRIP   := aarch64-linux-gnu-strip
  SYSROOT_FLAGS := --sysroot=$(TOOLCHAIN_ROOT) \
    -isystem $(TOOLCHAIN_ROOT)/usr/aarch64-linux-gnu/include \
    -B$(TOOLCHAIN_ROOT)/usr/aarch64-linux-gnu/lib \
    -B$(TOOLCHAIN_ROOT)/usr/lib/gcc-cross/aarch64-linux-gnu/14 \
    -L$(TOOLCHAIN_ROOT)/usr/aarch64-linux-gnu/lib
else
  CC      := aarch64-linux-gnu-gcc
  STRIP   := aarch64-linux-gnu-strip
  SYSROOT_FLAGS :=
endif

CFLAGS  := -O2 -Wall -Wextra -std=gnu99 -mcpu=cortex-a55 -D_GNU_SOURCE=1 -I$(SDK_INC) $(SYSROOT_FLAGS)
LDFLAGS := -lm -lpthread -ldl

HOST_CC      := gcc
HOST_CFLAGS  := -O2 -Wall -Wextra -std=gnu99 -DHOST_X11 -D_GNU_SOURCE=1 -I$(SDK_INC)
HOST_LDFLAGS := -lX11 -lm -lpthread -ldl

TARGET       := boot_logo
LINUX_TARGET := boot_logo_linux
PKG_DIR      := sdcard_package

.PHONY: all clean package linux run-linux

all: $(TARGET) package

$(TARGET): boot_logo.c $(SDK_SRC) $(SDK_INC)/rgds_sdk.h
	$(CC) $(CFLAGS) -o $@ boot_logo.c $(SDK_SRC) $(LDFLAGS)
	$(STRIP) $@

linux: $(LINUX_TARGET)

$(LINUX_TARGET): boot_logo.c $(SDK_SRC) $(SDK_INC)/rgds_sdk.h
	$(HOST_CC) $(HOST_CFLAGS) -o $@ boot_logo.c $(SDK_SRC) $(HOST_LDFLAGS)

run-linux: $(LINUX_TARGET)
	./$(LINUX_TARGET)

package: $(TARGET)
	rm -rf $(PKG_DIR)
	mkdir -p $(PKG_DIR)/Roms/APPS/boot_logo/logos
	mkdir -p $(PKG_DIR)/Roms/APPS/Imgs
	mkdir -p $(PKG_DIR)/Ports/Boot_Logo/logos
	cp $(TARGET) boot_logo.png $(PKG_DIR)/Roms/APPS/boot_logo/
	cp -r logos/* $(PKG_DIR)/Roms/APPS/boot_logo/logos/
	cp boot_logo.png $(PKG_DIR)/Roms/APPS/Imgs/Boot_Logo.png
	cp Boot_Logo.sh $(PKG_DIR)/Roms/APPS/Boot_Logo.sh
	cp $(TARGET) boot_logo.png $(PKG_DIR)/Ports/Boot_Logo/
	cp -r logos/* $(PKG_DIR)/Ports/Boot_Logo/logos/
	cp boot_logo.png $(PKG_DIR)/Ports/Boot_Logo/icon.png
	cp Boot_Logo.sh $(PKG_DIR)/Ports/Boot_Logo/launch.sh
	chmod +x $(PKG_DIR)/Roms/APPS/Boot_Logo.sh $(PKG_DIR)/Roms/APPS/boot_logo/$(TARGET)
	chmod +x $(PKG_DIR)/Ports/Boot_Logo/launch.sh $(PKG_DIR)/Ports/Boot_Logo/$(TARGET)

clean:
	rm -f $(TARGET) $(LINUX_TARGET)
	rm -rf $(PKG_DIR)
