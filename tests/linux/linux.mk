# Linux ABI test programs (docs/compat/linux/testing.md). lxhello and
# lxtest are freestanding: the project toolchain, the user flags and
# user.ld, no crt0 and no libc, so they carry no CosmoOS note and run
# under the Linux personality. hello_musl is a real statically linked
# musl program, built only where musl-gcc exists (the CI runner installs
# musl-tools); HAVE_MUSL tells the boot harness to require its output.

LINUX_TEST_OUT := $(OUT)/tests/linux
LINUX_TEST_PROGRAMS := lxhello lxtest lxsig
LINUX_TEST_CFLAGS := $(USER_CFLAGS) -Wno-missing-prototypes

# musl-gcc produces x86-64 code where the CI runner installs it; on other
# architectures the canary is not built.
ifeq ($(ARCH),x86_64)
MUSL_GCC ?= $(shell command -v musl-gcc 2>/dev/null)
else
MUSL_GCC :=
endif
HAVE_MUSL := $(if $(MUSL_GCC),1,0)

define linux_test_rule
$(LINUX_TEST_OUT)/$(1).elf: $(ROOT)/tests/linux/$(1).c $(ROOT)/tests/linux/lxabi.h $(ROOT)/compat/linux/linux_abi.h $(USER_LD)
	$$(call log,CC,$$<)
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(CC) $$(LINUX_TEST_CFLAGS) -c $$< -o $$@.o
	$$(call log,LD,$$@)
	$$(Q)$$(LD) $$(USER_LDFLAGS) -T $(USER_LD) -o $$@ $$@.o
endef
$(foreach p,$(LINUX_TEST_PROGRAMS),$(eval $(call linux_test_rule,$(p))))

# Position-independent programs (milestone 10): lxinterp is an ET_DYN
# interpreter without PT_INTERP; lxdyn is an ET_DYN executable whose
# PT_INTERP names lxinterp. Both are freestanding; lld lays them out
# (no user.ld), -z norelro keeps the relocated data writable.
LINUX_PIE_LDFLAGS := -pie -z max-page-size=0x1000 -z noexecstack -z separate-code -z norelro \
	--build-id=none --gc-sections
$(LINUX_TEST_OUT)/lxinterp.elf: $(ROOT)/tests/linux/lxinterp.c $(ROOT)/tests/linux/lxabi.h $(ROOT)/compat/linux/linux_abi.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LINUX_TEST_CFLAGS) -fPIE -c $< -o $@.o
	$(call log,LD,$@)
	$(Q)$(LD) $(LINUX_PIE_LDFLAGS) --no-dynamic-linker -o $@ $@.o
$(LINUX_TEST_OUT)/lxdyn.elf: $(ROOT)/tests/linux/lxdyn.c $(ROOT)/tests/linux/lxabi.h $(ROOT)/compat/linux/linux_abi.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LINUX_TEST_CFLAGS) -fPIE -c $< -o $@.o
	$(call log,LD,$@)
	$(Q)$(LD) $(LINUX_PIE_LDFLAGS) --dynamic-linker=/boot/tests/linux/lxinterp -o $@ $@.o

LINUX_TEST_ELFS := $(foreach p,$(LINUX_TEST_PROGRAMS) lxinterp lxdyn,$(LINUX_TEST_OUT)/$(p).elf)
LINUX_TEST_ARCHIVE_ENTRIES := $(foreach p,$(LINUX_TEST_PROGRAMS) lxinterp lxdyn,tests/linux/$(p)=$(LINUX_TEST_OUT)/$(p).elf)

ifneq ($(MUSL_GCC),)
$(LINUX_TEST_OUT)/hello_musl: $(ROOT)/tests/linux/hello_musl.c
	$(call log,MUSLCC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(MUSL_GCC) -static -Os -o $@ $<
LINUX_TEST_ELFS += $(LINUX_TEST_OUT)/hello_musl
LINUX_TEST_ARCHIVE_ENTRIES += tests/linux/hello_musl=$(LINUX_TEST_OUT)/hello_musl
endif

.PHONY: linux-tests
linux-tests: $(LINUX_TEST_ELFS)
