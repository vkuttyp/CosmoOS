# The package manager (/sbin/pkg) and the ports repository (docs/pkg/).
# pkg is an ordinary user program: its sources plus the kernel's SHA-512
# and Ed25519 code compiled for user mode. `make ports` cross-compiles
# every recipe under ports/ into $(PKG_REPO) with tools/pkgbuild.py and
# signs packages and INDEX with the module development key.

PKG_ELF  := $(OUT)/userland/pkg.elf
PKG_REPO := $(OUT)/pkg/repo
PKGSIGN_KEY ?= $(ROOT)/tools/keys/cosmo-dev.key
PKGBUILD := $(PYTHON) $(ROOT)/tools/pkgbuild.py

PKG_SRCS := pkg/pkg.c pkg/manifest.c pkg/version.c pkg/tar.c pkg/verify.c \
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
$(OUT)/pkgprog/%.analyzed: $(ROOT)/%.c
	$(call log,ANALYZE,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(USER_CFLAGS) -I$(ROOT)/pkg --analyze -Xanalyzer -analyzer-output=text $< -o /dev/null
	$(Q)touch $@

# --- ports -> repository ---------------------------------------------------------

PORT_DIRS := $(sort $(dir $(wildcard $(ROOT)/ports/*/port)))
PORT_SRCS := $(wildcard $(ROOT)/ports/*/port $(ROOT)/ports/*/src/* $(ROOT)/ports/*/data/* $(ROOT)/ports/*/doc/*)
PKG_INDEX := $(PKG_REPO)/INDEX
ifeq ($(SELFTEST),1)
PKG_FIXTURES := --test-fixtures
else
PKG_FIXTURES :=
endif

$(PKG_INDEX): $(PORT_SRCS) $(LIBC_A) $(LIBC_CRT0) $(USER_LD) $(ROOT)/tools/pkgbuild.py $(ROOT)/scripts/modsign.py $(PKGSIGN_KEY)
	$(Q)rm -rf $(PKG_REPO)
	$(Q)mkdir -p $(PKG_REPO)
	$(Q)for d in $(PORT_DIRS); do \
		$(PKGBUILD) build --port $$d --out $(PKG_REPO) --cc "$(CC)" --cflags "$(USER_CFLAGS)" \
			--ld "$(LD)" --ldflags "$(USER_LDFLAGS)" --ldscript $(USER_LD) --crt0 $(LIBC_CRT0) \
			--libc $(LIBC_A) --sign-key $(PKGSIGN_KEY) $(PKG_FIXTURES) || exit 1; \
	done
	$(Q)$(PKGBUILD) index --repo $(PKG_REPO) --sign-key $(PKGSIGN_KEY)

.PHONY: ports pkg
ports: $(PKG_INDEX)
pkg: $(PKG_ELF)

# Archive entries: every repository file as repo/<name>; the recipe list
# is known only after the build, so the archive rule shells out for it.
PKG_ARCHIVE_ENTRIES = $(foreach f,$(sort $(wildcard $(PKG_REPO)/*)),repo/$(notdir $(f))=$(f))

-include $(PKG_OBJS:.o=.d)
