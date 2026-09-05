# Linux ABI test programs (docs/compat/linux/testing.md). lxhello and
# lxtest are freestanding: the project toolchain, the user flags and
# user.ld, no crt0 and no libc, so they carry no CosmoOS note and run
# under the Linux personality. hello_musl is a real statically linked
# musl program, built only where musl-gcc exists (the CI runner installs
# musl-tools); HAVE_MUSL tells the boot harness to require its output.

LINUX_TEST_OUT := $(OUT)/tests/linux
LINUX_TEST_PROGRAMS := lxhello lxtest
LINUX_TEST_CFLAGS := $(USER_CFLAGS) -Wno-missing-prototypes

MUSL_GCC ?= $(shell command -v musl-gcc 2>/dev/null)
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

LINUX_TEST_ELFS := $(foreach p,$(LINUX_TEST_PROGRAMS),$(LINUX_TEST_OUT)/$(p).elf)
LINUX_TEST_ARCHIVE_ENTRIES := $(foreach p,$(LINUX_TEST_PROGRAMS),tests/linux/$(p)=$(LINUX_TEST_OUT)/$(p).elf)

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
