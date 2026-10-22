# Build standalone freetoon-app-vent for Toon 1 and Toon 2
TC_ROOT ?= $(HOME)/toolchains

TARGET ?= toon2

ifeq ($(TARGET),toon1)
  CROSS       := $(TC_ROOT)/toon1-toolchain/bin/arm-linux-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/toon1-toolchain/arm-buildroot-linux-gnueabi/sysroot
  ARCHFLAGS   := -march=armv5te -marm -mfloat-abi=soft
  OUT         := freetoon-app-vent-toon1
else
  CROSS       := $(TC_ROOT)/linaro/bin/arm-linux-gnueabihf-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/linaro/arm-linux-gnueabihf/libc
  ARCHFLAGS   :=
  OUT         := freetoon-app-vent
endif

CC     := $(CROSS)gcc
STRIP  := $(CROSS)strip
CFLAGS := $(SYSROOTFLAG) $(ARCHFLAGS) -O2 -Wall -Wextra -pthread

all: $(OUT)

$(OUT): vent_daemon.c
	$(CC) $(CFLAGS) vent_daemon.c -o $(OUT)
	$(STRIP) $(OUT)

clean:
	rm -f freetoon-app-vent freetoon-app-vent-toon1
