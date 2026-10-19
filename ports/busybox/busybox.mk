# BusyBox, built from source for the architecture in hand (roadmap M3,
# decision 6; ports/busybox/README.md): musl and the compiler-rt builtins
# first, with the project's clang, then BusyBox against them with the
# checked-in configuration. The three source archives are pinned by
# version and SHA-256 and fetched once into $(PORTS_CACHE); nothing else
# is downloaded.

PORTS_CACHE ?= $(ROOT)/.cache/ports

BUSYBOX_VERSION := 1.37.0
BUSYBOX_SHA256  := 3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4
BUSYBOX_URL     := https://busybox.net/downloads/busybox-$(BUSYBOX_VERSION).tar.bz2
MUSL_VERSION    := 1.2.5
MUSL_SHA256     := a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
MUSL_URL        := https://musl.libc.org/releases/musl-$(MUSL_VERSION).tar.gz
CRT_VERSION     := 19.1.7
CRT_SHA256      := c12b6e764202c615c1a3af9a13d477846878757ae0e29e5f8979215a6958fffc
CRT_URL         := https://github.com/llvm/llvm-project/releases/download/llvmorg-$(CRT_VERSION)/compiler-rt-$(CRT_VERSION).src.tar.xz

BUSYBOX_TAR := $(PORTS_CACHE)/busybox-$(BUSYBOX_VERSION).tar.bz2
MUSL_TAR    := $(PORTS_CACHE)/musl-$(MUSL_VERSION).tar.gz
CRT_TAR     := $(PORTS_CACHE)/compiler-rt-$(CRT_VERSION).src.tar.xz

# Each archive's second URL: the copy in this repository's "pinned-sources"
# release, the same bytes (fetch-pinned.py checks the digest either way).
PINNED_MIRROR := https://github.com/vkuttyp/CosmoOS/releases/download/pinned-sources

# One BusyBox per architecture, shared by the build variants beside
# $(OUT) (debug, release, chaos, crash...): none of them changes it, and
# each would otherwise spend minutes building the same binary.
# check-reproducible.sh names one inside each of its two trees.
BB_DIR      ?= $(dir $(OUT))busybox-$(ARCH)
BB_TARGET   := $(ARCH)-linux-musl
BB_SYSROOT  := $(BB_DIR)/sysroot
BB_LIBC     := $(BB_SYSROOT)/lib/libc.a
# Roadmap 1.1 P4: musl shared as well, libc.so being its dynamic linker
# too, at /lib/ld-musl-$(ARCH).so.1 on the system (PT_INTERP's path).
BB_LIBC_SO  := $(BB_SYSROOT)/lib/libc.so
BB_HEADERS  := $(BB_DIR)/musl-headers.stamp
LDSO_NAME   := ld-musl-$(ARCH).so.1
LDSO_PATH   := /lib/$(LDSO_NAME)
BB_BUILTINS := $(BB_DIR)/rt/libclang_rt.builtins.a
BB_CC       := $(BB_DIR)/cc
BB_CC_DYN   := $(BB_DIR)/cc-dyn
BUSYBOX_ELF := $(BB_DIR)/busybox
BUSYBOX_CONFIG := $(ROOT)/ports/busybox/busybox.config
# Applied to the source in this order (ports/busybox/README.md, "Patches"),
# with git apply: the CI image has git and no patch. GIT_CEILING_DIRECTORIES
# keeps git from taking bb-src for part of this checkout, which would make
# the patch's paths relative to the checkout's top.
BUSYBOX_PATCHES := $(ROOT)/ports/busybox/wget-ssl-client.patch
BUSYBOX_APPLETS := $(shell grep -v '^\#' $(ROOT)/ports/busybox/applets)

# compiler-rt's generic builtins, each file of lib/builtins that is not an
# x87 80-bit long double routine where long double is IEEE quad (AArch64):
# those are x86-only. musl's AArch64 printf and strtod need the quad ones.
BB_RT_EXCLUDE_aarch64 := divxc3 fixunsxfdi fixunsxfsi fixunsxfti fixxfdi fixxfti floatdixf floattixf floatundixf \
	floatuntixf mulxc3 powixf2
BB_RT_EXCLUDE_x86_64 :=

$(BUSYBOX_TAR):
	$(Q)$(PYTHON) $(ROOT)/scripts/fetch-pinned.py $@ $(BUSYBOX_SHA256) $(BUSYBOX_URL) $(PINNED_MIRROR)/$(notdir $(BUSYBOX_TAR))
