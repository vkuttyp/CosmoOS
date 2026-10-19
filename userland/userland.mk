# User programs (docs/userland/). Every program is one or more C files
# linked as crt0.o objects libc.a with the shared user.ld at 4 MiB. The
# program list also generates the boot archive entries: bin/<name> for
# the coreutils and the shell, sbin/<name> for the system tools, etc/ for
# the scripts; init keeps its `init=` entry (the kernel finds it by name).

USER_LD := $(ROOT)/userland/user.ld

# name := directory
USER_BIN_PROGRAMS  := sh echo cat ls cp mv rm mkdir mkfifo rmdir pwd true false sleep sync
USER_SBIN_PROGRAMS := mount umount ps kill dmesg sysctl vmctl hostname svc fsctl cosmo-install cosmo-upgrade cosmo-verify netcfg priv ip syslogd \
	lsblk blkid mkfs.cosmofs

PROG_DIR_sh     := shell
PROG_DIR_echo   := coreutils
PROG_DIR_cat    := coreutils
PROG_DIR_ls     := coreutils
PROG_DIR_cp     := coreutils
PROG_DIR_mv     := coreutils
PROG_DIR_rm     := coreutils
PROG_DIR_mkdir  := coreutils
PROG_DIR_mkfifo := coreutils
PROG_DIR_rmdir  := coreutils
PROG_DIR_pwd    := coreutils
PROG_DIR_true   := coreutils
PROG_DIR_false  := coreutils
PROG_DIR_sleep  := coreutils
PROG_DIR_sync   := coreutils
PROG_DIR_mount  := system
PROG_DIR_umount := system
PROG_DIR_ps     := system
PROG_DIR_kill   := system
PROG_DIR_dmesg  := system
PROG_DIR_sysctl := system
PROG_DIR_vmctl  := system
PROG_EXTRA_SRCS_vmctl := tools/fdt/fdt.c tools/fdt/fdt_read.c userland/system/vq.c userland/system/vblk.c userland/system/vnet.c
PROG_DIR_hostname := system
PROG_DIR_netcfg := system
PROG_DIR_ip := system
PROG_DIR_syslogd := system
PROG_DIR_priv := system
PROG_DIR_svc    := system
PROG_DIR_fsctl  := system
PROG_DIR_cosmo-install := system
PROG_EXTRA_SRCS_cosmo-install := userland/system/install_gpt.c userland/system/install_sha512.c
PROG_DIR_cosmo-upgrade := system
PROG_EXTRA_SRCS_cosmo-upgrade := userland/system/upgrade_fat.c userland/system/blkdevs.c userland/system/install_sha512.c \
	userland/system/upgrade_ed25519.c userland/system/relsig.c userland/lib/fetch.c
# A release file against its signed SHA256SUMS (the upgrading guide).
PROG_DIR_cosmo-verify := system
PROG_EXTRA_SRCS_cosmo-verify := userland/system/install_sha512.c userland/system/upgrade_ed25519.c userland/system/relsig.c
# Shell completeness S6: the block devices through /dev/blkctl.
PROG_DIR_lsblk := system
PROG_EXTRA_SRCS_lsblk := userland/system/blkdevs.c
PROG_DIR_blkid := system
PROG_EXTRA_SRCS_blkid := userland/system/blkdevs.c userland/system/install_gpt.c userland/system/blkid_crc32c.c
PROG_DIR_mkfs.cosmofs := system
PROG_EXTRA_SRCS_mkfs.cosmofs := userland/system/blkdevs.c

# Test-only programs: built and archived under SELFTEST, so a release
# image carries none of them.
USER_TEST_PROGRAMS := thrtest cwdtest
PROG_DIR_thrtest := tests
PROG_DIR_cwdtest := tests

USER_PROGRAMS := init $(USER_BIN_PROGRAMS) $(USER_SBIN_PROGRAMS)
ifeq ($(SELFTEST),1)
USER_PROGRAMS += $(USER_TEST_PROGRAMS)
endif
PROG_DIR_init := init

