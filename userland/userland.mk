# User programs (docs/userland/). Every program is one or more C files
# linked as crt0.o objects libc.a with the shared user.ld at 4 MiB. The
# program list also generates the boot archive entries: bin/<name> for
# the coreutils and the shell, sbin/<name> for the system tools, etc/ for
# the scripts; init keeps its `init=` entry (the kernel finds it by name).

USER_LD := $(ROOT)/userland/user.ld

# name := directory
USER_BIN_PROGRAMS  := sh echo cat ls cp mv rm mkdir rmdir pwd true false sleep
USER_SBIN_PROGRAMS := mount umount ps kill dmesg sysctl vmctl

PROG_DIR_sh     := shell
PROG_DIR_echo   := coreutils
PROG_DIR_cat    := coreutils
PROG_DIR_ls     := coreutils
PROG_DIR_cp     := coreutils
PROG_DIR_mv     := coreutils
PROG_DIR_rm     := coreutils
PROG_DIR_mkdir  := coreutils
PROG_DIR_rmdir  := coreutils
PROG_DIR_pwd    := coreutils
PROG_DIR_true   := coreutils
PROG_DIR_false  := coreutils
PROG_DIR_sleep  := coreutils
PROG_DIR_mount  := system
PROG_DIR_umount := system
PROG_DIR_ps     := system
PROG_DIR_kill   := system
PROG_DIR_dmesg  := system
PROG_DIR_sysctl := system
PROG_DIR_vmctl  := system

USER_PROGRAMS := init $(USER_BIN_PROGRAMS) $(USER_SBIN_PROGRAMS)
PROG_DIR_init := init

prog_srcs = userland/$(PROG_DIR_$(1))/$(1).c
prog_elf  = $(OUT)/userland/$(1).elf

USER_SRCS := $(foreach p,$(USER_PROGRAMS),$(call prog_srcs,$(p)))
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

USER_ARCHIVE_ENTRIES := \
	$(foreach p,$(USER_BIN_PROGRAMS),bin/$(p)=$(call prog_elf,$(p))) \
	$(foreach p,$(USER_SBIN_PROGRAMS),sbin/$(p)=$(call prog_elf,$(p))) \
	etc/rc=$(ROOT)/userland/etc/rc \
	etc/pkg/repos.conf=$(ROOT)/userland/etc/pkg/repos.conf \
	etc/pkg/keys/cosmo-dev.pub=$(ROOT)/userland/etc/pkg/keys/cosmo-dev.pub
ifeq ($(SELFTEST),1)
USER_ARCHIVE_ENTRIES += etc/rc.test=$(ROOT)/userland/etc/rc.test
endif
USER_ARCHIVE_DEPS := $(USER_ELFS) $(ROOT)/userland/etc/rc $(ROOT)/userland/etc/rc.test \
	$(ROOT)/userland/etc/pkg/repos.conf $(ROOT)/userland/etc/pkg/keys/cosmo-dev.pub

.PHONY: userland
userland: $(USER_ELFS)

-include $(USER_OBJS:.o=.d)
