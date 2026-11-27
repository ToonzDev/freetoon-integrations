# Matter for FreeToon: daemon + LVGL UI plugin.
#
#   make TARGET=toon2            daemon freetoon-app-matter.toon2 + matter_ui.toon2.so (Linaro armhf)
#   make TARGET=toon1            the same for Toon 1 (ARMv5; the bridge itself needs a Toon 2)
#   make abi-check TARGET=...    nothing may need a newer glibc than the Toon has
#   make test                    host unit test of the shared logic (cc on the Mac/PC)
#   make preview                 host render of the screen into preview/*.png (needs FREETOON_SRC)
#
# The cross builds run in the freetoon-build container (see README.md); the
# FreeToon tree supplies the LVGL/toonui headers the plugin compiles against.

FREETOON_SRC ?= ../../../freetoon-lvgl
UI_SRC       := $(FREETOON_SRC)/lvgl_ui_recovered/src
TC_ROOT      ?= $(HOME)/toolchains
TARGET       ?= toon2

ifeq ($(TARGET),toon1)
  CROSS       := $(TC_ROOT)/toon1-toolchain/bin/arm-linux-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/toon1-toolchain/arm-buildroot-linux-gnueabi/sysroot
  ARCHFLAGS   := -march=armv5te -marm -mfloat-abi=soft -DTOON1
  GLIBC_MAX   := 18
else ifeq ($(TARGET),toon2)
  CROSS       := $(TC_ROOT)/linaro/bin/arm-linux-gnueabihf-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/linaro/arm-linux-gnueabihf/libc
  ARCHFLAGS   :=
  GLIBC_MAX   := 21
else
  $(error Unknown TARGET '$(TARGET)' - use toon2 or toon1)
endif

CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip
OBJDUMP := $(CROSS)objdump
CFLAGS  := $(SYSROOTFLAG) $(ARCHFLAGS) -std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter
SO_INC  := -I. -I$(UI_SRC) -I$(UI_SRC)/lvgl -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE

OUT     ?= freetoon-app-matter.$(TARGET)
SO_OUT  := matter_ui.$(TARGET).so
LOGIC   := matter_logic.c matter_json.c
HDRS    := matter_logic.h matter_json.h

.PHONY: all abi-check test preview clean
all: $(OUT) $(SO_OUT)

$(OUT): matter_daemon.c $(LOGIC) $(HDRS)
	$(CC) $(CFLAGS) -static-libgcc matter_daemon.c $(LOGIC) -o $@ -lm
	$(STRIP) $@

# toonui dlopen()s this with RTLD_GLOBAL and provides every lv_* symbol
# (lv_qrcode included) and `settings`; only screen_create and
# freetoon_plugin_abi are exported.
$(SO_OUT): screen_matter.c $(LOGIC) $(HDRS)
	$(CC) $(CFLAGS) -shared -fPIC -fvisibility=hidden -static-libgcc $(SO_INC) \
	      screen_matter.c $(LOGIC) -o $@ -lm
	$(STRIP) --strip-unneeded $@

abi-check: $(OUT) $(SO_OUT)
	@for f in $(OUT) $(SO_OUT); do \
	  bad=$$($(OBJDUMP) -T $$f | grep -oE 'GLIBC_2\.[0-9]+' | sort -uV | awk -F. '$$2+0 > $(GLIBC_MAX)'); \
	  if [ -n "$$bad" ]; then echo "$$f needs glibc newer than 2.$(GLIBC_MAX): $$bad"; exit 1; fi; \
	  echo "$$f: OK, highest $$($(OBJDUMP) -T $$f | grep -oE 'GLIBC_2\.[0-9]+' | sort -uV | tail -1)"; \
	done

# ---- host --------------------------------------------------------------
HOSTCC ?= cc
test:
	$(HOSTCC) -std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -Wall -Wextra -g tests/test_logic.c $(LOGIC) \
	    -o tests/test_logic -lm
	./tests/test_logic

preview:
	$(MAKE) -f tools/preview.mk FREETOON_SRC=$(FREETOON_SRC)
	sh tools/shots.sh

clean:
	rm -f freetoon-app-matter.toon1 freetoon-app-matter.toon2 matter_ui.toon1.so matter_ui.toon2.so \
	      tests/test_logic
	rm -rf tests/test_logic.dSYM preview/build
