# CosmoOS top-level build.
#
#   make [ARCH=x86_64|aarch64] [BUILD=debug|release] [V=1]   build loader, kernel, libc, userland,
#                     packages, modules and test guests
#   make image        FAT boot image with loader and kernel
#   make run          boot the image under QEMU on the terminal (serial)
#   make test         automated QEMU boot test with PASS/FAIL exit code
#   make test-gic     AArch64: the same boot test on the GICv3 machine
#   make test-platforms  AArch64: boots where QEMU differs from virt's defaults; the platform line (CI: BUILD=release)
#   make test-guard   the same boot test on a CPU model with SMEP/SMAP/UMIP (x86-64) or PAN (AArch64)
#   make test-smp2    the same boot test with two CPUs (the default is four)
#   make test-modcycle the debug boot test with module.cycle=: cosmotest loaded and unloaded 1000 times
#   make test-install roadmap M2: install to a blank disk, boot it, persist a file (CI: BUILD=release)
#   make test-encrypt roadmap 1.2 B5: an encrypted root -- passphrase, slots, key file, 1 GiB, forged metadata (CI: BUILD=release)
#   make test-encrypt-b1b4 test-encrypt's last stage alone: B1-B4's tests on an encrypted root
#   make test-snapshot roadmap 1.2 B1: snapshots of the installed root from the shell (CI: BUILD=release)
#   make test-fsck    roadmap 1.2 B2: fsck at boot and offline, scrub --repair on a mirror (CI: BUILD=release)
#   make test-pci-hotplug roadmap 1.2 B3: PCIe device_del/device_add, idle and under I/O, NVMe, late partition scan
#   make test-mirror  roadmap 1.2 B3: a two-disk mirrored root, a disk pulled, replaced and resilvered; a spare (CI: BUILD=release)
#   make test-large   CFS-32: compressible files past the old run bound, read by v1.0.13 (CI: BUILD=release)
#   make test-fill    MEM-9: writers past memory and past the root's space, on a 256 MiB guest (CI: BUILD=release)
#   make test-upgrade shell completeness S7: upgrade an installed v1.0.3 to this build, roll back (CI: BUILD=release)
#   make test-busybox roadmap M3: BusyBox ash's scripted test and testsuite subset (CI: BUILD=release)
#   make test-security roadmap M5: verified boot refuses tampered files; VERIFY_BOOT=0 says so (CI: BUILD=release)
#   make test-net-login roadmap M4: DHCP, wget by address and by name on the installed system (CI: BUILD=release)
#   make test-pkg-net  roadmap 1.1 P6: pkg against HTTPS repositories served by the harness (CI: BUILD=release)
#   make test-display  virtio-gpu: the console on the display's own size, read back from a screendump (CI: BUILD=release)
#   make test-crash   build a deliberately faulting kernel, verify panic path
#   make test-wxn     AArch64: build a kernel that executes a writable page, verify WXN denies it
#   make test-chaos   debug suite under a migrator that moves ready threads between CPUs every few ticks
#   make test-harness-retry  boot with net-harness's first back-connection broken on purpose
#   make host-test    native unit tests of kernel algorithms under ASan/UBSan
#   make host-test-fetch  cosmo-fetch's TLS, redirect and range cases on the host
#   make litmus       check the quiescence litmus tests against the C11 model (needs herd7)
#   make fuzz         fuzz the parsers on the host (docs/verification/)
#   make analyze      clang static analyzer over all target sources
#   make reproducible build twice into separate trees and compare outputs, the release set included
#   make release      roadmap M6: the signed release set for ARCH in out/release/ARCH (BUILD=release)
#   make compile-commands  compile_commands.json for clangd with cross flags
#   make check-tools  verify the cross toolchain is usable (and run check-secrets)
#   make check-secrets  fail if a private or revoked module-signing key is tracked
#   make check-docs   every subsystem in docs/README.md has its five documentation files
#   make clean        remove $(OUT)
#
# Host and target are separate concepts throughout. See docs/build/.

ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

include $(ROOT)/build/config.mk
include $(ROOT)/build/toolchain.mk
include $(ROOT)/build/rules.mk

.PHONY: FORCE all kernel boot modules image run test test-entropy test-modcycle test-install test-encrypt test-encrypt-b1b4 test-snapshot test-fsck test-pci-hotplug test-mirror test-pkgtx test-be test-large test-fill test-upgrade test-security test-busybox test-net-login test-pkg-net test-display test-gic test-platforms test-guard test-smp2 test-crash test-wxn test-chaos test-harness-retry analyze analysis-gate reproducible release release-set compile-commands check-tools check-secrets check-docs clean help litmus
.DEFAULT_GOAL := all

include $(ROOT)/kernel/kernel.mk
include $(ROOT)/boot/uefi/boot.mk
include $(ROOT)/libc/libc.mk
# BusyBox before userland: its applet list decides which native programs
# move aside to cosmo-<name> (userland/userland.mk).
include $(ROOT)/ports/busybox/busybox.mk
include $(ROOT)/ports/dropbear/dropbear.mk
include $(ROOT)/ports/openssh/openssh.mk
include $(ROOT)/ports/mbedtls/mbedtls.mk
include $(ROOT)/userland/userland.mk
include $(ROOT)/pkg/pkg.mk
# The Linux ABI test programs and the virtualization guests both build
# for the architecture in hand; the guests live in tests/hv/$(ARCH)/.
include $(ROOT)/tests/linux/linux.mk
include $(ROOT)/tests/dyn/dyn.mk
include $(ROOT)/tests/security/security.mk
include $(ROOT)/tests/hv/hv.mk
ARCH_TEST_TARGETS := hv-guests linux-tests
include $(ROOT)/build/module.mk
include $(ROOT)/tests/host/host.mk
include $(ROOT)/tests/fuzz/fuzz.mk

all: $(ARCH_TEST_TARGETS) kernel boot libc userland pkg ports modules

IMAGE := $(OUT)/cosmoos.img

image: $(IMAGE)

