# SPDX-License-Identifier: GPL-2.0
#
# Build scx_kairos from this directory.
#
# The kernel's tools/sched_ext/Makefile only builds sources that sit in its
# own directory, so this file repeats its compile and skeleton rules for our
# sources. It reuses what that build already produced: libbpf.a and its
# installed headers, bpftool and vmlinux.h, plus the scx headers from the
# kernel source. Run `make -C ~/linux-src/tools/sched_ext` once first, and
# again after a kernel update so vmlinux.h matches the running kernel.

KSRC      ?= $(HOME)/linux-src
SCX_SRC   := $(KSRC)/tools/sched_ext
SCX_BUILD := $(SCX_SRC)/build

BPFTOOL   := $(SCX_BUILD)/sbin/bpftool
LIBBPF    := $(SCX_BUILD)/obj/libbpf/libbpf.a
VMLINUX_H := $(SCX_BUILD)/include/vmlinux.h
SCX_HDRS  := $(wildcard $(SCX_SRC)/include/scx/*.h)

# Sets SRCARCH (arm64 here, x86 on an x86 machine); bpf_tracing.h needs it.
include $(KSRC)/tools/scripts/Makefile.arch

CLANG ?= clang
OUT   := build

# As upstream: with -target bpf, clang does not search the system include
# dirs, so add them with -idirafter (searched last, only as a fallback).
SYS_INCLUDES := $(shell $(CLANG) -v -E - </dev/null 2>&1 | \
	sed -n '/<...> search starts here:/,/End of search list./{ s| \(/.*\)|-idirafter \1|p }')

# BPF flags as in tools/sched_ext/Makefile:
#  -mcpu=v3          BPF ISA v3 (32-bit jumps and ALU ops)
#  -mlittle-endian   arm64 and x86 are both little-endian
#  -fms-extensions   kernel headers embed tagged structs anonymously
BPF_CFLAGS := -g -O2 -mcpu=v3 -target bpf -mlittle-endian		\
	-D__TARGET_ARCH_$(SRCARCH)					\
	-I$(SCX_SRC)/include -I$(SCX_SRC)/include/bpf-compat		\
	-I$(SCX_BUILD)/include -I$(KSRC)/tools/include/uapi		\
	$(SYS_INCLUDES)							\
	-Wall -Wno-compare-distinct-pointer-types			\
	-Wno-microsoft-anon-tag -fms-extensions

CFLAGS   := -g -O2 -Wall -Werror
INCLUDES := -I$(OUT) -I$(SCX_BUILD)/include -I$(KSRC)/tools/include	\
	    -I$(KSRC)/tools/include/uapi -I$(SCX_SRC)/include
LDLIBS   := -lelf -lz -lpthread

all: scx_kairos

$(OUT):
	mkdir -p $@

# 1. Compile the BPF policy into a BPF ELF object.
$(OUT)/scx_kairos.bpf.o: scx_kairos.bpf.c scx_kairos.h $(VMLINUX_H) $(SCX_HDRS) | $(OUT)
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

# 2. Pass it through libbpf's static linker (upstream links three times and
#    diffs the last two outputs as a self-test; once is enough to build),
#    then generate the skeleton: a header that embeds the object and gives
#    the loader typed access to it (skel->rodata, skel->maps.stats, ...).
$(OUT)/scx_kairos.bpf.skel.h: $(OUT)/scx_kairos.bpf.o $(BPFTOOL)
	$(BPFTOOL) gen object $(OUT)/scx_kairos.bpf.linked.o $<
	$(BPFTOOL) gen skeleton $(OUT)/scx_kairos.bpf.linked.o name scx_kairos > $@

# 3. Compile the user-space loader and link it with the static libbpf.
scx_kairos: scx_kairos.c scx_kairos.h $(OUT)/scx_kairos.bpf.skel.h $(SCX_HDRS) $(LIBBPF)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBBPF) $(LDLIBS)

# These come from the upstream build; fail with a hint if they are missing.
$(BPFTOOL) $(LIBBPF) $(VMLINUX_H):
	@echo "missing $@; build the kernel examples first: make -C $(SCX_SRC)" >&2
	@exit 1

clean:
	rm -rf $(OUT) scx_kairos

.PHONY: all clean

# Don't leave a half-written skeleton header behind if a step fails.
.DELETE_ON_ERROR:
