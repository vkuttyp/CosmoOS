# The package manager (/sbin/pkg) and the ports repository (docs/pkg/).
# pkg is an ordinary user program: its sources plus the kernel's SHA-512
# and Ed25519 code compiled for user mode, and userland/lib/fetch.c, which
# runs /bin/cosmo-fetch for network repositories. `make ports` cross-compiles
# every recipe under ports/ into $(PKG_REPO) with tools/pkgbuild.py and
# signs packages and INDEX with $(PKGSIGN_KEY) (build/config.mk: the module
# signing key unless overridden).

PKG_ELF  := $(OUT)/userland/pkg.elf
PKG_REPO := $(OUT)/pkg/repo
PKGBUILD := $(PYTHON) $(ROOT)/tools/pkgbuild.py

PKG_SRCS := pkg/pkg.c pkg/manifest.c pkg/version.c pkg/tar.c pkg/verify.c userland/lib/fetch.c userland/lib/rootsnap.c \
	kernel/security/sha512.c kernel/security/ed25519.c
PKG_OBJS := $(addprefix $(OUT)/pkgprog/,$(patsubst %.c,%.o,$(PKG_SRCS)))

# Compiled into a separate object tree: the crypto sources also exist as kernel objects.
$(OUT)/pkgprog/%.o: $(ROOT)/%.c
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(USER_CFLAGS) -I$(ROOT)/pkg -MMD -MP -c $< -o $@

$(PKG_ELF): $(PKG_OBJS) $(LIBC_CRT0) $(LIBC_A) $(USER_LD)
	$(call log,LD,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(LD) $(USER_LDFLAGS) -T $(USER_LD) -o $@ $(LIBC_CRT0) $(PKG_OBJS) $(LIBC_A)

PKG_ANALYZE := $(addprefix $(OUT)/pkgprog/,$(patsubst %.c,%.analyzed,$(filter pkg/%,$(PKG_SRCS))))
$(OUT)/pkgprog/%.analyzed: $(ROOT)/%.c analyze-force
	$(call log,ANALYZE,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(USER_CFLAGS) -I$(ROOT)/pkg --analyze -Xanalyzer -analyzer-output=plist-multi-file $< -o $@.tmp
	$(Q)mv $@.tmp $@

# --- ports -> repository ---------------------------------------------------------

PORT_DIRS := $(sort $(dir $(wildcard $(ROOT)/ports/*/port)))
PORT_SRCS := $(wildcard $(ROOT)/ports/*/port $(ROOT)/ports/*/src/* $(ROOT)/ports/*/data/* $(ROOT)/ports/*/doc/*)
PKG_INDEX := $(PKG_REPO)/INDEX
# A package file outside the repository (roadmap 1.1 P4, #416's review):
# libdemo 2.0, which no longer provides libdemo.so, for rc.test to try
# over libdemo 1.0 while demo-c requires it.
PKG_FIXTURE_DIR := $(OUT)/pkg/fixtures
ifeq ($(SELFTEST),1)
PKG_FIXTURES := --test-fixtures
else
PKG_FIXTURES :=
endif

# The dynamically linked BusyBox as a package too (roadmap 1.1 P4):
# /usr/bin/busybox-dyn, which requires libc.so (the base system's).
$(PKG_INDEX): $(PORT_SRCS) $(LIBC_A) $(LIBC_CRT0) $(USER_LD) $(ROOT)/tools/pkgbuild.py $(ROOT)/scripts/modsign.py $(PKGSIGN_KEY) \
		$(BUSYBOX_DYN_ELF)
	$(Q)rm -rf $(PKG_REPO)
	$(Q)mkdir -p $(PKG_REPO)
	$(Q)for d in $(PORT_DIRS); do \
		$(PKGBUILD) build --port $$d --out $(PKG_REPO) --cc "$(CC)" --cflags "$(USER_CFLAGS)" \
			--ld "$(LD)" --ldflags "$(USER_LDFLAGS)" --ldscript $(USER_LD) --crt0 $(LIBC_CRT0) \
			--libc $(LIBC_A) --sign-key $(PKGSIGN_KEY) $(PKG_FIXTURES) || exit 1; \
	done
	$(Q)$(PKGBUILD) pack --name busybox-dyn --version $(BUSYBOX_VERSION) \
		--summary "BusyBox $(BUSYBOX_VERSION), dynamically linked against musl" \
		--file usr/bin/busybox-dyn=$(BUSYBOX_DYN_ELF) --sign-key $(PKGSIGN_KEY) \
		--out $(PKG_REPO)/busybox-dyn-$(BUSYBOX_VERSION).cpk
	$(Q)$(PKGBUILD) index --repo $(PKG_REPO) --sign-key $(PKGSIGN_KEY)
ifeq ($(SELFTEST),1)
	$(Q)mkdir -p $(PKG_FIXTURE_DIR)
	$(Q)$(PKGBUILD) pack --name libdemo --version 2.0 --summary "library test: libdemo without libdemo.so" \
		--file usr/share/libdemo/README=$(ROOT)/ports/libdemo/data/libdemo-so.txt --sign-key $(PKGSIGN_KEY) \
		--out $(PKG_FIXTURE_DIR)/libdemo-2.0.cpk
endif

.PHONY: ports pkg
ports: $(PKG_INDEX)
pkg: $(PKG_ELF)

# Archive entries: every repository file as repo/<name>; the recipe list
# is known only after the build, so the archive rule shells out for it.
PKG_ARCHIVE_ENTRIES = $(foreach f,$(sort $(wildcard $(PKG_REPO)/*)),repo/$(notdir $(f))=$(f))
ifeq ($(SELFTEST),1)
PKG_FIXTURE_ARCHIVE_ENTRIES := tests/pkg/libdemo-2.0.cpk=$(PKG_FIXTURE_DIR)/libdemo-2.0.cpk
endif

-include $(PKG_OBJS:.o=.d)