$(MUSL_TAR):
	$(Q)$(PYTHON) $(ROOT)/scripts/fetch-pinned.py $@ $(MUSL_SHA256) $(MUSL_URL) $(PINNED_MIRROR)/$(notdir $(MUSL_TAR))
$(CRT_TAR):
	$(Q)$(PYTHON) $(ROOT)/scripts/fetch-pinned.py $@ $(CRT_SHA256) $(CRT_URL) $(PINNED_MIRROR)/$(notdir $(CRT_TAR))

# musl in two steps around the builtins: its headers (configured, and
# installed into the sysroot), which the builtins compile against; then
# the libraries, static and shared, libc.so linked with the builtins
# (LIBCC; AArch64's long double needs the quad routines). One configure
# serves both steps.
$(BB_HEADERS): $(MUSL_TAR) $(ROOT)/ports/busybox/busybox.mk
	$(call log,MUSL,$(MUSL_VERSION) $(BB_TARGET) headers)
	$(Q)rm -rf $(BB_DIR)/musl-src $(BB_DIR)/musl-build $(BB_SYSROOT)
	$(Q)mkdir -p $(BB_DIR)/musl-src $(BB_DIR)/musl-build
	$(Q)tar -xzf $(MUSL_TAR) -C $(BB_DIR)/musl-src --strip-components=1
	$(Q)cd $(BB_DIR)/musl-build && $(BB_DIR)/musl-src/configure --target=$(BB_TARGET) --prefix=/ --syslibdir=/lib \
		--enable-shared CC="$(CC)" CFLAGS="--target=$(BB_TARGET) -O2 -ffile-prefix-map=$(BB_DIR)=busybox" \
		LDFLAGS="-fuse-ld=lld -Wl,--build-id=none" LIBCC="$(BB_BUILTINS)" AR="$(AR)" RANLIB="$(AR) s" \
		> configure.log 2>&1 \
		|| { cat configure.log; exit 1; }
	$(Q)$(MAKE) -s -C $(BB_DIR)/musl-build DESTDIR=$(BB_SYSROOT) install-headers > /dev/null
	$(Q)touch $@

$(BB_LIBC) $(BB_LIBC_SO) &: $(BB_HEADERS) $(BB_BUILTINS)
	$(call log,MUSL,$(MUSL_VERSION) $(BB_TARGET))
	$(Q)$(MAKE) -s -C $(BB_DIR)/musl-build > $(BB_DIR)/musl-build/make.log 2>&1 || { tail -40 $(BB_DIR)/musl-build/make.log; exit 1; }
	$(Q)$(MAKE) -s -C $(BB_DIR)/musl-build DESTDIR=$(BB_SYSROOT) install-libs > /dev/null
	$(Q)touch $(BB_LIBC) $(BB_LIBC_SO)

