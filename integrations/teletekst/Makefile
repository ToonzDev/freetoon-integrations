# Build standalone freetoon-app-teletekst and teletekst_ui.so for Toon 1 and Toon 2
TC_ROOT ?= $(HOME)/toolchains

TARGET ?= toon2

ifeq ($(TARGET),toon1)
  CROSS       := $(TC_ROOT)/toon1-toolchain/bin/arm-linux-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/toon1-toolchain/arm-buildroot-linux-gnueabi/sysroot
  ARCHFLAGS   := -march=armv5te -marm -mfloat-abi=soft -DTOON1
  OUT         ?= freetoon-app-teletekst.toon1
  SO_OUT      := teletekst_ui.toon1.so
else
  CROSS       := $(TC_ROOT)/linaro/bin/arm-linux-gnueabihf-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/linaro/arm-linux-gnueabihf/libc
  ARCHFLAGS   :=
  OUT         ?= freetoon-app-teletekst.toon2
  SO_OUT      := teletekst_ui.toon2.so
endif

CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip
CFLAGS  := $(SYSROOTFLAG) $(ARCHFLAGS) -O2 -Wall -Wextra -pthread
SO_INC  := -I. -I../../../freetoon-lvgl/lvgl_ui_recovered/src -I../../../freetoon-lvgl/lvgl_ui_recovered/src/lvgl -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE

all: $(OUT) $(SO_OUT)

$(OUT): teletekst_daemon.c
	$(CC) $(CFLAGS) teletekst_daemon.c -o $(OUT)
	$(STRIP) $(OUT)

$(SO_OUT): screen_teletekst.c
	$(CC) $(CFLAGS) -shared -fPIC $(SO_INC) screen_teletekst.c -o $(SO_OUT)
	$(STRIP) $(SO_OUT)

clean:
	rm -f freetoon-app-teletekst freetoon-app-teletekst.toon1 freetoon-app-teletekst.toon2 teletekst_ui.toon1.so teletekst_ui.toon2.so
