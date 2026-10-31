# Build configuration knobs. Override on the command line or in the
# environment. Everything here describes the TARGET build, never the host.

# Target architecture. Directory names under kernel/arch/ are the valid set.
ARCH ?= x86_64

# debug: -O1 -g, assertions, self-tests run at boot.
# release: -O2 -g, assertions kept (they are cheap and this is a kernel),
#          self-tests off.
BUILD ?= debug

# Dependency validation defaults to debug-only, independently selectable
# for auditing a release kernel or checking a debug kernel without it.
ifeq ($(BUILD),debug)
LOCKDEP ?= 1
else
LOCKDEP ?= 0
endif
ifneq ($(LOCKDEP),0)
ifneq ($(LOCKDEP),1)
$(error LOCKDEP must be 0 or 1, got '$(LOCKDEP)')
endif
endif

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
# Grouped targets (`&:`, build/module.mk) need GNU make 4.3; a space in the
# key directory would split the prerequisite list.
ifneq ($(filter 3.% 4.0 4.1 4.2,$(MAKE_VERSION)),)
$(error GNU make 4.3 or newer is required (found $(MAKE_VERSION)))
endif
ifneq ($(words $(COSMO_KEYDIR)),1)
$(error COSMO_KEYDIR must not contain spaces: '$(COSMO_KEYDIR)')
endif
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
# The signing identity as a file whose contents change exactly when the
# key path, the trusted set or the revoked list does (scripts/
# write-if-changed below, in kernel/kernel.mk): the key ring, every
# signed module and the boot manifest depend on it, so switching keys or
# trust in an existing output tree rebuilds them rather than keeping
# what the previous identity signed or trusted (PR #368 review).
SIGNING_STAMP = $(OUT)/gen/signing.stamp
# Packages and the INDEX are signed with the same key unless overridden; the
# image ships every key the kernel trusts as /etc/pkg/keys/<name>.pub: a
# developer build its own and the release key, so it accepts the published
# network repository (roadmap 1.1 P6) as a release does. PKG_TRUST_PUB, the
# first, is the key the build's own packages verify by (an override of it
# is shipped too). pkg and cosmo-upgrade read at most 16 keys.
PKGSIGN_KEY ?= $(MODSIGN_KEY)
PKG_TRUST_PUB ?= $(firstword $(KEYRING_PUBS))
PKG_TRUST_PUBS ?= $(PKG_TRUST_PUB) $(filter-out $(PKG_TRUST_PUB),$(KEYRING_PUBS))
ifneq ($(word 17,$(PKG_TRUST_PUBS)),)
$(error /etc/pkg/keys would hold $(words $(PKG_TRUST_PUBS)) keys; pkg and cosmo-upgrade read at most 16)
endif

# CRASH_TEST=1 makes kernel_main fault on purpose after the self-tests so
# the panic path and the harness's failure detection can be verified. The
# other values: 2 W+X (test-wxn), 3 lockdep (test-crash), 4 a stack
# overrun (test-stackprot), 5 a failed Nova bounds check (test-crash-nova).
# Never enable in a build you intend to run for real.
CRASH_TEST ?= 0
# COSMOFS_STOP=1 builds a kernel that stops a cosmofs rollback's switch
# after its blocks are written and before its root (test-snapshot's crash
# point "during the switch"; docs/kernel-services/filesystem/cosmofs/
# testing.md). Like CRASH_TEST, only in a sibling output tree.
COSMOFS_STOP ?= 0

# PERCPU_WARN=1 turns the debug builds' per-CPU claim check (S25) from a
# panic into a once-per-site warning, so one boot lists every site that
# reads a per-CPU answer where the thread could move. The sweep's tool;
# never the shipped form.
PERCPU_WARN ?= 0

# HARNESS_BREAK=N makes net-harness's first N back-connection attempts
# fail on purpose, from inside the guest, so the retry the flake needs is
# exercised on every boot of that build rather than on one boot in twenty
# (docs/audit/next-subsystem-nettest-retry.md). `make test-harness-retry`
# builds and boots it. Debug builds only; never the shipped form.
HARNESS_BREAK ?= 0

# SCHED_BALANCE=0 removes the balancer: a thread then stays on the CPU
# that placement chose for it, as it did before that unit
# (docs/audit/next-subsystem-load-balancer.md). Shipped on; the knob
# exists so the tests that prove the balancer can turn it off.
SCHED_BALANCE ?= 1

# SCHED_CHAOS=1 (debug builds) arms a migrator in the tick that moves a
# ready thread to another CPU every few ticks for no reason: the whole
# suite under migration. `make test-chaos` builds and boots it.
SCHED_CHAOS ?= 0

# Verified boot (docs/boot/design.md, "Verified boot"). 1 (the default in
# every build type): the UEFI loader refuses to boot unless \cosmo\manifest
# is signed by a key in its ring (KEYRING_PUBS, the module ring) and
# kernel.elf, boot.tar and cmdline match it. 0 is the developer override: a
# loader that checks nothing and tells the kernel the boot is unverified,
# which the kernel's banner and sysctl kernel.verified_boot then say. There
# is no runtime switch. It changes the loader, so it gets its own tree.
VERIFY_BOOT ?= 1
ifeq ($(filter $(VERIFY_BOOT),0 1),)
$(error VERIFY_BOOT must be 0 or 1, got '$(VERIFY_BOOT)')
endif

# Output tree. Never inside the source directories.
OUT ?= $(ROOT)/out/$(ARCH)-$(BUILD)$(if $(filter 0,$(VERIFY_BOOT)),-noverify)

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
# A `make dist` tarball has no .git: its .build-id names the commit it was made from (scripts/dist.py).
BUILD_ID := $(if $(wildcard $(ROOT)/.build-id),$(shell cat $(ROOT)/.build-id),$(shell cd $(ROOT) && git describe --always --dirty 2>/dev/null || echo unknown))