# The builtins are position-independent: libc.so and every dynamic
# program link them (a static link takes PIC code as it is).
$(BB_BUILTINS): $(CRT_TAR) $(BB_HEADERS) $(ROOT)/ports/busybox/busybox.mk
	$(call log,BUILTINS,compiler-rt $(CRT_VERSION) $(BB_TARGET))
	$(Q)rm -rf $(BB_DIR)/rt
	$(Q)mkdir -p $(BB_DIR)/rt/src
	$(Q)tar -xJf $(CRT_TAR) -C $(BB_DIR)/rt/src --strip-components=1 compiler-rt-$(CRT_VERSION).src/lib/builtins
	$(Q)cd $(BB_DIR)/rt && for f in src/lib/builtins/*.c; do \
		b=$$(basename $$f .c); \
		case " $(BB_RT_EXCLUDE_$(ARCH)) " in *" $$b "*) continue ;; esac; \
		$(CC) --target=$(BB_TARGET) -nostdinc -isystem $(BB_SYSROOT)/include -isystem $(CLANG_RESOURCE_INC) \
			-O2 -fPIC -fno-builtin -fvisibility=hidden -ffile-prefix-map=$(BB_DIR)=busybox -w -c $$f -o $$b.o || exit 1; \
	done
	$(Q)$(AR) rcs $@ $(BB_DIR)/rt/*.o

$(BB_CC): $(ROOT)/ports/busybox/cc.in $(ROOT)/ports/busybox/busybox.mk
	$(Q)mkdir -p $(dir $@)
	$(Q)sed -e 's|@CC@|$(CC)|g' -e 's|@TARGET@|$(BB_TARGET)|g' -e 's|@SYSROOT@|$(BB_SYSROOT)|g' \
		-e 's|@RESOURCE@|$(CLANG_RESOURCE_INC)|g' -e 's|@BUILTINS@|$(BB_BUILTINS)|g' -e 's|@BBDIR@|$(BB_DIR)|g' $< > $@
	$(Q)chmod +x $@

$(BB_CC_DYN): $(ROOT)/ports/busybox/cc-dyn.in $(ROOT)/ports/busybox/busybox.mk
	$(Q)mkdir -p $(dir $@)
	$(Q)sed -e 's|@CC@|$(CC)|g' -e 's|@TARGET@|$(BB_TARGET)|g' -e 's|@SYSROOT@|$(BB_SYSROOT)|g' \
		-e 's|@RESOURCE@|$(CLANG_RESOURCE_INC)|g' -e 's|@BUILTINS@|$(BB_BUILTINS)|g' -e 's|@BBDIR@|$(BB_DIR)|g' \
		-e 's|@LDSO@|$(LDSO_PATH)|g' $< > $@
	$(Q)chmod +x $@

# The checked-in configuration must be complete: `oldconfig` with every
# answer the default may not change it, or the build would not be the
# configuration the tree records. KCONFIG_NOTIMESTAMP keeps the build
# time out of the configuration and the binary (make reproducible).
$(BUSYBOX_ELF): $(BUSYBOX_TAR) $(BUSYBOX_CONFIG) $(BUSYBOX_PATCHES) $(BB_LIBC) $(BB_BUILTINS) $(BB_CC)
	$(call log,BUSYBOX,$(BUSYBOX_VERSION) $(BB_TARGET))
	$(Q)rm -rf $(BB_DIR)/bb-src $(BB_DIR)/bb-build
	$(Q)mkdir -p $(BB_DIR)/bb-src $(BB_DIR)/bb-build
	$(Q)tar -xjf $(BUSYBOX_TAR) -C $(BB_DIR)/bb-src --strip-components=1
	$(Q)for p in $(BUSYBOX_PATCHES); do \
		(cd $(BB_DIR)/bb-src && GIT_CEILING_DIRECTORIES=$(BB_DIR) git apply -p1 $$p) || exit 1; done
	$(Q)cp $(BUSYBOX_CONFIG) $(BB_DIR)/bb-build/.config
	$(Q)yes "" | KCONFIG_NOTIMESTAMP=1 $(MAKE) -s -C $(BB_DIR)/bb-src O=$(BB_DIR)/bb-build HOSTCC="$(CC)" oldconfig \
		> $(BB_DIR)/oldconfig.log 2>&1
	$(Q)cmp -s $(BB_DIR)/bb-build/.config $(BUSYBOX_CONFIG) \
		|| { echo "busybox: $(BUSYBOX_CONFIG) is incomplete (oldconfig changed it)"; exit 1; }
	$(Q)KCONFIG_NOTIMESTAMP=1 $(MAKE) -s -C $(BB_DIR)/bb-src O=$(BB_DIR)/bb-build CC=$(BB_CC) HOSTCC="$(CC)" AR="$(AR)" NM="$(NM)" \
		OBJCOPY="$(OBJCOPY)" LD="$(LD)" SKIP_STRIP=y > $(BB_DIR)/build.log 2>&1 || { tail -40 $(BB_DIR)/build.log; exit 1; }
	$(Q)$(OBJCOPY) --strip-all --remove-section=.note --remove-section=.comment $(BB_DIR)/bb-build/busybox_unstripped $@

# The same BusyBox dynamically linked (roadmap 1.1 P4): the checked-in
# configuration less CONFIG_STATIC, built through cc-dyn in a second build
# directory from the same patched source. /bin/busybox stays the static
# one -- the shell must not depend on /lib -- and this is
# /bin/busybox.dyn, with applet links of its own under /usr/bin.
BUSYBOX_DYN_ELF := $(BB_DIR)/busybox.dyn
BUSYBOX_DYN_CONFIG := $(BB_DIR)/busybox-dyn.config
$(BUSYBOX_DYN_ELF): $(BUSYBOX_ELF) $(BUSYBOX_CONFIG) $(BB_LIBC_SO) $(BB_BUILTINS) $(BB_CC_DYN)
	$(call log,BUSYBOX,$(BUSYBOX_VERSION) $(BB_TARGET) dynamic)
	$(Q)rm -rf $(BB_DIR)/bb-build-dyn
	$(Q)mkdir -p $(BB_DIR)/bb-build-dyn
	$(Q)sed 's/^CONFIG_STATIC=y$$/# CONFIG_STATIC is not set/' $(BUSYBOX_CONFIG) > $(BUSYBOX_DYN_CONFIG)
	$(Q)cmp -s $(BUSYBOX_DYN_CONFIG) $(BUSYBOX_CONFIG) && { echo "busybox: CONFIG_STATIC=y not found"; exit 1; } || true
	$(Q)cp $(BUSYBOX_DYN_CONFIG) $(BB_DIR)/bb-build-dyn/.config
	$(Q)yes "" | KCONFIG_NOTIMESTAMP=1 $(MAKE) -s -C $(BB_DIR)/bb-src O=$(BB_DIR)/bb-build-dyn HOSTCC="$(CC)" oldconfig \
		> $(BB_DIR)/oldconfig-dyn.log 2>&1
	$(Q)cmp -s $(BB_DIR)/bb-build-dyn/.config $(BUSYBOX_DYN_CONFIG) \
		|| { echo "busybox: the dynamic configuration is incomplete (oldconfig changed it)"; exit 1; }
	$(Q)KCONFIG_NOTIMESTAMP=1 $(MAKE) -s -C $(BB_DIR)/bb-src O=$(BB_DIR)/bb-build-dyn CC=$(BB_CC_DYN) HOSTCC="$(CC)" \
		AR="$(AR)" NM="$(NM)" OBJCOPY="$(OBJCOPY)" LD="$(LD)" SKIP_STRIP=y > $(BB_DIR)/build-dyn.log 2>&1 \
		|| { tail -40 $(BB_DIR)/build-dyn.log; exit 1; }
	$(Q)$(OBJCOPY) --strip-all --remove-section=.note --remove-section=.comment \
		$(BB_DIR)/bb-build-dyn/busybox_unstripped $@

.PHONY: busybox
busybox: $(BUSYBOX_ELF) $(BUSYBOX_DYN_ELF)

# BusyBox's own testsuite, packed reproducibly for the image (make test-busybox).
BUSYBOX_TESTSUITE := $(BB_DIR)/testsuite.tgz
$(BUSYBOX_TESTSUITE): $(BUSYBOX_ELF) $(ROOT)/ports/busybox/pack-testsuite.py
	$(Q)$(PYTHON) $(ROOT)/ports/busybox/pack-testsuite.py $(BB_DIR)/bb-src/testsuite $@

# musl's shared libc and its dynamic linker, a link to it (roadmap 1.1 P4):
# /lib/libc.so and /lib/ld-musl-$(ARCH).so.1, the path PT_INTERP names.
MUSL_ARCHIVE_ENTRIES := lib/libc.so=$(BB_LIBC_SO) lib/$(LDSO_NAME)@libc.so
MUSL_ARCHIVE_DEPS := $(BB_LIBC_SO)

# /bin/busybox and a symbolic link per applet (mkbootarchive's NAME@TARGET),
# and the acceptance test's scripts and data under /boot/tests/busybox.
BUSYBOX_ARCHIVE_ENTRIES := bin/busybox=$(BUSYBOX_ELF) $(foreach a,$(BUSYBOX_APPLETS),bin/$(a)@busybox) \
	tests/busybox/ash.sh=$(ROOT)/tests/busybox/ash.sh tests/busybox/suite.sh=$(ROOT)/tests/busybox/suite.sh \
	tests/busybox/testsuite.tgz=$(BUSYBOX_TESTSUITE) tests/busybox/busybox.config=$(BUSYBOX_CONFIG)
# The dynamic BusyBox and its links (roadmap 1.1 P4): /bin/busybox.dyn,
# /usr/bin/<applet> -> /bin/busybox.dyn; /bin comes first in every PATH
# the system sets, so these run only when named or put first.
BUSYBOX_ARCHIVE_ENTRIES += bin/busybox.dyn=$(BUSYBOX_DYN_ELF) $(foreach a,$(BUSYBOX_APPLETS),usr/bin/$(a)@../../bin/busybox.dyn)
BUSYBOX_ARCHIVE_DEPS := $(BUSYBOX_DYN_ELF) $(BUSYBOX_ELF) $(BUSYBOX_TESTSUITE) $(ROOT)/tests/busybox/ash.sh $(ROOT)/tests/busybox/suite.sh \
	$(BUSYBOX_CONFIG) $(ROOT)/ports/busybox/applets
