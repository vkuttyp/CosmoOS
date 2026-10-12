# Roadmap M5's layout probes (tests/security/, docs/kernel/process/testing.md,
# "test-security"), in every image: make test-security runs them on release
# builds. aslr-static and aslr-pie are static musl programs built with the
# BusyBox port's musl sysroot and compiler-rt (ports/busybox), so both
# architectures have them: aslr-static through the port's compiler wrapper
# (ET_EXEC at its link address), aslr-pie as a static PIE (ET_DYN with no
# interpreter, musl's rcrt1.o relocates it) that the kernel places at a
# random bias (linked with 4 KiB pages: a larger p_align would make the
# kernel align the bias and draw fewer bits). aslr-native is the native personality's probe.

SECURITY_TEST_OUT := $(OUT)/tests/security
BB_INC := -nostdinc -isystem $(BB_SYSROOT)/include -isystem $(CLANG_RESOURCE_INC) -ffile-prefix-map=$(ROOT)=/cosmo

$(SECURITY_TEST_OUT)/aslr-static: $(ROOT)/tests/security/aslr.c $(BB_LIBC) $(BB_BUILTINS) $(BB_CC) $(ROOT)/tests/security/security.mk
	$(call log,MUSLCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(BB_CC) -O2 -DASLR_KIND=static -ffile-prefix-map=$(ROOT)=/cosmo -o $@ $<

$(SECURITY_TEST_OUT)/aslr-pie: $(ROOT)/tests/security/aslr.c $(BB_LIBC) $(BB_BUILTINS) $(ROOT)/tests/security/security.mk
	$(call log,MUSLCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) --target=$(BB_TARGET) $(BB_INC) -O2 -fPIE -DASLR_KIND=pie -c $< -o $@.o
	$(Q)$(CC) --target=$(BB_TARGET) -fuse-ld=lld -static-pie -nostdlib -Wl,--build-id=none -Wl,-z,max-page-size=4096 \
		$(BB_SYSROOT)/lib/rcrt1.o $(BB_SYSROOT)/lib/crti.o $@.o -L$(BB_SYSROOT)/lib -lc $(BB_BUILTINS) \
		$(BB_SYSROOT)/lib/crtn.o -o $@

SECURITY_NATIVE_OBJ := $(SECURITY_TEST_OUT)/aslr-native.o
$(SECURITY_NATIVE_OBJ): $(ROOT)/tests/security/aslr-native.c
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(USER_CFLAGS) -c $< -o $@

$(SECURITY_TEST_OUT)/aslr-native: $(SECURITY_NATIVE_OBJ) $(LIBC_CRT0) $(LIBC_A) $(USER_LD)
	$(call log,LD,$@)
	$(Q)$(LD) $(USER_LDFLAGS) -T $(USER_LD) -o $@ $(LIBC_CRT0) $(SECURITY_NATIVE_OBJ) $(LIBC_A)

SECURITY_TEST_ELFS := $(addprefix $(SECURITY_TEST_OUT)/,aslr-static aslr-pie aslr-native)
SECURITY_TEST_ARCHIVE_ENTRIES := $(foreach p,aslr-static aslr-pie aslr-native,tests/security/$(p)=$(SECURITY_TEST_OUT)/$(p))
