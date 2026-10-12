# UEFI loader component build. Produces a PE/COFF EFI application.

LOADER_EFI := $(OUT)/boot/$(LOADER_EFI_NAME)
# The same loader built with VERIFY_BOOT=0, whatever this build's setting:
# make test-security boots it against a tampered image
# (docs/boot/testing.md). Built only when asked for; never in an image
# unless the build itself says VERIFY_BOOT=0.
LOADER_EFI_NOVERIFY := $(OUT)/boot/noverify/$(LOADER_EFI_NAME)

LOADER_SRCS := \
	boot/uefi/main.c \
	boot/uefi/acpi.c \
	boot/uefi/console.c \
	boot/uefi/string.c \
	boot/uefi/memory.c \
	boot/uefi/elf.c \
	boot/uefi/arch/$(ARCH)/cpu.c \
	boot/uefi/arch/$(ARCH)/paging.c \
	boot/uefi/arch/$(ARCH)/serial.c

LOADER_ASM_SRCS :=
ifeq ($(ARCH),aarch64)
LOADER_ASM_SRCS += boot/uefi/arch/aarch64/el2_stub.S
endif

LOADER_OBJS := $(call objs_of,$(LOADER_SRCS))
LOADER_ASM_OBJS := $(call objs_of,$(LOADER_ASM_SRCS))

$(eval $(call compile_rules,$(LOADER_OBJS),LOADER_CFLAGS))
$(eval $(call assemble_rules,$(LOADER_ASM_OBJS),LOADER_CFLAGS))

# Verified boot (boot/uefi/verify.c, docs/boot/design.md "Verified boot"):
# the kernel's own SHA-512, Ed25519 and key-ring sources, built freestanding
# for the loader into objects of its own, and the ring generated from
# KEYRING_PUBS (kernel/kernel.mk) -- the keys the kernel trusts for modules
# are the keys the loader trusts for the manifest.
LOADER_KCFLAGS = $(LOADER_CFLAGS) -I$(ROOT)/kernel/include
LOADER_KCRYPTO_OBJS := $(addprefix $(OUT)/boot/kcrypto/,sha512.o ed25519.o keyring.o keyring_builtin.o)
LOADER_VERIFY_OBJ := $(OUT)/boot/uefi/verify.o
LOADER_NOVERIFY_OBJ := $(OUT)/boot/noverify/verify.o

$(filter-out %/keyring_builtin.o,$(LOADER_KCRYPTO_OBJS)): $(OUT)/boot/kcrypto/%.o: $(ROOT)/kernel/security/%.c
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LOADER_KCFLAGS) $(EXTRA_CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/boot/kcrypto/keyring_builtin.o: $(KEYRING_SRC)
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LOADER_KCFLAGS) -ffile-prefix-map=$(OUT)=/cosmo/out -c $< -o $@

$(LOADER_VERIFY_OBJ): $(ROOT)/boot/uefi/verify.c
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LOADER_KCFLAGS) -DCONFIG_VERIFY_BOOT=$(VERIFY_BOOT) $(EXTRA_CFLAGS) -MMD -MP -c $< -o $@

$(LOADER_NOVERIFY_OBJ): $(ROOT)/boot/uefi/verify.c
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LOADER_KCFLAGS) -DCONFIG_VERIFY_BOOT=0 $(EXTRA_CFLAGS) -MMD -MP -c $< -o $@

# A VERIFY_BOOT=0 loader links none of the checks. (VERIFY_BOOT has a tree
# of its own, build/config.mk, so one verify.o per tree is right.)
ifeq ($(VERIFY_BOOT),0)
LOADER_VERIFY_LINK := $(LOADER_VERIFY_OBJ)
else
LOADER_VERIFY_LINK := $(LOADER_VERIFY_OBJ) $(LOADER_KCRYPTO_OBJS)
endif

$(OUT)/boot/uefi/verify.analyzed: $(ROOT)/boot/uefi/verify.c analyze-force
	$(call log,ANALYZE,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LOADER_KCFLAGS) -DCONFIG_VERIFY_BOOT=1 $(EXTRA_CFLAGS) --analyze -Xanalyzer -analyzer-output=plist-multi-file $< -o $@.tmp
	$(Q)mv $@.tmp $@

LOADER_ANALYZE := $(patsubst %.o,%.analyzed,$(LOADER_OBJS)) $(OUT)/boot/uefi/verify.analyzed

$(LOADER_EFI): $(LOADER_OBJS) $(LOADER_ASM_OBJS) $(LOADER_VERIFY_LINK)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(LDLINK) $(LOADER_LDFLAGS) /out:$@ $(LOADER_OBJS) $(LOADER_ASM_OBJS) $(LOADER_VERIFY_LINK)

$(LOADER_EFI_NOVERIFY): $(LOADER_OBJS) $(LOADER_ASM_OBJS) $(LOADER_NOVERIFY_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(LDLINK) $(LOADER_LDFLAGS) /out:$@ $(LOADER_OBJS) $(LOADER_ASM_OBJS) $(LOADER_NOVERIFY_OBJ)

boot: $(LOADER_EFI)

-include $(LOADER_OBJS:.o=.d) $(LOADER_VERIFY_OBJ:.o=.d) $(LOADER_NOVERIFY_OBJ:.o=.d) \
	$(patsubst %.o,%.d,$(filter-out %/keyring_builtin.o,$(LOADER_KCRYPTO_OBJS)))
