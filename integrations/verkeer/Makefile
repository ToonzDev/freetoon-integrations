# Build freetoon-app-verkeer and verkeer_ui.so for Toon 1 and Toon 2;
# `make host` builds the daemon for the build host, `make sim` the UI plugin
# for the FreeToon simulator (tests).
TC_ROOT ?= $(HOME)/toolchains

TARGET ?= toon2

ifeq ($(TARGET),toon1)
  CROSS       := $(TC_ROOT)/toon1-toolchain/bin/arm-linux-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/toon1-toolchain/arm-buildroot-linux-gnueabi/sysroot
  ARCHFLAGS   := -march=armv5te -marm -mfloat-abi=soft -DTOON1
  OUT         ?= freetoon-app-verkeer.toon1
  SO_OUT      := verkeer_ui.toon1.so
  # drop the toolchain's "for GNU/Linux 4.1.0" note, as toonui does (Toon 1 runs kernel 2.6)
  POST        := $(CROSS)objcopy --remove-section .note.ABI-tag
else
  CROSS       := $(TC_ROOT)/linaro/bin/arm-linux-gnueabihf-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/linaro/arm-linux-gnueabihf/libc
  ARCHFLAGS   :=
  OUT         ?= freetoon-app-verkeer.toon2
  SO_OUT      := verkeer_ui.toon2.so
endif

CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip
CFLAGS  := $(SYSROOTFLAG) $(ARCHFLAGS) -O2 -Wall -Wextra
CORE    := ../../../freetoon-lvgl/lvgl_ui_recovered/src
SO_INC  := -I. -I$(CORE) -I$(CORE)/lvgl -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE

all: $(OUT) $(SO_OUT)

$(OUT): verkeer_daemon.c
	$(CC) $(CFLAGS) verkeer_daemon.c -o $(OUT) -lm
	$(if $(POST),$(POST) $(OUT))
	$(STRIP) $(OUT)

$(SO_OUT): screen_verkeer.c
	$(CC) $(CFLAGS) $(if $(filter toon1,$(TARGET)),-DTOON1,) -shared -fPIC $(SO_INC) screen_verkeer.c -o $(SO_OUT)
	$(STRIP) $(SO_OUT)

host:
	gcc -O1 -Wall -Wextra -g -fsanitize=address,undefined verkeer_daemon.c -o tests/freetoon-app-verkeer.host -lm

sim:
	gcc -O1 -Wall -Wextra -g -shared -fPIC $(SO_INC) -DSIM screen_verkeer.c -o tests/verkeer_ui.sim.so

clean:
	rm -f freetoon-app-verkeer.toon1 freetoon-app-verkeer.toon2 verkeer_ui.toon1.so verkeer_ui.toon2.so tests/freetoon-app-verkeer.host tests/verkeer_ui.sim.so
