# Build standalone freetoon-app-stocks and stocks_ui.so for Toon 1 and Toon 2
TC_ROOT ?= $(HOME)/toolchains

TARGET ?= toon2

ifeq ($(TARGET),toon1)
  CROSS       := $(TC_ROOT)/toon1-toolchain/bin/arm-linux-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/toon1-toolchain/arm-buildroot-linux-gnueabi/sysroot
  ARCHFLAGS   := -march=armv5te -marm -mfloat-abi=soft -DTOON1
  OUT         ?= freetoon-app-stocks.toon1
  SO_OUT      := stocks_ui.toon1.so
else
  CROSS       := $(TC_ROOT)/linaro/bin/arm-linux-gnueabihf-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/linaro/arm-linux-gnueabihf/libc
  ARCHFLAGS   :=
  OUT         ?= freetoon-app-stocks.toon2
  SO_OUT      := stocks_ui.toon2.so
endif

CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip
CFLAGS  := $(SYSROOTFLAG) $(ARCHFLAGS) -O2 -Wall -Wextra -pthread
SO_INC  := -I. -I../../../freetoon-lvgl/lvgl_ui_recovered/src -I../../../freetoon-lvgl/lvgl_ui_recovered/src/lvgl -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE

all: $(OUT) $(SO_OUT)

$(OUT): stocks_daemon.c
	$(CC) $(CFLAGS) stocks_daemon.c -o $(OUT) -lm
	$(STRIP) $(OUT)

$(SO_OUT): screen_stocks.c
	$(CC) $(CFLAGS) -shared -fPIC $(SO_INC) screen_stocks.c -o $(SO_OUT) -lm
	$(STRIP) $(SO_OUT)

clean:
	rm -f freetoon-app-stocks freetoon-app-stocks.toon1 freetoon-app-stocks.toon2 stocks_ui.toon1.so stocks_ui.toon2.so
