# Host-side unit tests: kernel algorithms compiled natively with ASan and
# UBSan. The shim directory supplies host versions of the arch headers;
# everything else is the real kernel source.

HOST_CC ?= $(CC)
HOST_OUT := $(OUT)/host

HOST_CFLAGS := \
	-std=c11 -g -O1 -fno-omit-frame-pointer \
	-fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-Wall -Wextra -Werror -Wshadow -Wno-unused-function \
	-DCONFIG_DEBUG=1 -DCONFIG_SELFTEST=0 -DCONFIG_CRASH_TEST=0 \
	-I$(ROOT)/tests/host/shim \
	-I$(ROOT)/tests/host \
	-I$(ROOT)/kernel/include \
	-I$(ROOT)/kernel/memory \
	-I$(ROOT)/boot/protocol

HOST_LDFLAGS := -fsanitize=address,undefined

HOST_COMMON_SRCS := \
	tests/host/harness.c \
	tests/host/shim_spinlock.c \
	kernel/memory/buddy.c

HOST_BUDDY_SRCS := $(HOST_COMMON_SRCS) tests/host/test_buddy.c
HOST_SLAB_SRCS  := $(HOST_COMMON_SRCS) kernel/memory/slab.c kernel/memory/kmalloc.c tests/host/test_slab.c
HOST_CRYPTO_SRCS := $(HOST_COMMON_SRCS) kernel/security/sha512.c kernel/security/ed25519.c kernel/core/crc32c.c tests/host/test_crypto.c
HOST_MODELF_SRCS := $(HOST_COMMON_SRCS) kernel/module/modelf.c tests/host/test_modelf.c
HOST_COSMOFS_SRCS := $(HOST_COMMON_SRCS) tests/host/test_cosmofs.c
HOST_LIBC_SRCS := tests/host/test_libc.c
HOST_PKG_SRCS := tests/host/test_pkg.c pkg/manifest.c pkg/version.c pkg/tar.c
HOST_LINUX_SRCS := tests/host/test_linux.c compat/linux/convert.c
HOST_HV_SRCS := $(HOST_COMMON_SRCS) kernel/arch/x86_64/svm_npt.c tests/host/test_hv.c
HOST_RELOC_A64_SRCS := $(HOST_COMMON_SRCS) kernel/arch/aarch64/modreloc.c tests/host/test_reloc_aarch64.c
HOST_VIRTQ_SRCS := $(HOST_COMMON_SRCS) drivers/virtio/virtqueue.c tests/host/test_virtq.c
HOST_CRED_SRCS := $(HOST_COMMON_SRCS) kernel/process/cred.c tests/host/test_cred.c
HOST_QUIESCE_SRCS := $(HOST_COMMON_SRCS) tests/host/test_quiesce.c

HOST_TESTS := $(HOST_OUT)/test_buddy $(HOST_OUT)/test_slab $(HOST_OUT)/test_crypto $(HOST_OUT)/test_modelf $(HOST_OUT)/test_cosmofs $(HOST_OUT)/test_libc $(HOST_OUT)/test_pkg $(HOST_OUT)/test_linux $(HOST_OUT)/test_hv $(HOST_OUT)/test_reloc_aarch64 $(HOST_OUT)/test_virtq $(HOST_OUT)/test_cred $(HOST_OUT)/test_quiesce

$(HOST_OUT)/test_buddy: $(addprefix $(ROOT)/,$(HOST_BUDDY_SRCS))
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_slab: $(addprefix $(ROOT)/,$(HOST_SLAB_SRCS))
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_crypto: $(addprefix $(ROOT)/,$(HOST_CRYPTO_SRCS))
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_modelf: $(addprefix $(ROOT)/,$(HOST_MODELF_SRCS))
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -DMODELF_HOST_TEST=1 $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_libc: $(addprefix $(ROOT)/,$(HOST_LIBC_SRCS)) $(ROOT)/libc/src/printf.c $(ROOT)/libc/src/malloc.c $(ROOT)/libc/src/conv.c
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) -std=c11 -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-Wall -Wextra -Werror -Wno-missing-prototypes -Wno-builtin-requires-header -Wno-incompatible-library-redeclaration \
		-DLIBC_HOST_TEST=1 $< $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_pkg: $(addprefix $(ROOT)/,$(HOST_PKG_SRCS))
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) -std=c11 -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-Wall -Wextra -Werror -Wno-missing-prototypes -I$(ROOT)/pkg $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_linux: $(addprefix $(ROOT)/,$(HOST_LINUX_SRCS))
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/compat/linux $(addprefix $(ROOT)/,$(HOST_LINUX_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_hv: $(addprefix $(ROOT)/,$(HOST_HV_SRCS)) $(ROOT)/kernel/arch/x86_64/include/x86/svm.h
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/kernel/arch/x86_64/include $(addprefix $(ROOT)/,$(HOST_HV_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_reloc_aarch64: $(addprefix $(ROOT)/,$(HOST_RELOC_A64_SRCS)) $(ROOT)/kernel/arch/aarch64/include/aarch64/modreloc.h
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/kernel/arch/aarch64/include $(addprefix $(ROOT)/,$(HOST_RELOC_A64_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_virtq: $(addprefix $(ROOT)/,$(HOST_VIRTQ_SRCS)) $(ROOT)/drivers/include/drivers/virtio.h
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/drivers/include $(addprefix $(ROOT)/,$(HOST_VIRTQ_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cred: $(addprefix $(ROOT)/,$(HOST_CRED_SRCS)) $(ROOT)/kernel/include/kernel/cred.h
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $(addprefix $(ROOT)/,$(HOST_CRED_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_quiesce: $(addprefix $(ROOT)/,$(HOST_QUIESCE_SRCS)) $(ROOT)/kernel/include/kernel/quiesce_core.h
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -pthread $(addprefix $(ROOT)/,$(HOST_QUIESCE_SRCS)) $(HOST_LDFLAGS) -pthread -o $@

$(HOST_OUT)/test_cosmofs: $(addprefix $(ROOT)/,$(HOST_COSMOFS_SRCS))
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/kernel-services/filesystem/cosmofs $^ $(HOST_LDFLAGS) -o $@

.PHONY: host-test
host-test: $(HOST_TESTS)
	$(Q)for t in $(HOST_TESTS); do echo "== $$t"; ASAN_OPTIONS=detect_leaks=0 $$t || exit 1; done