# The boot archive: init plus the boot-time and test modules, in the
# order the kernel loads them (dependencies first). See
# scripts/mkbootarchive.py and docs/kernel/module/. The loader rides in it
# (efi/, shell completeness S7): the manifest signs the archive, so
# cosmo-upgrade installs a loader the signature covers.
BOOT_ARCHIVE := $(OUT)/boot.tar
BOOT_ARCHIVE_ENTRIES = init=$(INIT_ELF) $(USER_ARCHIVE_ENTRIES) $(MUSL_ARCHIVE_ENTRIES) $(DYN_TEST_ARCHIVE_ENTRIES) $(BUSYBOX_ARCHIVE_ENTRIES) $(DROPBEAR_ARCHIVE_ENTRIES) $(OPENSSH_ARCHIVE_ENTRIES) $(MBEDTLS_ARCHIVE_ENTRIES) sbin/pkg=$(PKG_ELF) $(PKG_ARCHIVE_ENTRIES) $(PKG_FIXTURE_ARCHIVE_ENTRIES) $(LINUX_TEST_ARCHIVE_ENTRIES) $(SECURITY_TEST_ARCHIVE_ENTRIES) $(HV_ARCHIVE_ENTRIES) $(MODULE_ARCHIVE_ENTRIES) efi/$(LOADER_EFI_NAME)=$(LOADER_EFI)

# The archive's list of entries as a file that changes exactly when the
# list does (an entry added or renamed, not only a file changed), so an
# existing tree rebuilds the archive then. The repository's entries are
# left out: they are known only once the repository is built, and its
# INDEX is a prerequisite already.
ARCHIVE_STAMP = $(OUT)/gen/archive-entries.stamp
.PHONY: archive-stamp-force
archive-stamp-force:
$(ARCHIVE_STAMP): archive-stamp-force
	$(Q)mkdir -p $(dir $@)
	$(Q)printf '%s\n' $(filter-out $(PKG_ARCHIVE_ENTRIES),$(BOOT_ARCHIVE_ENTRIES)) > $@.tmp
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(BOOT_ARCHIVE): $(ARCHIVE_STAMP) $(USER_ARCHIVE_DEPS) $(MUSL_ARCHIVE_DEPS) $(DYN_TEST_ELFS) $(BUSYBOX_ARCHIVE_DEPS) $(DROPBEAR_ARCHIVE_DEPS) $(OPENSSH_ARCHIVE_DEPS) $(MBEDTLS_ARCHIVE_DEPS) $(PKG_ELF) $(PKG_INDEX) $(LINUX_TEST_ELFS) $(SECURITY_TEST_ELFS) $(HV_GUEST_BINS) $(MODULE_KOS) $(LOADER_EFI) $(ROOT)/scripts/mkbootarchive.py
	$(call log,ARCHIVE,$@)
	$(Q)$(PYTHON) $(ROOT)/scripts/mkbootarchive.py $@ $(BOOT_ARCHIVE_ENTRIES)

# The command line, \cosmo\cmdline (docs/boot/design.md, "The command
# line"). It is signed with the kernel and the archive, so it is fixed at
# build time: root=boot-disk names the CosmoOS root partition on the disk
# the loader was read from, which the live image does not have and the
# installed disk does (roadmap M5: the installer signs nothing and writes
# no file on the ESP). Rewritten only when CMDLINE changes.
CMDLINE ?= root=boot-disk
CMDLINE_FILE := $(OUT)/cmdline
FORCE:
# $(file) writes the value exactly, newline added: no shell sees it, so
# a quote in it is a quote in the signed text (PR #368 review).
$(CMDLINE_FILE): FORCE
	$(shell mkdir -p $(dir $@))$(file >$@.tmp,$(CMDLINE))
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

# The signed manifest over kernel, archive and command line (scripts/
# mkmanifest.py, docs/boot/design.md "Verified boot"), with the key that
# signs modules: the loader trusts the ring the kernel trusts.
MANIFEST := $(OUT)/manifest
$(MANIFEST): $(KERNEL_ELF) $(BOOT_ARCHIVE) $(CMDLINE_FILE) $(MODSIGN_KEY) $(SIGNING_STAMP) $(ROOT)/scripts/mkmanifest.py \
		$(ROOT)/scripts/modsign.py
	$(call log,MANIFEST,$@)
	$(Q)$(PYTHON) $(ROOT)/scripts/mkmanifest.py sign --key $(MODSIGN_KEY) --out $@ $(KERNEL_ELF) $(BOOT_ARCHIVE) $(CMDLINE_FILE)

$(IMAGE): $(KERNEL_ELF) $(LOADER_EFI) $(BOOT_ARCHIVE) $(CMDLINE_FILE) $(MANIFEST) $(ROOT)/scripts/mkimage.sh $(ROOT)/scripts/mkgpt.py
	$(call log,IMAGE,$@)
	$(Q)$(ROOT)/scripts/mkimage.sh $@ $(LOADER_EFI) $(KERNEL_ELF) $(BOOT_ARCHIVE) $(CMDLINE_FILE) $(MANIFEST)

# Roadmap M6 (docs/development.md, "Releases"): one architecture's release
# set -- the signed live image, the loader, kernel.elf, boot.tar, the
# command line, the manifest, cosmo-upgrade (S7), the signed package of
# cosmo-upgrade and cosmo-verify, SHA256SUMS and its signature -- made by
# scripts/mkrelease.py in RELEASE_DIR under the names the GitHub release carries.
# Always a release build: `make release` runs release-set with
# BUILD=release. Signed with whatever key SIGNING selects: the developer
# key by default, the release key in the release workflow (SIGNING=release
# MODSIGN_KEY=... KEYRING_PUBS=tools/keys/<release>.pub).
RELEASE_DIR ?= $(ROOT)/out/release/$(ARCH)
release:
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=release RELEASE_DIR=$(RELEASE_DIR) release-set

UPGRADE_ELF = $(call prog_elf,cosmo-upgrade)
VERIFY_ELF = $(call prog_elf,cosmo-verify)
release-set: $(IMAGE) $(LOADER_EFI) $(KERNEL_ELF) $(BOOT_ARCHIVE) $(CMDLINE_FILE) $(MANIFEST) $(UPGRADE_ELF) $(VERIFY_ELF) \
		$(COSMO_FETCH_ELF) \
		$(ROOT)/scripts/mkrelease.py $(ROOT)/tools/pkgbuild.py $(ROOT)/scripts/modsign.py
	$(call log,RELEASE,$(RELEASE_DIR))
	$(Q)$(PYTHON) $(ROOT)/scripts/mkrelease.py $(ARCH) $(RELEASE_DIR) $(IMAGE) $(LOADER_EFI) $(KERNEL_ELF) \
		$(BOOT_ARCHIVE) $(MANIFEST) $(CMDLINE_FILE) $(UPGRADE_ELF) $(VERIFY_ELF) $(COSMO_FETCH_ELF) $(MODSIGN_KEY) $(PKGSIGN_KEY) \
		$(BUILD_ID)

run: $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(ROOT)/scripts/qemu-run.sh $(IMAGE)

# BOOT_LOG: where the serial log goes; a variant boot names its own so
# CI's artifact keeps both.
BOOT_LOG ?= $(OUT)/boot-test.log
# The whole-boot timeout for test, test-harness-retry and test-chaos, and for
# test-gic, test-guard and test-smp2 through test; and
# test-crash and test-wxn have CRASH_TIMEOUT (below; they end in an early
# panic). A hang guard, not a performance budget (each
# self-test has its own budget). AArch64 under TCG is the slower guest, and
# over 257 CI boots (40 runs, 2026-10-06) it took a median of 154 s, p95 173 s,
# and exceeded 180 s four times on slow runners -- twice on branches before
# the one that prompted this -- with every self-test passed. 240 s matches
# test-chaos. x86-64 followed on 2026-10-07: over 81 boots of main's last
# twelve runs it took a median of 141 s and a p95 of 170 s, with a maximum
# of 175 s passing, on runners of two speeds (about 125 s and about 165 s a
# boot); on the slow kind the suite had grown to within 5 s of 180, and the
# merge of PR #325 timed out its test-harness-retry boot at 184 s with every
# self-test passed (run 37653536848). The same 240 s on both.
# AArch64 300 s from 2026-10-15: over the last 40 CI runs its entropy job's
# 146 boots took a median of 212 s and its passing ones at most 235.7 s, and
# six timed out at 240 s with every self-test passed, all six the
# no-entropy boot, whose self-test time puts their whole boot at 240-246 s
# (docs/testing/flakes.md). The build job's 261 AArch64 boots: p95 217 s,
# at most 228.8 s.
# test-chaos kept its own 240 s until 2026-10-17: over 57 AArch64 chaos
# boots of the last 60 CI runs it took a median of 209.7 s, a p95 of
# 228.3 s and at most 236.9 s passing, and one timed out at 247 s with
# every self-test passed (run 39056570530; docs/testing/flakes.md). It
# takes BOOT_TIMEOUT now: 300 s on AArch64, 240 s on x86-64 as before.
# The shell, network and key harnesses derive their deadlines from it.
BOOT_TIMEOUT ?= $(if $(filter aarch64,$(ARCH)),300,240)
# test-crash and test-wxn (the whole debug suite, then an early panic):
# AArch64 240 s from 2026-10-20. Over the last 40 CI runs (39155230078 to
# 39347750738) its 40 crash boots took a median of 172.4 s, a p95 of
# 177.1 s and at most 179.4 s passing (main's latest), and two timed out
# at 180 s with every self-test passed; their self-test totals (175.1 and
# 175.5 s) put the whole boot at 192.5-192.9 s (boot = 1.115 x self-test
# time - 2.8 s over the 38 passing ones, within 0.8 s). Its 38 WXN boots:
# median 170.3 s, at most 177.8 s. x86-64's 41 crash boots: median 153.2 s,
# at most 158.6 s; it keeps the runner's 180 s.
CRASH_TIMEOUT ?= $(if $(filter aarch64,$(ARCH)),240,180)
# test-busybox: one boot that runs the scripted test and fifteen testsuite files.
BUSYBOX_TIMEOUT ?= 900
test: $(IMAGE)
	$(Q)COSMO_ARCH=$(ARCH) QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" HAVE_MUSL=$(HAVE_MUSL) \
		BOOT_CMDLINE="$$(cat $(CMDLINE_FILE))" $(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --timeout $(BOOT_TIMEOUT) --image $(IMAGE) --log $(BOOT_LOG) \
		--kernel $(KERNEL_ELF) --symbolizer $(LLVM_PREFIX)llvm-symbolizer \
		$(if $(filter release,$(BUILD)),--shell-burst)

# The default CPU models (scripts/qemu-run.sh) have no SMEP, SMAP or UMIP
# (qemu64) and no PAN (cortex-a72), so on them the kernel's guard on its
# own access to user memory is a no-op and `test` cannot see it fail.
# This boots the same image on a model that has the guard, and the
# harness requires the kernel's own line saying every protection is on
# (QEMU_GUARD=1; docs/kernel/security/design.md, "Hardening"). The
# default boot stays the control, where the guard's absence is handled.
test-guard:
ifeq ($(ARCH),aarch64)
	$(Q)QEMU_GUARD=1 QEMU_CPU=cortex-a76 $(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) \
		BOOT_LOG=$(OUT)/boot-test-guard.log test
else
	$(Q)QEMU_GUARD=1 QEMU_CPU='qemu64,+nx,+svm,+npt,+smep,+smap,+umip' $(MAKE) --no-print-directory -C $(ROOT) \
		ARCH=$(ARCH) BUILD=$(BUILD) BOOT_LOG=$(OUT)/boot-test-guard.log test
endif

# M1's acceptance boots (docs/kernel/security/design.md §6), each with
# one entropy source: the image without the virtio-rng on the default CPU
# model, which has no random instruction either, and `random.jitter=off`
# -- no source at all: getrandom's NONBLOCK is EAGAIN, INSECURE answers,
# an encrypted pool is refused and the boot completes; on AArch64 the same
# without `random.jitter=off`, which the jitter source must seed (not on
# x86-64: under QEMU's TCG its timings are, in some boots, regular enough
# that the estimator refuses every batch, as it should; the AArch64
# machine that needs the source is Parallels Desktop); a model with the
# random instructions and `random.firmware=off` (OVMF and AAVMF offer the
# CPU's instructions as EFI_RNG_PROTOCOL), which must seed the pool from
# the CPU alone; and the
# virtio-rng with `random.firmware=off random.jitter=off`, which must
# seed from the device alone. The ordinary `test` boot is seeded by the
# firmware (boot protocol v10: OVMF and AAVMF offer EFI_RNG_PROTOCOL over
# the virtio-rng). The harness reads QEMU_RNG, QEMU_HWRNG, BOOT_FW_SEED
# and BOOT_JITTER to know which lines to require.
ifeq ($(ARCH),aarch64)
HWRNG_CPU ?= neoverse-v1
NORNG_CPU := cortex-a72
else
HWRNG_CPU ?= qemu64,+nx,+svm,+npt,+rdrand,+rdseed
NORNG_CPU := qemu64,+nx,+svm,+npt
endif
test-entropy:
	$(Q)QEMU_RNG=0 QEMU_HWRNG=0 QEMU_CPU='$(NORNG_CPU)' BOOT_JITTER=0 $(MAKE) --no-print-directory -C $(ROOT) \
		ARCH=$(ARCH) BUILD=$(BUILD) CMDLINE='$(CMDLINE) random.jitter=off' BOOT_LOG=$(OUT)/boot-test-noentropy.log test
ifeq ($(ARCH),aarch64)
	$(Q)QEMU_RNG=0 QEMU_HWRNG=0 QEMU_CPU='$(NORNG_CPU)' $(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) \
		BUILD=$(BUILD) BOOT_LOG=$(OUT)/boot-test-jitter.log test
endif
	$(Q)QEMU_RNG=0 QEMU_HWRNG=1 QEMU_CPU='$(HWRNG_CPU)' BOOT_FW_SEED=0 $(MAKE) --no-print-directory -C $(ROOT) \
		ARCH=$(ARCH) BUILD=$(BUILD) CMDLINE='$(CMDLINE) random.firmware=off' BOOT_LOG=$(OUT)/boot-test-cpurng.log test
	$(Q)QEMU_HWRNG=0 QEMU_CPU='$(NORNG_CPU)' BOOT_FW_SEED=0 BOOT_JITTER=0 $(MAKE) --no-print-directory -C $(ROOT) \
		ARCH=$(ARCH) BUILD=$(BUILD) CMDLINE='$(CMDLINE) random.firmware=off random.jitter=off' \
		BOOT_LOG=$(OUT)/boot-test-devrng.log test

# Load and unload in cycles (docs/kernel/module/design.md, invariant
# M27): the debug boot test with module.cycle= on the command line, so the
# kernel loads and unloads cosmotest MODCYCLE_COUNT times right after the
# boot modules; the harness requires the hook's PASS line for that entry
# and count. Debug builds only (the hook is a test seam).
MODCYCLE_COUNT ?= 1000
test-modcycle:
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=debug \
		CMDLINE='$(CMDLINE) module.cycle=tests/cosmotest.ko:$(MODCYCLE_COUNT)' BOOT_LOG=$(OUT)/boot-test-modcycle.log test

# Roadmap M2's acceptance test (tests/boot/install_test.py,
# docs/userland/testing.md): the live image installs itself on a blank
# disk with cosmo-install, and the disk is checked on the host. Run with
# BUILD=release in CI, where each boot takes seconds. Its own work
# directory: the boots' scratch disks and serial logs stay beside it.
# Roadmap M3's acceptance test: BusyBox ash runs the scripted test and the
# testsuite subset (docs/userland/testing.md, "BusyBox"; CI: BUILD=release).
test-busybox: $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/busybox_test.py --image $(IMAGE) --workdir $(OUT)/test-busybox \
		--timeout $(BUSYBOX_TIMEOUT)

test-install: $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/install_test.py --image $(IMAGE) --workdir $(OUT)/test-install \
		--timeout $(BOOT_TIMEOUT) --key $(MODSIGN_KEY)

# MEM-9 (tests/boot/fill_test.py): on a 256 MiB guest with this build
# installed, 512 MiB written with no sync to a data pool, and the root
# filled by one dd until ENOSPC; nothing killed, spawns work after.
test-fill: $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/fill_test.py --image $(IMAGE) --workdir $(OUT)/test-fill \
		--timeout $(BOOT_TIMEOUT)

# Roadmap 1.2 B5's acceptance test (tests/boot/encrypt_test.py,
# docs/kernel-services/filesystem/cosmofs/testing.md, "test-encrypt"): an
# encrypted install whose passphrase the harness types; three wrong ones
# restart the machine with nothing mounted; no plaintext on the disk; a
# second slot; a key-file mirror booting unattended; a 1 GiB file through a
# two-level checksum index; every metadata domain forged and refused.
# ENCRYPT_STAGES picks stages (the install always runs). The last stage,
# skipped with ENCRYPT_B1_B4=0, runs B1-B4's tests again on an encrypted
# root: COSMO_TEST_ENCRYPT makes every cosmo-install they type --encrypt
# and answers each passphrase prompt (tests/boot/install_test.py). test-be
# runs without its older release, whose kernel cannot read format 14.
# test-encrypt-b1b4 runs that stage alone; ENCRYPT_B1B4_TESTS picks its
# tests (CI runs it as parallel jobs, BUILD=release).
ENCRYPT_STAGES ?=
ENCRYPT_B1_B4 ?= 1
ENCRYPT_B1B4_TESTS ?= snapshot fsck mirror pkgtx be
ENCRYPT_PASS := b1-b4 on an encrypted root
test-encrypt: release-set $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/encrypt_test.py --image $(IMAGE) --workdir $(OUT)/test-encrypt \
		--timeout $(BOOT_TIMEOUT) --arch $(ARCH) $(if $(ENCRYPT_STAGES),--stages $(ENCRYPT_STAGES))
ifeq ($(ENCRYPT_B1_B4),1)
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) \
		ENCRYPT_B1B4_TESTS='$(ENCRYPT_B1B4_TESTS)' test-encrypt-b1b4
endif

ENCRYPT_B1B3 := $(addprefix test-,$(filter-out be,$(ENCRYPT_B1B4_TESTS)))
test-encrypt-b1b4: release-set $(IMAGE)
ifneq ($(ENCRYPT_B1B3),)
	$(Q)COSMO_TEST_ENCRYPT='$(ENCRYPT_PASS)' $(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) \
		$(ENCRYPT_B1B3)
endif
ifneq ($(filter be,$(ENCRYPT_B1B4_TESTS)),)
	$(Q)COSMO_TEST_ENCRYPT='$(ENCRYPT_PASS)' QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) \
		QEMU_ACCEL=$(QEMU_ACCEL) $(PYTHON) $(ROOT)/tests/boot/be_test.py --image $(IMAGE) --set $(UPGRADE_SET_DIR) \
		--key $(MODSIGN_KEY) --arch $(ARCH) --workdir $(OUT)/test-be-encrypted --timeout $(BOOT_TIMEOUT) \
		$(if $(filter space,$(BE_ONLY)),--only space)
endif
	@echo "test-encrypt: B1-B4 on an encrypted root: $(addprefix test-,$(ENCRYPT_B1B4_TESTS)): PASS ($(ARCH))"

# Roadmap 1.2 B1's acceptance test (tests/boot/snapshot_test.py,
# docs/kernel-services/filesystem/cosmofs/testing.md, "test-snapshot"):
# snapshots of the installed root from the shell -- identity, diff, sizes,
# ids never reused across a remount; a rollback of a broken root, and the
# three crash points, one of them through a sibling kernel built
# COSMOFS_STOP=1 (as test-crash builds CRASH_TEST=1). (CI: BUILD=release,
# in the install job.)
test-snapshot: $(IMAGE)
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) COSMOFS_STOP=1 OUT=$(OUT)-stop image
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/snapshot_test.py --image $(IMAGE) --workdir $(OUT)/test-snapshot \
		--timeout $(BOOT_TIMEOUT) --key $(MODSIGN_KEY) \
		--stop-kernel $(OUT)-stop/kernel/kernel.elf --stop-boot-tar $(OUT)-stop/boot.tar

# Roadmap 1.2 B2's acceptance test (tests/boot/fsck_test.py,
# docs/kernel-services/filesystem/cosmofs/testing.md, "test-fsck"): an
# installed root damaged by tools/cosmofs-damage.py in each of the
# checker's classes, booted -- repaired at boot or refused by name, then
# fsck.cosmofs --repair from the live shell -- a read-only check that
# writes nothing, and scrub --repair on a two-way mirror. (CI:
# BUILD=release, in the install job.)
test-fsck: $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/fsck_test.py --image $(IMAGE) --workdir $(OUT)/test-fsck \
		--timeout $(BOOT_TIMEOUT)

# Roadmap 1.2 B3's acceptance test (tests/boot/mirror_test.py,
# docs/kernel-services/filesystem/cosmofs/testing.md, "test-mirror"):
# install onto a two-disk mirror, remove a disk under a writer, replace
# and resilver it, scrub, reboot from the new pair; then a spare takes
# over; then a boot with a copy missing, and its return; then (PKG-11)
# upgrades, a repair and a rollback reaching both disks' ESPs, on this
# build's release set and two made from it. MIRROR_STAGES=esp runs
# that stage alone. (CI: BUILD=release, the mirror job.)
MIRROR_SET_DIR := $(OUT)/test-mirror-set
MIRROR_STAGES ?=
test-mirror: RELEASE_DIR = $(MIRROR_SET_DIR)
test-mirror: release-set $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/mirror_test.py --image $(IMAGE) --workdir $(OUT)/test-mirror \
		--timeout $(BOOT_TIMEOUT) --set $(MIRROR_SET_DIR) --key $(MODSIGN_KEY) \
		$(if $(MIRROR_STAGES),--stages $(MIRROR_STAGES))

# Roadmap 1.2 B3's PCIe hotplug test (tests/boot/pci_hotplug_test.py,
# docs/drivers/pci/testing.md, "test-pci-hotplug"): QEMU device_del and
# device_add against root ports, idle and under I/O, virtio-blk and NVMe,
# and the partitions of a disk that arrives after boot.
test-pci-hotplug: $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/pci_hotplug_test.py --image $(IMAGE) --workdir $(OUT)/test-pci-hotplug \
		--timeout $(BOOT_TIMEOUT)

