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

HOST_TESTS := $(HOST_OUT)/test_buddy $(HOST_OUT)/test_slab $(HOST_OUT)/test_crypto $(HOST_OUT)/test_modelf $(HOST_OUT)/test_cosmofs $(HOST_OUT)/test_libc

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

$(HOST_OUT)/test_cosmofs: $(addprefix $(ROOT)/,$(HOST_COSMOFS_SRCS))
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/kernel-services/filesystem/cosmofs $^ $(HOST_LDFLAGS) -o $@

.PHONY: host-test
host-test: $(HOST_TESTS)
	$(Q)for t in $(HOST_TESTS); do echo "== $$t"; ASAN_OPTIONS=detect_leaks=0 $$t || exit 1; done
