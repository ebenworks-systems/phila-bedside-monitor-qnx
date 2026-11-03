# Phila Health Ecosystem bedside monitor prototype - build for QNX 8.0 and for the host.
#
#   make test        build and run the unit tests on this machine (gcc or clang)
#   make qnx         cross-compile every program and test for QNX 8.0 (needs qnxsdp-env.sh sourced)
#   make clean
#
# QNX_ARCH picks the target: x86_64 (the QEMU image, default) or aarch64le (a Raspberry Pi 4).
#
# SPDX-License-Identifier: MIT

QNX_ARCH ?= x86_64
CC_HOST  ?= cc
CC_QNX   ?= qcc -Vgcc_nto$(QNX_ARCH)

WARN     := -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wformat=2 -Wno-format-nonliteral
CFLAGS   ?= -O2 -g
CPPFLAGS := -Iinclude
HOST_CFLAGS := $(CFLAGS) $(WARN) -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
QNX_CFLAGS  := $(CFLAGS) $(WARN) -std=gnu11 -D_QNX_SOURCE

CORE     := src/core/news2.c src/core/sha256.c src/core/record.c src/core/scenario.c src/core/beds.c
TESTS    := news2 sha256 record beds scenario
QNX_BINS := phila-sensord phila-monitord phila-hog phila-display phila-export

HOST_OUT := build/host
QNX_OUT  := build/qnx-$(QNX_ARCH)

.PHONY: all host test qnx clean

all: test

host: $(addprefix $(HOST_OUT)/test_,$(TESTS)) $(HOST_OUT)/phila-export

test: host
	@set -e; for t in $(TESTS); do ./$(HOST_OUT)/test_$$t; done
	@python3 tools/verify_record.py --self-test

$(HOST_OUT)/test_%: tests/test_%.c $(CORE) include/phila/*.h tests/check.h | $(HOST_OUT)
	$(CC_HOST) $(CPPFLAGS) $(HOST_CFLAGS) -o $@ $< $(CORE)

$(HOST_OUT)/phila-export: src/tools/export.c $(CORE) include/phila/*.h | $(HOST_OUT)
	$(CC_HOST) $(CPPFLAGS) $(HOST_CFLAGS) -o $@ $< $(CORE)

qnx: $(addprefix $(QNX_OUT)/,$(QNX_BINS)) $(addprefix $(QNX_OUT)/test_,$(TESTS))

$(QNX_OUT)/phila-sensord: src/qnx/sensord.c src/qnx/rt.c $(CORE) include/phila/*.h src/qnx/*.h | $(QNX_OUT)
	$(CC_QNX) $(CPPFLAGS) $(QNX_CFLAGS) -o $@ src/qnx/sensord.c src/qnx/rt.c $(CORE)

$(QNX_OUT)/phila-monitord: src/qnx/monitord.c src/qnx/resmgr.c src/qnx/rt.c $(CORE) include/phila/*.h src/qnx/*.h | $(QNX_OUT)
	$(CC_QNX) $(CPPFLAGS) $(QNX_CFLAGS) -o $@ src/qnx/monitord.c src/qnx/resmgr.c src/qnx/rt.c $(CORE) -lslog2

$(QNX_OUT)/phila-hog: src/qnx/hog.c src/qnx/rt.c src/qnx/*.h | $(QNX_OUT)
	$(CC_QNX) $(CPPFLAGS) $(QNX_CFLAGS) -o $@ src/qnx/hog.c src/qnx/rt.c

$(QNX_OUT)/phila-display: src/qnx/display.c src/qnx/*.h | $(QNX_OUT)
	$(CC_QNX) $(CPPFLAGS) $(QNX_CFLAGS) -o $@ src/qnx/display.c

$(QNX_OUT)/phila-export: src/tools/export.c $(CORE) include/phila/*.h | $(QNX_OUT)
	$(CC_QNX) $(CPPFLAGS) $(QNX_CFLAGS) -o $@ $< $(CORE)

$(QNX_OUT)/test_%: tests/test_%.c $(CORE) include/phila/*.h tests/check.h | $(QNX_OUT)
	$(CC_QNX) $(CPPFLAGS) $(QNX_CFLAGS) -o $@ $< $(CORE)

$(HOST_OUT) $(QNX_OUT):
	mkdir -p $@

clean:
	rm -rf build
