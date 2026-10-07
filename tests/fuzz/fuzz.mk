# Fuzz targets (docs/verification/design.md, "Fuzzing").
#
#   make fuzz                      build and run every target with the portable
#                                  driver: seeds, corpus, FUZZ_RUNS mutations
#   make fuzz FUZZ_RUNS=200000     longer
#   make fuzz FUZZ_ENGINE=libfuzzer  link with -fsanitize=fuzzer (Linux clang)
#   make fuzz-build                build only
#
# Every target compiles the real kernel or userland source under ASan and
# UBSan; the host harness supplies panic, klog and the page arena where a
# target needs them.

FUZZ_OUT      := $(OUT)/fuzz
FUZZ_RUNS     ?= 20000
FUZZ_SEED     ?= 1
FUZZ_ENGINE   ?= driver
FUZZ_CORPUS   ?=

# FUZZ_EXTRA_CFLAGS / FUZZ_EXTRA_LDFLAGS: added to every target, for a
# coverage build (tools/fuzz-coverage.py) into its own OUT.
FUZZ_CFLAGS := $(HOST_CFLAGS) -I$(ROOT)/tests/fuzz $(FUZZ_EXTRA_CFLAGS)
FUZZ_LDFLAGS := $(HOST_LDFLAGS) $(FUZZ_EXTRA_LDFLAGS)
ifeq ($(FUZZ_ENGINE),libfuzzer)
FUZZ_CFLAGS  += -fsanitize=fuzzer
FUZZ_LDFLAGS += -fsanitize=fuzzer
FUZZ_DRIVER  :=
FUZZ_RUN_ARGS = -runs=$(FUZZ_RUNS) -seed=$(FUZZ_SEED) $(FUZZ_CORPUS)
else
FUZZ_DRIVER  := tests/fuzz/driver.c
FUZZ_RUN_ARGS = -runs $(FUZZ_RUNS) -seed $(FUZZ_SEED) -out $(FUZZ_OUT) $(FUZZ_CORPUS)
endif

# Leak detection where the sanitizer supports it (not on Darwin).
FUZZ_ASAN_OPTIONS ?= $(if $(filter Darwin,$(shell uname -s)),detect_leaks=0,detect_leaks=1)

FUZZ_COMMON := $(FUZZ_DRIVER) tests/host/harness.c tests/host/shim_spinlock.c kernel/memory/buddy.c

FUZZ_MODELF_SRCS  := tests/fuzz/fuzz_modelf.c kernel/module/modelf.c $(FUZZ_COMMON)
FUZZ_ELF_SRCS     := tests/fuzz/fuzz_elf.c kernel/process/elf.c $(FUZZ_COMMON)
FUZZ_PKG_SRCS     := tests/fuzz/fuzz_pkg.c pkg/manifest.c pkg/version.c pkg/tar.c $(FUZZ_DRIVER)
FUZZ_LINUX_SRCS   := tests/fuzz/fuzz_linux.c compat/linux/convert.c $(FUZZ_COMMON)
FUZZ_VIRTQ_SRCS   := tests/fuzz/fuzz_virtq.c drivers/virtio/virtqueue.c $(FUZZ_COMMON)
FUZZ_LZ4_SRCS     := tests/fuzz/fuzz_lz4.c kernel/core/lz4.c $(FUZZ_DRIVER)
FUZZ_USB_DESC_SRCS := tests/fuzz/fuzz_usb_desc.c drivers/usb/usb_desc.c $(FUZZ_COMMON)
FUZZ_FBVALID_SRCS := tests/fuzz/fuzz_fbvalid.c kernel/core/fbvalid.c $(FUZZ_COMMON)
FUZZ_COSMOFS_SRCS := tests/fuzz/fuzz_cosmofs.c tests/fuzz/shim_fs.c \
	kernel-services/filesystem/cosmofs/cosmofs_core.c kernel-services/filesystem/cosmofs/cosmofs.c \
	kernel-services/filesystem/cosmofs/cosmofs_snap.c \
	kernel-services/filesystem/cosmofs/cosmofs_member.c \
	kernel/core/lz4.c \
	kernel-services/filesystem/cosmofs/cosmofs_scrub.c \
	kernel-services/filesystem/cosmofs/cosmofs_check.c \
	kernel-services/filesystem/cosmofs/cosmofs_crypt.c \
	kernel/security/chacha20.c \
	kernel/security/sha512.c \
	kernel/core/crc32c.c kernel/memory/slab.c kernel/memory/kmalloc.c $(FUZZ_COMMON)