# A program is one file, plus whatever PROG_EXTRA_SRCS_<name> lists.
prog_srcs = userland/$(PROG_DIR_$(1))/$(1).c $(PROG_EXTRA_SRCS_$(1))
prog_elf  = $(OUT)/userland/$(1).elf

USER_SRCS := $(sort $(foreach p,$(USER_PROGRAMS),$(call prog_srcs,$(p))))   # a shared source once
USER_OBJS := $(call objs_of,$(USER_SRCS))
$(eval $(call compile_rules,$(USER_OBJS),USER_CFLAGS))

define prog_link_rule
$(call prog_elf,$(1)): $(call objs_of,$(call prog_srcs,$(1))) $(LIBC_CRT0) $(LIBC_A) $(USER_LD)
	$$(call log,LD,$$@)
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(LD) $$(USER_LDFLAGS) -T $(USER_LD) -o $$@ $(LIBC_CRT0) $(call objs_of,$(call prog_srcs,$(1))) $(LIBC_A)
endef
$(foreach p,$(USER_PROGRAMS),$(eval $(call prog_link_rule,$(p))))

INIT_ELF  := $(call prog_elf,init)
USER_ELFS := $(foreach p,$(USER_PROGRAMS),$(call prog_elf,$(p)))

# A native program whose name a BusyBox applet takes (roadmap M3) is
# installed as /bin/cosmo-<name>: /bin/sh is BusyBox ash, the native
# shell /bin/cosmo-sh (docs/userland/design.md, "BusyBox").
native_bin_name = $(if $(filter $(1),$(BUSYBOX_APPLETS)),cosmo-$(1),$(1))
USER_ARCHIVE_ENTRIES := \
	$(foreach p,$(USER_BIN_PROGRAMS),bin/$(call native_bin_name,$(p))=$(call prog_elf,$(p))) \
	$(foreach p,$(USER_SBIN_PROGRAMS),sbin/$(p)=$(call prog_elf,$(p))) \
	etc/rc=$(ROOT)/userland/etc/rc \
	etc/fstab=$(ROOT)/userland/etc/fstab \
	etc/passwd=$(ROOT)/userland/etc/passwd \
	etc/group=$(ROOT)/userland/etc/group \
	etc/svc/hello=$(ROOT)/userland/etc/svc/hello \
	etc/svc/greeter=$(ROOT)/userland/etc/svc/greeter \
	etc/svc/syslogd=$(ROOT)/userland/etc/svc/syslogd \
	etc/pkg/repos.conf=$(ROOT)/userland/etc/pkg/repos.conf etc/pkg/provides=$(ROOT)/userland/etc/pkg/provides \
	etc/cosmo-release.conf=$(ROOT)/userland/etc/cosmo-release.conf \
	$(foreach k,$(PKG_TRUST_PUBS),etc/pkg/keys/$(notdir $(k))=$(k))
ifeq ($(SELFTEST),1)
USER_ARCHIVE_ENTRIES += etc/rc.test=$(ROOT)/userland/etc/rc.test etc/rc.linux=$(ROOT)/userland/etc/rc.linux \
	$(foreach p,$(USER_TEST_PROGRAMS),tests/native/$(p)=$(call prog_elf,$(p)))
endif
USER_ARCHIVE_DEPS := $(USER_ELFS) $(ROOT)/userland/etc/rc $(ROOT)/userland/etc/fstab $(ROOT)/userland/etc/rc.test $(ROOT)/userland/etc/rc.linux \
	$(ROOT)/userland/etc/pkg/repos.conf $(ROOT)/userland/etc/pkg/provides $(ROOT)/userland/etc/cosmo-release.conf $(PKG_TRUST_PUBS) $(ROOT)/userland/etc/passwd $(ROOT)/userland/etc/group \
	$(ROOT)/userland/etc/svc/hello $(ROOT)/userland/etc/svc/greeter $(ROOT)/userland/etc/svc/syslogd

.PHONY: userland
userland: $(USER_ELFS)

-include $(USER_OBJS:.o=.d)
