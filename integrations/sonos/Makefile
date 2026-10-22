# Build standalone freetoon-app-sonos and sonos_ui.so for Toon 1 and Toon 2
TC_ROOT ?= $(HOME)/toolchains

TARGET ?= toon2

ifeq ($(TARGET),toon1)
  CROSS       := $(TC_ROOT)/toon1-toolchain/bin/arm-linux-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/toon1-toolchain/arm-buildroot-linux-gnueabi/sysroot
  ARCHFLAGS   := -march=armv5te -marm -mfloat-abi=soft -DTOON1
  OUT         ?= freetoon-app-sonos.toon1
  SO_OUT      := sonos_ui.toon1.so
else
  CROSS       := $(TC_ROOT)/linaro/bin/arm-linux-gnueabihf-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/linaro/arm-linux-gnueabihf/libc
  ARCHFLAGS   :=
  OUT         ?= freetoon-app-sonos.toon2
  SO_OUT      := sonos_ui.toon2.so
endif

CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip
CFLAGS  := $(SYSROOTFLAG) $(ARCHFLAGS) -O2 -Wall -Wextra -pthread
SO_INC  := -I. -I../../../freetoon-lvgl/lvgl_ui_recovered/src -I../../../freetoon-lvgl/lvgl_ui_recovered/src/lvgl -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE

all: $(OUT) $(SO_OUT)

$(OUT): sonos_daemon.c
	$(CC) $(CFLAGS) sonos_daemon.c -o $(OUT)
	$(STRIP) $(OUT)

$(SO_OUT): sonos.c screen_sonos.c
	$(CC) $(CFLAGS) -shared -fPIC $(SO_INC) sonos.c screen_sonos.c -o $(SO_OUT)
	$(STRIP) $(SO_OUT)

clean:
	rm -f freetoon-app-sonos freetoon-app-sonos.toon1 freetoon-app-sonos.toon2 sonos_ui.toon1.so sonos_ui.toon2.so test_sonos_ui.so