# Shell completeness S7's acceptance test (tests/boot/upgrade_test.py,
# docs/userland/testing.md, "test-upgrade"): the published v1.0.3, fetched
# and checked against its SHA256SUMS (tools/fetch-release.py; .cache/ is the
# cache), installed, upgraded to this build's release set and rolled back.
# UPGRADE_SET_TAG=v1.0.4 upgrades to that published release instead of this
# build's set: fetched the same way (its SHA256SUMS.sig checked against
# tools/keys), signed by the release key.
# UPGRADE_BASE_TAG=v1.0.4 starts from the published v1.0.4, which upgrades
# itself with its own cosmo-upgrade (upgrade_test.py --base-tag).
UPGRADE_BASE_TAG ?= v1.0.3
UPGRADE_BASE_DIR := $(ROOT)/.cache/releases/$(UPGRADE_BASE_TAG)
UPGRADE_SET_TAG ?=
ifeq ($(UPGRADE_SET_TAG),)
UPGRADE_SET_DIR := $(OUT)/test-upgrade-set
UPGRADE_SET_PUB = $(PKG_TRUST_PUB)
test-upgrade: RELEASE_DIR = $(UPGRADE_SET_DIR)
test-upgrade: release-set
else
UPGRADE_SET_DIR := $(ROOT)/.cache/releases/$(UPGRADE_SET_TAG)
UPGRADE_SET_PUB = $(firstword $(wildcard $(ROOT)/tools/keys/*.pub))
endif
test-upgrade:
	$(Q)$(PYTHON) $(ROOT)/tools/fetch-release.py --tag $(UPGRADE_BASE_TAG) --arch $(ARCH) --dest $(UPGRADE_BASE_DIR)
	$(Q)$(if $(UPGRADE_SET_TAG),$(PYTHON) $(ROOT)/tools/fetch-release.py --tag $(UPGRADE_SET_TAG) --arch $(ARCH) --dest $(UPGRADE_SET_DIR),true)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/upgrade_test.py --base $(UPGRADE_BASE_DIR)/cosmoos-$(ARCH).img --base-tag $(UPGRADE_BASE_TAG) \
		--set $(UPGRADE_SET_DIR) --pub $(UPGRADE_SET_PUB) --arch $(ARCH) --workdir $(OUT)/test-upgrade \
		--timeout $(BOOT_TIMEOUT)
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) OUT=$(OUT) test-pkgtx
	$(Q)$(if $(UPGRADE_SET_TAG),true,$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) OUT=$(OUT) test-be)

# Roadmap 1.2 B4's boot environments (tests/boot/be_test.py), run by
# test-upgrade on this build's set and one made from it; its forward stage
# starts from the published BE_OLDER_TAG, whose cosmo-upgrade keeps no
# record of the root's base system. BE_ONLY=forward runs that stage alone,
# BE_ONLY=space the be-space boot alone after the install (here and in
# test-encrypt-b1b4's run of test-be; tools/be-space-probe.py).
BE_OLDER_TAG ?= v1.0.7
BE_OLDER_DIR := $(ROOT)/.cache/releases/$(BE_OLDER_TAG)
BE_ONLY ?=
test-be: RELEASE_DIR = $(UPGRADE_SET_DIR)
test-encrypt test-encrypt-b1b4: RELEASE_DIR = $(UPGRADE_SET_DIR)
test-be: release-set $(IMAGE)
	$(Q)$(PYTHON) $(ROOT)/tools/fetch-release.py --tag $(BE_OLDER_TAG) --arch $(ARCH) --dest $(BE_OLDER_DIR)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/be_test.py --image $(IMAGE) --set $(UPGRADE_SET_DIR) --key $(MODSIGN_KEY) \
		--arch $(ARCH) --workdir $(OUT)/test-be --timeout $(BOOT_TIMEOUT) \
		--older $(BE_OLDER_DIR)/cosmoos-$(ARCH).img $(if $(BE_ONLY),--only $(BE_ONLY))

# CFS-32's acceptance test (tests/boot/large_test.py): the published
# LARGE_OLD_TAG installs itself and upgrades to this build, which writes
# compressible files past the old run bound on two pools of its own (the
# largest a 1 GiB pool holds, and 1 GiB); the release's kernel, booted
# from its environment, reads both back. (CI: BUILD=release.)
# LARGE_BEFORE=1 measures the same fill on the release's own kernel only.
LARGE_OLD_TAG ?= v1.0.13
LARGE_OLD_DIR := $(ROOT)/.cache/releases/$(LARGE_OLD_TAG)
test-large: RELEASE_DIR = $(UPGRADE_SET_DIR)
test-large: release-set
	$(Q)$(PYTHON) $(ROOT)/tools/fetch-release.py --tag $(LARGE_OLD_TAG) --arch $(ARCH) --dest $(LARGE_OLD_DIR)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/large_test.py --base $(LARGE_OLD_DIR)/cosmoos-$(ARCH).img \
		--set $(UPGRADE_SET_DIR) --key $(MODSIGN_KEY) --arch $(ARCH) --workdir $(OUT)/test-large \
		--timeout $(BOOT_TIMEOUT) $(if $(LARGE_BEFORE),--before)

# Roadmap 1.2 B4's package transactions (PKG-3; tests/boot/pkgtx_test.py,
# docs/pkg/testing.md, "test-pkgtx"), run by test-upgrade: on this build's
# installed system, a pkg install and a pkg upgrade killed between two
# packages are undone at the next boot; pkg rollback; the space refusal.
test-pkgtx: $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/pkgtx_test.py --image $(IMAGE) --workdir $(OUT)/test-pkgtx \
		--timeout $(BOOT_TIMEOUT)

# Roadmap M5's acceptance test (tests/boot/security_test.py,
# docs/boot/testing.md, "test-security"): the signed image boots and says
# so; a copy with one byte of the kernel, the archive or the command line
# changed, a manifest signed by another key, a forged key id and a missing
# manifest are each refused by the loader before the kernel runs; the
# VERIFY_BOOT=0 loader boots the tampered kernel and says UNVERIFIED BOOT.
# CI: BUILD=release, a job of its own.
test-security: $(IMAGE) $(LOADER_EFI_NOVERIFY)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/security_test.py --image $(IMAGE) --noverify-loader $(LOADER_EFI_NOVERIFY) \
		--key $(MODSIGN_KEY) --workdir $(OUT)/test-security --timeout $(BOOT_TIMEOUT)

# Roadmap M4's acceptance test (tests/boot/net_login_test.py,
# docs/userland/testing.md, "test-net-login"): the installed system on QEMU
# user networking with no fw_cfg address -- DHCP, wget by address and by
# name. CI: BUILD=release, a job of its own.
test-net-login: $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/net_login_test.py --image $(IMAGE) --workdir $(OUT)/test-net-login \
		--timeout $(BOOT_TIMEOUT)

# Roadmap 1.1 P6's acceptance test (tests/boot/pkgnet_test.py,
# docs/pkg/testing.md, "test-pkg-net"): the installed system against
# package repositories this harness serves over HTTPS -- install, upgrade,
# refused certificates, packages, keys, replays and expiry. CI:
# BUILD=release, a job of its own.
# Step e serves this build's release set to cosmo-upgrade --fetch.
PKGNET_SET_DIR := $(OUT)/test-pkg-net-set
test-pkg-net: RELEASE_DIR = $(PKGNET_SET_DIR)
test-pkg-net: $(IMAGE) $(PKG_INDEX) release-set
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/pkgnet_test.py --image $(IMAGE) --repo $(PKG_REPO) --sign-key $(PKGSIGN_KEY) \
		--set $(PKGNET_SET_DIR) --arch $(ARCH) --workdir $(OUT)/test-pkg-net --timeout $(BOOT_TIMEOUT)

# The virtio-gpu unit's acceptance test (tests/boot/display_test.py,
# docs/drivers/virtio/testing.md, "virtio-gpu"): the console on a
# virtio-gpu-pci of DISPLAY_SIZE, read back from QEMU's screendump. CI:
# BUILD=release.
# On x86-64 it also boots the crash-test kernel (CRASH_TEST=1, as
# test-crash builds it) without the exit device and reads the panic off
# the screen; AArch64's exit is semihosting, which cannot be left out.
# And a debug kernel built with CRASH_TEST=3, which panics with lockdep's
# own lock owned by another CPU: the panic flush must not wait for it.
DISPLAY_SIZE ?= 1920x1080
test-display: $(IMAGE)
ifeq ($(ARCH),x86_64)
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) CRASH_TEST=1 OUT=$(OUT)-crash image
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=debug CRASH_TEST=3 \
		OUT=$(ROOT)/out/$(ARCH)-debug-paniclock image
endif
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/display_test.py --image $(IMAGE) --workdir $(OUT)/test-display \
		--size $(DISPLAY_SIZE) --timeout $(BOOT_TIMEOUT) \
		$(if $(filter x86_64,$(ARCH)),--panic-image $(OUT)-crash/cosmoos.img \
		--panic-lockdep-image $(ROOT)/out/$(ARCH)-debug-paniclock/cosmoos.img)

# The same boot test with two CPUs. Every other boot here uses the
# default four, and a test that needs a third CPU without saying so passes
# them all: `virtio-remove-inflight`, `sched-spread` and `sched-balance-pair`
# each failed at two CPUs, unseen by CI
# (docs/audit/2026-10-05-two-cpu-validation-report.md). This boot catches a
# test that wrongly assumes a third CPU. It does not run every SMP test:
# those that genuinely need three or more CPUs (`sched-spread`,
# `sched-migrate` and others) skip here with their reason, and the
# four-CPU boots run them. One CPU is covered by `QEMU_SMP=1 make test`
# locally.
# It also runs two devices in their other modes (docs/drivers/nvme/design.md,
# docs/drivers/virtio/design.md): the NVMe controller with one MSI-X
# vector, the driver's one-vector mode, and on x86-64 a legacy-only
# virtio-net with a 1024-entry receive queue (CI's QEMU 10 makes it so by
# default, Homebrew's QEMU makes it 256; set here so both run the same),
# the legacy transport through an I/O BAR (AArch64 cannot
# reach PCI I/O space; Parallels Desktop's memory-BAR legacy device is
# the other half).
ifeq ($(ARCH),x86_64)
SMP2_VNET_OPTS := ,disable-modern=on,rx_queue_size=1024
endif
test-smp2:
	$(Q)QEMU_NVME_OPTS=,msix_qsize=1 QEMU_VNET_OPTS='$(SMP2_VNET_OPTS)' $(MAKE) --no-print-directory -C $(ROOT) \
		ARCH=$(ARCH) BUILD=$(BUILD) QEMU_SMP=2 BOOT_LOG=$(OUT)/boot-test-smp2.log test

# QEMU's virt machine defaults to gic-version=2, so `test` exercises one
# of the two AArch64 interrupt controllers and never the other. This runs
# the other, in both of the MSI configurations a GICv3 can have: an ITS
# (what GICv3 hardware offers, and QEMU's default for that machine) and a
# GICv2m frame (the fallback when firmware describes no ITS). A no-op on
# architectures with no GIC, so CI can call it for every target.
# Platform discovery (docs/kernel/arch/aarch64/testing.md, "make
# test-platforms"): the AArch64 image under each QEMU configuration that
# differs from virt's defaults -- GICv2 and GICv3, highmem off, a CPU with
# and without RNDR, no serial port -- must reach the shell and print the
# platform line with what that machine has. On both architectures, 1, 4
# and 8 GiB of RAM: the loader's direct map reaches the top of RAM and
# `free` reports it (docs/boot/design.md, "The direct map").
test-platforms: $(IMAGE)
	$(Q)$(PYTHON) $(ROOT)/tests/boot/platform_test.py --arch $(ARCH) --image $(IMAGE) --workdir $(OUT)/test-platforms --key $(MODSIGN_KEY)

test-gic:
ifeq ($(ARCH),aarch64)
	$(Q)QEMU_GIC=3 $(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) test
	$(Q)if qemu-system-aarch64 -machine virt,help 2>&1 | grep -q '^  *msi='; then \
		QEMU_GIC=3 QEMU_MSI=gicv2m $(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) test; \
	else \
		echo "test-gic: this QEMU has no virt 'msi' property (needs 11 or newer);"; \
		echo "test-gic: the GICv2m-under-GICv3 fallback is not exercised here."; \
	fi
else
	@echo "test-gic: $(ARCH) has no GIC; nothing to do"
endif

# The whole suite under a chaos migrator: a debug kernel whose tick moves
# a ready thread to another CPU every few ticks for no reason
# (SCHED_CHAOS=1, docs/kernel/scheduler/design.md "Migration"), built
# into a sibling output tree. The harness requires the migrator to have
# moved something. CI's ordinary AArch64 boots take up to 167 s; chaos
# added 11 s of self-test work and exhausted the shell's 170 s deadline
# under the then-default 180 s total. 240 s for this heavier full boot;
# individual self-test watchdogs and shell signal-latency checks remain.
# (Ordinary x86-64 boots have 240 s too since BOOT_TIMEOUT, above, and
# AArch64's 300 s since 2026-10-15; chaos keeps 240 s on both: its 39
# AArch64 CI boots over the 40 runs measured then took at most 217.9 s.)
test-chaos:
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=debug \
		SCHED_CHAOS=1 OUT=$(OUT)-chaos image
	$(Q)COSMO_ARCH=$(ARCH) QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --chaos --timeout $(BOOT_TIMEOUT) \
		--image $(OUT)-chaos/cosmoos.img --log $(OUT)-chaos/boot-test-chaos.log \
		--kernel $(OUT)-chaos/kernel/kernel.elf --symbolizer $(LLVM_PREFIX)llvm-symbolizer

# Build a kernel whose net-harness back-connection fails on purpose the
# first time and boot it: the retry that exists for a QEMU defect seen on
# one boot in twenty, exercised on this one
# (docs/audit/next-subsystem-nettest-retry.md).
test-harness-retry:
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=debug \
		HARNESS_BREAK=1 OUT=$(OUT)-hbreak image
	$(Q)COSMO_ARCH=$(ARCH) QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --harness-retry --timeout $(BOOT_TIMEOUT) \
		--image $(OUT)-hbreak/cosmoos.img --log $(OUT)-hbreak/boot-test-hbreak.log \
		--kernel $(OUT)-hbreak/kernel/kernel.elf --symbolizer $(LLVM_PREFIX)llvm-symbolizer

# Build a deliberately crashing kernel into a sibling output tree and
# verify that the panic path reports properly and the harness sees FAIL.
test-crash:
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) \
		CRASH_TEST=1 OUT=$(OUT)-crash image
	$(Q)COSMO_ARCH=$(ARCH) QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --expect-panic fault --timeout $(CRASH_TIMEOUT) \
		--image $(OUT)-crash/cosmoos.img --log $(OUT)-crash/boot-test-crash.log \
		--kernel $(OUT)-crash/kernel/kernel.elf --symbolizer $(LLVM_PREFIX)llvm-symbolizer

# AArch64: build a kernel that maps one page writable and executable on
# purpose and executes it (CRASH_TEST=2); SCTLR_EL1.WXN must make that
# an instruction abort, and the harness requires that panic's report.
# A no-op elsewhere (x86-64 has no WXN; its NX is proved by test-crash).
test-wxn:
ifeq ($(ARCH),aarch64)
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) \
		CRASH_TEST=2 OUT=$(OUT)-wxn image
	$(Q)COSMO_ARCH=$(ARCH) QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --expect-panic wxn --timeout $(CRASH_TIMEOUT) \
		--image $(OUT)-wxn/cosmoos.img --log $(OUT)-wxn/boot-test-wxn.log \
		--kernel $(OUT)-wxn/kernel/kernel.elf --symbolizer $(LLVM_PREFIX)llvm-symbolizer
else
	@echo "test-wxn: $(ARCH) has no WXN; nothing to do"
endif

# The quiescence protocol's memory-order litmus tests, checked by herd7
# against the RC11 model: verdicts, reachability witnesses and negative
# controls (tests/litmus/run_litmus.py, docs/kernel/quiesce/testing.md).
# herdtools7 comes from opam or Debian testing/sid; CI runs it in a job
# of its own.
litmus:
	$(Q)$(PYTHON) $(ROOT)/tests/litmus/run_litmus.py

ANALYSIS_REPORTS ?= $(KERNEL_ANALYZE) $(LOADER_ANALYZE) $(MODULE_ANALYZE) $(PKG_ANALYZE)

analysis-gate:
	$(Q)$(PYTHON) $(ROOT)/scripts/check-analysis.py --root $(ROOT) --out $(OUT) \
		--arch $(ARCH) --baseline $(ROOT)/tools/analysis/$(ARCH).json \
		--inventory $(OUT)/analysis-inventory.json $(ANALYSIS_REPORTS)

analyze: $(KERNEL_ANALYZE) $(LOADER_ANALYZE) $(MODULE_ANALYZE) $(PKG_ANALYZE) $(KERNEL_ELF)
	$(Q)$(ROOT)/scripts/check-fpregs.sh $(KERNEL_ELF) $(OBJDUMP)
	$(Q)$(MAKE) --no-print-directory analysis-gate

reproducible:
	$(Q)$(ROOT)/scripts/check-reproducible.sh $(ARCH) $(BUILD)

check-docs:
	$(Q)$(PYTHON) $(ROOT)/scripts/check-docs.py $(ROOT)

# compile_commands.json for clangd/IDEs, using the real cross flags so
# editor diagnostics match the build. The file is git-ignored.
ARCH_INC := -I$(ROOT)/kernel/arch/$(ARCH)/include
compile-commands:
	$(call log,GEN,$(ROOT)/compile_commands.json)
	$(Q)( \
	  $(foreach s,$(KERNEL_GENERIC_SRCS),printf '%s\t%s\n' '$(s)' '$(KERNEL_CFLAGS)';) \
	  $(foreach s,$(filter %.c,$(KERNEL_ARCH_SRCS)),printf '%s\t%s\n' '$(s)' '$(KERNEL_CFLAGS) $(ARCH_INC)';) \
	  $(foreach s,$(LOADER_SRCS),printf '%s\t%s\n' '$(s)' '$(LOADER_CFLAGS)';) \
	  $(foreach m,$(MODULES),$(foreach s,$(MODULE_$(m)_SRCS),printf '%s\t%s\n' '$(s)' '$(MODULE_CFLAGS)';)) \
	) | $(PYTHON) $(ROOT)/scripts/gen-compile-commands.py $(ROOT) $(CC) > $(ROOT)/compile_commands.json

check-tools:
	$(Q)$(ROOT)/scripts/check-tools.sh "$(CC)" "$(LD)" "$(LDLINK)" "$(OBJCOPY)" "$(PYTHON)"
	$(Q)$(ROOT)/scripts/check-secrets.sh

# No private key and no revoked public key may be tracked (docs/kernel/module/design.md).
check-secrets:
	$(Q)$(ROOT)/scripts/check-secrets.sh

clean:
	$(Q)rm -rf $(OUT)

help:
	@sed -n '2,/^#   make clean/p' $(ROOT)/Makefile | sed 's/^# \{0,1\}//'
	@echo
	@echo "ARCH=$(ARCH) BUILD=$(BUILD) OUT=$(OUT)"
	@echo "HOST=$(HOST_OS)/$(HOST_ARCH) CC=$(CC) LD=$(LD)"