# The network stack on the host (docs/verification/design.md, "The network
# targets"): the protocol layers unchanged, over tests/fuzz/shim_net.c.
FUZZ_NET_STACK := kernel-services/network/ether.c kernel-services/network/arp.c kernel-services/network/ipv4.c \
	kernel-services/network/ipv6.c kernel-services/network/udp.c kernel-services/network/tcp.c \
	kernel-services/network/nat.c kernel-services/network/fw.c kernel-services/network/mbuf.c \
	kernel-services/network/cksum.c kernel-services/network/inet.c
FUZZ_NET_FRAME_SRCS := tests/fuzz/fuzz_net_frame.c tests/fuzz/shim_net.c $(FUZZ_NET_STACK) $(FUZZ_COMMON)
FUZZ_TCP_SEGMENTS_SRCS := tests/fuzz/fuzz_tcp_segments.c tests/fuzz/shim_net.c $(FUZZ_NET_STACK) $(FUZZ_COMMON)
FUZZ_NET_CONFIG_SRCS := tests/fuzz/fuzz_net_config.c tests/fuzz/shim_net.c $(FUZZ_NET_STACK) $(FUZZ_COMMON)
FUZZ_DHCP_DNS_SRCS := tests/fuzz/fuzz_dhcp_dns.c tests/fuzz/shim_net.c kernel-services/network/tapsvc.c \
	$(FUZZ_NET_STACK) $(FUZZ_COMMON)
# Checked-in corpora: frames from a boot's capture (tools/pcap-to-seeds.py).
FUZZ_NET_FRAME_CORPUS := $(ROOT)/tests/fuzz/corpus/net_frame

FUZZ_TARGETS := $(FUZZ_OUT)/fuzz_modelf $(FUZZ_OUT)/fuzz_elf $(FUZZ_OUT)/fuzz_pkg $(FUZZ_OUT)/fuzz_linux \
	$(FUZZ_OUT)/fuzz_virtq $(FUZZ_OUT)/fuzz_cosmofs $(FUZZ_OUT)/fuzz_lz4 $(FUZZ_OUT)/fuzz_usb_desc $(FUZZ_OUT)/fuzz_fbvalid \
	$(FUZZ_OUT)/fuzz_net_frame $(FUZZ_OUT)/fuzz_tcp_segments $(FUZZ_OUT)/fuzz_dhcp_dns $(FUZZ_OUT)/fuzz_net_config

