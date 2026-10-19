# Dynamically linked test programs (roadmap 1.1 P4;
# docs/compat/linux/testing.md, "Shared libraries"), built with
# ports/busybox's cc-dyn against musl's libc.so: PT_INTERP is
# /lib/ld-musl-$(ARCH).so.1. dynhello checks the auxiliary vector, RELRO,
# W^X and TLS; dltest dlopens the shipped libdltest.so (RUNPATH
# /usr/lib/cosmo-test), a missing library, a library with a text
# relocation, and LD_LIBRARY_PATH with and without AT_SECURE.

DYN_TEST_OUT := $(OUT)/tests/dyn
DYN_DEPS := $(BB_CC_DYN) $(BB_LIBC_SO) $(BB_BUILTINS) $(ROOT)/tests/dyn/dyn.mk
DYN_CFLAGS := -O2 -Wall -Werror -ffile-prefix-map=$(ROOT)=/cosmo

$(DYN_TEST_OUT)/dynhello: $(ROOT)/tests/dyn/dynhello.c $(DYN_DEPS)
	$(call log,DYNCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(BB_CC_DYN) $(DYN_CFLAGS) -o $@ $<
$(DYN_TEST_OUT)/dltest: $(ROOT)/tests/dyn/dltest.c $(DYN_DEPS)
	$(call log,DYNCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(BB_CC_DYN) $(DYN_CFLAGS) -Wl,--enable-new-dtags -Wl,-rpath,/usr/lib/cosmo-test -o $@ $<
$(DYN_TEST_OUT)/libdltest.so: $(ROOT)/tests/dyn/libdltest.c $(DYN_DEPS)
	$(call log,DYNCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(BB_CC_DYN) $(DYN_CFLAGS) -shared -Wl,-soname,libdltest.so -o $@ $<
$(DYN_TEST_OUT)/shadow/libdltest.so: $(ROOT)/tests/dyn/libdltest.c $(DYN_DEPS)
	$(call log,DYNCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(BB_CC_DYN) $(DYN_CFLAGS) -DANSWER=7 -shared -Wl,-soname,libdltest.so -o $@ $<
$(DYN_TEST_OUT)/libtextrel.so: $(ROOT)/tests/dyn/libtextrel.c $(DYN_DEPS)
	$(call log,DYNCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(BB_CC_DYN) $(DYN_CFLAGS) -shared -Wl,-z,notext -Wl,-soname,libtextrel.so -o $@ $<

DYN_TEST_ELFS := $(addprefix $(DYN_TEST_OUT)/,dynhello dltest libdltest.so shadow/libdltest.so libtextrel.so)
DYN_TEST_ARCHIVE_ENTRIES := tests/dyn/dynhello=$(DYN_TEST_OUT)/dynhello tests/dyn/dltest=$(DYN_TEST_OUT)/dltest \
	tests/dyn/libtextrel.so=$(DYN_TEST_OUT)/libtextrel.so tests/dyn/shadow/libdltest.so=$(DYN_TEST_OUT)/shadow/libdltest.so \
	usr/lib/cosmo-test/libdltest.so=$(DYN_TEST_OUT)/libdltest.so

.PHONY: dyn-tests
dyn-tests: $(DYN_TEST_ELFS)
