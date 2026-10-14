# mbedTLS, and ssl_client on it: https for BusyBox wget with the server's
# certificate checked (the https unit; ports/mbedtls/README.md). Static
# against the musl sysroot and compiler-rt builtins the BusyBox port
# builds, with the project's clang, as dropbear and sftp-server are. Two
# pinned downloads, fetched once into $(PORTS_CACHE) through
# scripts/fetch-pinned.py: the mbedTLS release, and the Mozilla CA
# certificates as curl publishes them, installed as
# /etc/ssl/certs/ca-certificates.crt.

MBEDTLS_VERSION := 3.6.7
MBEDTLS_SHA256  := a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
MBEDTLS_URL     := https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$(MBEDTLS_VERSION)/mbedtls-$(MBEDTLS_VERSION).tar.bz2
MBEDTLS_TAR     := $(PORTS_CACHE)/mbedtls-$(MBEDTLS_VERSION).tar.bz2
CACERT_DATE     := 2026-09-25
CACERT_SHA256   := a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505
CACERT_URL      := https://curl.se/ca/cacert-$(CACERT_DATE).pem
CACERT_PEM      := $(PORTS_CACHE)/cacert-$(CACERT_DATE).pem

# One build per architecture beside $(OUT), shared by the build variants as
# BusyBox's is.
MBEDTLS_DIR    ?= $(dir $(OUT))mbedtls-$(ARCH)
MBEDTLS_LIBS   := $(MBEDTLS_DIR)/src/library/libmbedtls.a $(MBEDTLS_DIR)/src/library/libmbedx509.a \
	$(MBEDTLS_DIR)/src/library/libmbedcrypto.a
SSL_CLIENT_ELF := $(MBEDTLS_DIR)/ssl_client
# The library's default configuration, then ours (cosmo_config.h).
MBEDTLS_CFLAGS := -O2 -ffile-prefix-map=$(MBEDTLS_DIR)=mbedtls -ffile-prefix-map=$(ROOT)=/cosmo \
	-I$(ROOT)/ports/mbedtls -DMBEDTLS_USER_CONFIG_FILE='<cosmo_config.h>'

$(MBEDTLS_TAR):
	$(Q)$(PYTHON) $(ROOT)/scripts/fetch-pinned.py $@ $(MBEDTLS_SHA256) $(MBEDTLS_URL) \
		$(PINNED_MIRROR)/$(notdir $(MBEDTLS_TAR))
$(CACERT_PEM):
	$(Q)$(PYTHON) $(ROOT)/scripts/fetch-pinned.py $@ $(CACERT_SHA256) $(CACERT_URL) \
		$(PINNED_MIRROR)/$(notdir $(CACERT_PEM))

# The library's own Makefile, its three archives only (no programs or
# tests). APPLE_BUILD=0: it would otherwise take the host for the target
# on macOS. AR_DASH empty: llvm-ar.
$(MBEDTLS_LIBS) &: $(MBEDTLS_TAR) $(BB_LIBC) $(BB_BUILTINS) $(BB_CC) $(ROOT)/ports/mbedtls/cosmo_config.h \
		$(ROOT)/ports/mbedtls/mbedtls.mk
	$(call log,MBEDTLS,$(MBEDTLS_VERSION) $(BB_TARGET))
	$(Q)rm -rf $(MBEDTLS_DIR)/src
	$(Q)mkdir -p $(MBEDTLS_DIR)/src
	$(Q)tar -xjf $(MBEDTLS_TAR) -C $(MBEDTLS_DIR)/src --strip-components=1
	$(Q)$(MAKE) -s -C $(MBEDTLS_DIR)/src/library CC=$(BB_CC) AR="$(AR)" AR_DASH= APPLE_BUILD=0 \
		CFLAGS="$(MBEDTLS_CFLAGS)" libmbedcrypto.a libmbedx509.a libmbedtls.a \
		> $(MBEDTLS_DIR)/build.log 2>&1 || { tail -40 $(MBEDTLS_DIR)/build.log; exit 1; }

$(SSL_CLIENT_ELF): $(ROOT)/ports/mbedtls/ssl_client.c $(MBEDTLS_LIBS)
	$(call log,CC,ssl_client $(BB_TARGET))
	$(Q)$(BB_CC) $(MBEDTLS_CFLAGS) -Wall -Wextra -Werror -I$(MBEDTLS_DIR)/src/include -o $@.unstripped $< \
		$(MBEDTLS_LIBS)
	$(Q)$(OBJCOPY) --strip-all --remove-section=.note --remove-section=.comment $@.unstripped $@

.PHONY: ssl_client
ssl_client: $(SSL_CLIENT_ELF)

# /bin/ssl_client, which wget runs for https (ports/busybox/wget-ssl-client.patch),
# and the certificate authorities it trusts.
MBEDTLS_ARCHIVE_ENTRIES := bin/ssl_client=$(SSL_CLIENT_ELF) etc/ssl/certs/ca-certificates.crt=$(CACERT_PEM)
MBEDTLS_ARCHIVE_DEPS := $(SSL_CLIENT_ELF) $(CACERT_PEM)