$(FUZZ_OUT)/fuzz_fbvalid: $(addprefix $(ROOT)/,$(FUZZ_FBVALID_SRCS))
	$(call log,FUZZCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(FUZZ_CFLAGS) $(addprefix $(ROOT)/,$(FUZZ_FBVALID_SRCS)) $(FUZZ_LDFLAGS) -o $@

$(FUZZ_OUT)/fuzz_modelf: $(addprefix $(ROOT)/,$(FUZZ_MODELF_SRCS)) $(ROOT)/tests/host/modelf_image.h
	$(call log,FUZZCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(FUZZ_CFLAGS) -DMODELF_HOST_TEST=1 $(addprefix $(ROOT)/,$(FUZZ_MODELF_SRCS)) $(FUZZ_LDFLAGS) -o $@

$(FUZZ_OUT)/fuzz_elf: $(addprefix $(ROOT)/,$(FUZZ_ELF_SRCS))
	$(call log,FUZZCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(FUZZ_CFLAGS) -DELF_HOST_TEST=1 $(addprefix $(ROOT)/,$(FUZZ_ELF_SRCS)) $(FUZZ_LDFLAGS) -o $@

$(FUZZ_OUT)/fuzz_pkg: $(addprefix $(ROOT)/,$(FUZZ_PKG_SRCS))
	$(call log,FUZZCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) -std=c11 -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-Wall -Wextra -Werror -Wno-missing-prototypes -I$(ROOT)/pkg -I$(ROOT)/tests/fuzz \
		$(if $(filter libfuzzer,$(FUZZ_ENGINE)),-fsanitize=fuzzer,) \
		$(addprefix $(ROOT)/,$(FUZZ_PKG_SRCS)) $(FUZZ_LDFLAGS) -o $@

$(FUZZ_OUT)/fuzz_linux: $(addprefix $(ROOT)/,$(FUZZ_LINUX_SRCS))
	$(call log,FUZZCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(FUZZ_CFLAGS) -I$(ROOT)/compat/linux $(addprefix $(ROOT)/,$(FUZZ_LINUX_SRCS)) $(FUZZ_LDFLAGS) -o $@

$(FUZZ_OUT)/fuzz_lz4: $(addprefix $(ROOT)/,$(FUZZ_LZ4_SRCS))
	$(call log,FUZZCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(FUZZ_CFLAGS) $(addprefix $(ROOT)/,$(FUZZ_LZ4_SRCS)) $(FUZZ_LDFLAGS) -o $@

$(FUZZ_OUT)/fuzz_virtq: $(addprefix $(ROOT)/,$(FUZZ_VIRTQ_SRCS))
	$(call log,FUZZCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(FUZZ_CFLAGS) -I$(ROOT)/drivers/include $(addprefix $(ROOT)/,$(FUZZ_VIRTQ_SRCS)) $(FUZZ_LDFLAGS) -o $@

$(FUZZ_OUT)/fuzz_usb_desc: $(addprefix $(ROOT)/,$(FUZZ_USB_DESC_SRCS))
	$(call log,FUZZCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(FUZZ_CFLAGS) -I$(ROOT)/drivers/include $(addprefix $(ROOT)/,$(FUZZ_USB_DESC_SRCS)) $(FUZZ_LDFLAGS) -o $@

$(FUZZ_OUT)/fuzz_cosmofs: $(addprefix $(ROOT)/,$(FUZZ_COSMOFS_SRCS))
	$(call log,FUZZCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(FUZZ_CFLAGS) -I$(ROOT)/kernel-services/filesystem/cosmofs \
		$(addprefix $(ROOT)/,$(FUZZ_COSMOFS_SRCS)) $(FUZZ_LDFLAGS) -o $@

FUZZ_NET_HDRS := $(ROOT)/tests/fuzz/shim_net.h $(ROOT)/tests/fuzz/netpkt.h

$(FUZZ_OUT)/fuzz_net_frame: $(addprefix $(ROOT)/,$(FUZZ_NET_FRAME_SRCS)) $(FUZZ_NET_HDRS)
	$(call log,FUZZCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(FUZZ_CFLAGS) $(addprefix $(ROOT)/,$(FUZZ_NET_FRAME_SRCS)) $(FUZZ_LDFLAGS) -o $@

$(FUZZ_OUT)/fuzz_tcp_segments: $(addprefix $(ROOT)/,$(FUZZ_TCP_SEGMENTS_SRCS)) $(FUZZ_NET_HDRS)
	$(call log,FUZZCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(FUZZ_CFLAGS) -DTCP_HOST_TEST=1 $(addprefix $(ROOT)/,$(FUZZ_TCP_SEGMENTS_SRCS)) $(FUZZ_LDFLAGS) -o $@

$(FUZZ_OUT)/fuzz_net_config: $(addprefix $(ROOT)/,$(FUZZ_NET_CONFIG_SRCS)) $(FUZZ_NET_HDRS)
	$(call log,FUZZCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(FUZZ_CFLAGS) -DFW_HOST_TEST=1 $(addprefix $(ROOT)/,$(FUZZ_NET_CONFIG_SRCS)) $(FUZZ_LDFLAGS) -o $@

$(FUZZ_OUT)/fuzz_dhcp_dns: $(addprefix $(ROOT)/,$(FUZZ_DHCP_DNS_SRCS)) $(FUZZ_NET_HDRS)
	$(call log,FUZZCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(FUZZ_CFLAGS) -DTAPSVC_HOST_TEST=1 $(addprefix $(ROOT)/,$(FUZZ_DHCP_DNS_SRCS)) $(FUZZ_LDFLAGS) -o $@

.PHONY: fuzz fuzz-build
fuzz-build: $(FUZZ_TARGETS)

fuzz: $(FUZZ_TARGETS)
	$(Q)for t in $(FUZZ_TARGETS); do \
		corpus=""; case $$t in */fuzz_net_frame) corpus="$(FUZZ_NET_FRAME_CORPUS)";; esac; \
		echo "== $$t"; ASAN_OPTIONS=$(FUZZ_ASAN_OPTIONS) $$t $(FUZZ_RUN_ARGS) $$corpus || exit 1; done
	@echo "fuzz: PASS"
