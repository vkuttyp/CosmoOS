# Linux ABI test programs (docs/compat/linux/testing.md). lxhello and
# lxtest are freestanding: the project toolchain, the user flags and
# user.ld, no crt0 and no libc, so they carry no CosmoOS note and run
# under the Linux personality. hello_musl is a real statically linked
# musl program, built only where musl-gcc exists (the CI runner installs
# musl-tools); HAVE_MUSL tells the boot harness to require its output.

LINUX_TEST_OUT := $(OUT)/tests/linux
LINUX_TEST_PROGRAMS := lxhello lxtest lxsig lxrights lxcwd lxpty
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

# lxnointerp (roadmap 1.1 P4): a position-independent executable whose
# PT_INTERP names a file that does not exist; lxtest requires its execve
# to fail with ENOENT. Freestanding, laid out by lld (no user.ld). The
# stand-in interpreter that ran before musl's (lxinterp, lxdyn) is gone:
# tests/dyn/ holds the dynamically linked programs.
LINUX_PIE_LDFLAGS := -pie -z max-page-size=0x1000 -z noexecstack -z separate-code -z norelro \
	--build-id=none --gc-sections
$(LINUX_TEST_OUT)/lxnointerp.elf: $(ROOT)/tests/linux/lxnointerp.c $(ROOT)/tests/linux/lxabi.h $(ROOT)/compat/linux/linux_abi.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LINUX_TEST_CFLAGS) -fPIE -c $< -o $@.o
	$(call log,LD,$@)
	$(Q)$(LD) $(LINUX_PIE_LDFLAGS) --dynamic-linker=/lib/ld-cosmo-missing.so.1 -o $@ $@.o

LINUX_TEST_ELFS := $(foreach p,$(LINUX_TEST_PROGRAMS) lxnointerp,$(LINUX_TEST_OUT)/$(p).elf)
LINUX_TEST_ARCHIVE_ENTRIES := $(foreach p,$(LINUX_TEST_PROGRAMS) lxnointerp,tests/linux/$(p)=$(LINUX_TEST_OUT)/$(p).elf)

ifneq ($(MUSL_GCC),)
$(LINUX_TEST_OUT)/hello_musl: $(ROOT)/tests/linux/hello_musl.c
	$(call log,MUSLCC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(MUSL_GCC) -static -Os -o $@ $<
LINUX_TEST_ELFS += $(LINUX_TEST_OUT)/hello_musl
LINUX_TEST_ARCHIVE_ENTRIES += tests/linux/hello_musl=$(LINUX_TEST_OUT)/hello_musl
# A libc event loop's assumption: a closed fd leaves every epoll set it was
# in (the epoll-close unit, docs/kernel/io/design.md "epoll").
$(LINUX_TEST_OUT)/epoll_musl: $(ROOT)/tests/linux/epoll_musl.c
	$(call log,MUSLCC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(MUSL_GCC) -static -Os -o $@ $<
LINUX_TEST_ELFS += $(LINUX_TEST_OUT)/epoll_musl
LINUX_TEST_ARCHIVE_ENTRIES += tests/linux/epoll_musl=$(LINUX_TEST_OUT)/epoll_musl
endif

.PHONY: linux-tests
linux-tests: $(LINUX_TEST_ELFS)
