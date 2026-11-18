# BamBuddy for FreeToon: daemon + LVGL UI plugin.
#
#   make TARGET=toon2            bambuddy.toon2 + bambuddy_ui.toon2.so (Linaro armhf)
#   make TARGET=toon1            bambuddy.toon1 + bambuddy_ui.toon1.so (Toon 1 toolchain)
#   make package                 bambuddy.tgz with the builds that exist (files at the archive root)
#   make TARGET=toon2 abi-check  nothing may need glibc newer than 2.21
#
# FREETOON_SRC is a freetoon-lvgl checkout; the plugin compiles against its
# LVGL, lv_conf.h, settings.h and theme.h (THEME_* colours).

APP          := bambuddy
EXTRA_PKG    := relay/bambuddy-relay.sh
FREETOON_SRC ?= ../../../freetoon-lvgl
UI_SRC       := $(FREETOON_SRC)/lvgl_ui_recovered/src
TC_ROOT      ?= $(HOME)/toolchains
TARGET       ?= toon2

ifeq ($(TARGET),toon1)
  CROSS       := $(TC_ROOT)/toon1-toolchain/bin/arm-linux-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/toon1-toolchain/arm-buildroot-linux-gnueabi/sysroot
  ARCHFLAGS   := -march=armv5te -marm -mfloat-abi=soft -DTOON1
else ifeq ($(TARGET),toon2)
  CROSS       := $(TC_ROOT)/linaro/bin/arm-linux-gnueabihf-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/linaro/arm-linux-gnueabihf/libc
  ARCHFLAGS   :=
else
  $(error Unknown TARGET '$(TARGET)' - use toon2 or toon1)
endif

CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip
OBJDUMP := $(CROSS)objdump
CFLAGS  := $(SYSROOTFLAG) $(ARCHFLAGS) -std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
SO_INC  := -I. -I$(UI_SRC) -I$(UI_SRC)/lvgl -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE

BIN := $(APP).$(TARGET)
SO  := $(APP)_ui.$(TARGET).so

.PHONY: all package abi-check clean
all: $(BIN) $(SO)

$(BIN): $(APP)_daemon.c ftapp.h ftjson.h
	$(CC) $(CFLAGS) -static-libgcc $(APP)_daemon.c -o $@ -lm
	$(STRIP) $@

# toonui dlopen()s this with RTLD_GLOBAL and provides every lv_* symbol and
# `settings`; only screen_create and freetoon_plugin_abi are exported.
$(SO): screen_$(APP).c ftui.h ftjson.h
	$(CC) $(CFLAGS) -shared -fPIC -fvisibility=hidden -static-libgcc $(SO_INC) screen_$(APP).c -o $@ -lm
	$(STRIP) --strip-unneeded $@

$(APP): launcher.sh
	cp launcher.sh $@
	chmod 755 $@

package: $(APP)
	chmod 755 $(APP) $(wildcard $(APP).toon1 $(APP).toon2)
	tar --owner=0 --group=0 --numeric-owner --mode=go-w -czf $(APP).tgz manifest.json README.md $(APP) \
	    $(wildcard $(APP).toon1 $(APP).toon2 $(APP)_ui.toon1.so $(APP)_ui.toon2.so) $(EXTRA_PKG)
	tar tzvf $(APP).tgz

abi-check: $(BIN) $(SO)
	@for f in $(BIN) $(SO); do \
	  bad=$$($(OBJDUMP) -T $$f | grep -oE 'GLIBC_2\.[0-9]+' | sort -uV | awk -F. '$$2+0 > 21'); \
	  if [ -n "$$bad" ]; then echo "$$f needs glibc newer than 2.21: $$bad"; exit 1; fi; \
	  echo "$$f: OK, highest $$($(OBJDUMP) -T $$f | grep -oE 'GLIBC_2\.[0-9]+' | sort -uV | tail -1)"; \
	done

clean:
	rm -f $(APP).toon1 $(APP).toon2 $(APP)_ui.toon1.so $(APP)_ui.toon2.so $(APP) $(APP).tgz
