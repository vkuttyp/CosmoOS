# Build configuration knobs. Override on the command line or in the
# environment. Everything here describes the TARGET build, never the host.

# Target architecture. Directory names under kernel/arch/ are the valid set.
ARCH ?= x86_64

# debug: -O1 -g, assertions, self-tests run at boot.
# release: -O2 -g, assertions kept (they are cheap and this is a kernel),
#          self-tests off.
BUILD ?= debug

ifeq ($(filter $(BUILD),debug release),)
$(error BUILD must be debug or release, got '$(BUILD)')
endif

# Boot-time self-tests. Default follows BUILD; force with SELFTEST=0/1.
ifeq ($(BUILD),debug)
SELFTEST ?= 1
else
SELFTEST ?= 0
endif

# Module signatures. 1 (default in every build type) refuses a module
# without a valid signature from a key in the kernel's ring. 0 is a
# development convenience: unsigned modules load with a warning and the
# kernel reports itself as tainted. A bad signature is refused either way.
MODULE_SIG_ENFORCE ?= 1

# Signing keys (docs/kernel/module/design.md, "Security"). No private key
# is ever part of the repository.
#   SIGNING=dev      (default) a per-machine developer key pair, generated on
#                    first use by scripts/devkey.sh in $(COSMO_KEYDIR), outside
#                    the tree. Modules and packages are signed with it and the
#                    kernel built here trusts its public half (plus any release
#                    public keys checked in under tools/keys/*.pub).
#   SIGNING=release  MODSIGN_KEY (a key that never touches a shared build host)
#                    and KEYRING_PUBS (the public keys the kernel trusts) must
#                    both be given explicitly; nothing is generated.
SIGNING ?= dev
COSMO_KEYDIR ?= $(HOME)/.config/cosmoos/keys
TRUSTED_RELEASE_PUBS := $(sort $(wildcard $(ROOT)/tools/keys/*.pub))
ifeq ($(SIGNING),dev)
MODSIGN_KEY ?= $(COSMO_KEYDIR)/dev.key
SIGNING_PUB := $(COSMO_KEYDIR)/dev.pub
KEYRING_PUBS ?= $(SIGNING_PUB) $(TRUSTED_RELEASE_PUBS)
else ifeq ($(SIGNING),release)
ifeq ($(MODSIGN_KEY),)
$(error SIGNING=release needs MODSIGN_KEY=<path to the release signing key>)
endif
ifeq ($(KEYRING_PUBS),)
$(error SIGNING=release needs KEYRING_PUBS=<public keys the kernel trusts>)
endif
SIGNING_PUB :=
else
$(error SIGNING must be dev or release, got '$(SIGNING)')
endif
# Packages and the INDEX are signed with the same key unless overridden; the
# image ships the first trusted public key as /etc/pkg/keys/<name>.pub.
PKGSIGN_KEY ?= $(MODSIGN_KEY)
PKG_TRUST_PUB ?= $(firstword $(KEYRING_PUBS))

# CRASH_TEST=1 makes kernel_main fault on purpose after the self-tests so
# the panic path and the harness's failure detection can be verified.
# Never enable in a build you intend to run for real.
CRASH_TEST ?= 0

# Output tree. Never inside the source directories.
OUT ?= $(ROOT)/out/$(ARCH)-$(BUILD)

# V=1 shows full command lines.
V ?= 0
ifeq ($(V),0)
Q := @
log = @printf '  %-8s %s\n' '$(1)' '$(patsubst $(ROOT)/%,%,$(2))'
else
Q :=
log =
endif

# QEMU test-run settings.
QEMU_MEM ?= 256M
QEMU_SMP ?= 4
QEMU_ACCEL ?= tcg
QEMU_EXTRA ?=

# Reproducibility: fixed epoch unless the caller provides one.
SOURCE_DATE_EPOCH ?= 0
export SOURCE_DATE_EPOCH

# Build identity, derived from the tree so equal sources give equal builds.
BUILD_ID := $(shell cd $(ROOT) && git describe --always --dirty 2>/dev/null || echo unknown)
