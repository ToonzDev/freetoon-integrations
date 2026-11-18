# Radiatoren for FreeToon: daemon + LVGL UI plugin.
#
#   make TARGET=toon2            daemon radiatoren.toon2 + radiatoren_ui.toon2.so (Linaro armhf)
#   make TARGET=toon2 package    ... and radiatoren.tgz (files at the archive root)
#   make TARGET=toon2 abi-check  nothing may need glibc newer than 2.21
#   make test                    host unit test of the logic (cc on the Mac)
#   make preview                 host render of the screens to preview/*.png (Mac, clang)
#
# The cross builds run in the freetoon-build container (see README.md); the
# FreeToon tree supplies the LVGL/toonui headers the plugin compiles against.

FREETOON_SRC ?= ../../../freetoon-lvgl
UI_SRC       := $(FREETOON_SRC)/lvgl_ui_recovered/src
TC_ROOT      ?= $(HOME)/toolchains
TARGET       ?= toon2

ifeq ($(TARGET),toon1)
  # Not built: the Toon 1 toolchain is not installed here. Kept for symmetry
  # with the other integrations.
  CROSS       := $(TC_ROOT)/toon1-toolchain/bin/arm-linux-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/toon1-toolchain/arm-buildroot-linux-gnueabi/sysroot
  ARCHFLAGS   := -march=armv5te -marm -mfloat-abi=soft -DTOON1
else ifeq ($(TARGET),toon2)
  CROSS       := $(TC_ROOT)/linaro/bin/arm-linux-gnueabihf-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/linaro/arm-linux-gnueabihf/libc
  ARCHFLAGS   :=
else
  $(error Unknown TARGET '$(TARGET)' - use toon2 (or toon1))
endif

CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip
OBJDUMP := $(CROSS)objdump
CFLAGS  := $(SYSROOTFLAG) $(ARCHFLAGS) -std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter
SO_INC  := -I. -I$(UI_SRC) -I$(UI_SRC)/lvgl -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE

BIN     := radiatoren.$(TARGET)
SO      := radiatoren_ui.$(TARGET).so
LOGIC   := radiatoren_logic.c radiatoren_logic.h

.PHONY: all package abi-check test preview clean
all: $(BIN) $(SO)

$(BIN): radiatoren_daemon.c $(LOGIC)
	$(CC) $(CFLAGS) -static-libgcc radiatoren_daemon.c radiatoren_logic.c -o $@ -lm
	$(STRIP) $@

# toonui dlopen()s this with RTLD_GLOBAL and provides every lv_* symbol and
# `settings`; only screen_create and freetoon_plugin_abi are exported.
$(SO): screen_radiatoren.c $(LOGIC)
	$(CC) $(CFLAGS) -shared -fPIC -fvisibility=hidden -static-libgcc $(SO_INC) \
	      screen_radiatoren.c radiatoren_logic.c -o $@ -lm
	$(STRIP) --strip-unneeded $@

# Launcher named in manifest.json "binary": picks the build for this Toon.
radiatoren: launcher.sh
	cp launcher.sh $@
	chmod 755 $@

package: all radiatoren
	chmod 755 $(BIN) radiatoren
	tar --owner=0 --group=0 --numeric-owner --mode=go-w -czf radiatoren.tgz \
	    manifest.json radiatoren $(BIN) $(SO) README.md
	tar tzvf radiatoren.tgz

abi-check: $(BIN) $(SO)
	@for f in $(BIN) $(SO); do \
	  bad=$$($(OBJDUMP) -T $$f | grep -oE 'GLIBC_2\.[0-9]+' | sort -uV | awk -F. '$$2+0 > 21'); \
	  if [ -n "$$bad" ]; then echo "$$f needs glibc newer than 2.21: $$bad"; exit 1; fi; \
	  echo "$$f: OK, highest $$($(OBJDUMP) -T $$f | grep -oE 'GLIBC_2\.[0-9]+' | sort -uV | tail -1)"; \
	done

# ---- host --------------------------------------------------------------
HOSTCC ?= cc
test:
	$(HOSTCC) -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -g tests/test_logic.c radiatoren_logic.c \
	    -o tests/test_logic -lm
	./tests/test_logic

# Renders the plugin with the real LVGL + fonts from the FreeToon tree into
# preview/*.png (light and dark, overview and editor). See tools/render.c.
preview:
	$(MAKE) -f tools/preview.mk FREETOON_SRC=$(FREETOON_SRC)

clean:
	rm -f radiatoren.toon1 radiatoren.toon2 radiatoren_ui.toon1.so radiatoren_ui.toon2.so radiatoren \
	      radiatoren.tgz tests/test_logic
