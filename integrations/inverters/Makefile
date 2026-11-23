# Build freetoon-app-inverters for Toon 1 and Toon 2; `make test` runs the
# parser tests on the build host.
TC_ROOT ?= $(HOME)/toolchains

TARGET ?= toon2

ifeq ($(TARGET),toon1)
  CROSS       := $(TC_ROOT)/toon1-toolchain/bin/arm-linux-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/toon1-toolchain/arm-buildroot-linux-gnueabi/sysroot
  ARCHFLAGS   := -march=armv5te -marm -mfloat-abi=soft -DTOON1
  OUT         ?= freetoon-app-inverters.toon1
  # drop the toolchain's "for GNU/Linux 4.1.0" note, as toonui does (Toon 1 runs kernel 2.6)
  POST        := $(CROSS)objcopy --remove-section .note.ABI-tag
else
  CROSS       := $(TC_ROOT)/linaro/bin/arm-linux-gnueabihf-
  SYSROOTFLAG := --sysroot=$(TC_ROOT)/linaro/arm-linux-gnueabihf/libc
  ARCHFLAGS   :=
  OUT         ?= freetoon-app-inverters.toon2
endif

CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip
CFLAGS  := $(SYSROOTFLAG) $(ARCHFLAGS) -O2 -Wall -Wextra
SRCS    := inverters_daemon.c inv_drivers.c inv_crypto.c minijson.c

all: $(OUT)

$(OUT): $(SRCS) inv_drivers.h inv_crypto.h minijson.h
	$(CC) $(CFLAGS) $(SRCS) -o $(OUT)
	$(if $(POST),$(POST) $(OUT))
	$(STRIP) $(OUT)

test:
	gcc -O1 -Wall -Wextra -g -fsanitize=address,undefined tests/test_drivers.c inv_drivers.c inv_crypto.c minijson.c -I. -o tests/test_drivers
	./tests/test_drivers

host:
	gcc -O1 -Wall -Wextra -g $(SRCS) -o tests/freetoon-app-inverters.host

clean:
	rm -f freetoon-app-inverters.toon1 freetoon-app-inverters.toon2 tests/test_drivers tests/freetoon-app-inverters.host
