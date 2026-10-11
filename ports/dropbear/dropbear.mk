# dropbear, built from source for the architecture in hand (roadmap M4,
# decision 6; ports/dropbear/README.md): the ssh server and its key
# generator, static against the musl sysroot and compiler-rt builtins the
# BusyBox port builds (ports/busybox/busybox.mk), with the project's clang.
# The source archive is pinned by version and SHA-256 and fetched once into
# $(PORTS_CACHE) through scripts/fetch-pinned.py, like BusyBox's.

DROPBEAR_VERSION := 2026.94
DROPBEAR_SHA256  := e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81
DROPBEAR_URL     := https://matt.ucc.asn.au/dropbear/releases/dropbear-$(DROPBEAR_VERSION).tar.bz2
DROPBEAR_TAR     := $(PORTS_CACHE)/dropbear-$(DROPBEAR_VERSION).tar.bz2

# One build per architecture beside $(OUT), shared by the build variants as
# BusyBox's is; make reproducible names one inside each of its two trees.
DB_DIR          ?= $(dir $(OUT))dropbear-$(ARCH)
DROPBEAR_ELF    := $(DB_DIR)/dropbear
DROPBEARKEY_ELF := $(DB_DIR)/dropbearkey
DROPBEAR_LOCALOPTS := $(ROOT)/ports/dropbear/localoptions.h

$(DROPBEAR_TAR):
	$(Q)$(PYTHON) $(ROOT)/scripts/fetch-pinned.py $@ $(DROPBEAR_SHA256) $(DROPBEAR_URL) \
		$(PINNED_MIRROR)/$(notdir $(DROPBEAR_TAR))

# configure for the musl target with BusyBox's compiler wrapper (a static
# link with musl's start files, the image at 4 MiB). Hardening is off:
# its -pie would make a static-pie, which the wrapper's fixed image base
# rules out. No zlib, no login records (utmp, wtmp, lastlog: decision 6),
# no syslog (dropbear runs in the foreground, -F, and logs to stderr,
# which svc keeps in the service's log).
# -ffile-prefix-map keeps the build directory out of the binary (make
# reproducible).
$(DROPBEAR_ELF) $(DROPBEARKEY_ELF) &: $(DROPBEAR_TAR) $(DROPBEAR_LOCALOPTS) $(BB_LIBC) $(BB_BUILTINS) $(BB_CC) \
		$(ROOT)/ports/dropbear/dropbear.mk
	$(call log,DROPBEAR,$(DROPBEAR_VERSION) $(BB_TARGET))
	$(Q)rm -rf $(DB_DIR)/src
	$(Q)mkdir -p $(DB_DIR)/src
	$(Q)tar -xjf $(DROPBEAR_TAR) -C $(DB_DIR)/src --strip-components=1
	$(Q)cp $(DROPBEAR_LOCALOPTS) $(DB_DIR)/src/localoptions.h
	$(Q)cd $(DB_DIR)/src && ./configure --host=$(BB_TARGET) \
		CC=$(BB_CC) AR="$(AR)" RANLIB="$(AR) s" CFLAGS="-O2 -ffile-prefix-map=$(DB_DIR)=dropbear" \
		--enable-static --disable-harden --disable-zlib --disable-syslog --disable-lastlog --disable-utmp \
		--disable-utmpx --disable-wtmp --disable-wtmpx --disable-loginfunc --disable-pututline \
		--disable-pututxline --enable-bundled-libtom > $(DB_DIR)/configure.log 2>&1 \
		|| { tail -40 $(DB_DIR)/configure.log; exit 1; }
	$(Q)$(MAKE) -s -C $(DB_DIR)/src PROGRAMS="dropbear dropbearkey" STATIC=1 > $(DB_DIR)/build.log 2>&1 \
		|| { tail -40 $(DB_DIR)/build.log; exit 1; }
	$(Q)$(OBJCOPY) --strip-all --remove-section=.note --remove-section=.comment $(DB_DIR)/src/dropbear $(DROPBEAR_ELF)
	$(Q)$(OBJCOPY) --strip-all --remove-section=.note --remove-section=.comment $(DB_DIR)/src/dropbearkey \
		$(DROPBEARKEY_ELF)

.PHONY: dropbear
dropbear: $(DROPBEAR_ELF)

# /sbin/dropbear and /sbin/dropbearkey, and the start script the installed
# system's service runs (cosmo-install writes /etc/svc/dropbear there).
DROPBEAR_ARCHIVE_ENTRIES := sbin/dropbear=$(DROPBEAR_ELF) sbin/dropbearkey=$(DROPBEARKEY_ELF) \
	etc/dropbear/start=$(ROOT)/ports/dropbear/start
DROPBEAR_ARCHIVE_DEPS := $(DROPBEAR_ELF) $(DROPBEARKEY_ELF) $(ROOT)/ports/dropbear/start
