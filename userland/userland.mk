# User programs. Built with the same freestanding cross toolchain as the
# kernel but for the user ABI: static, non-PIC, SysV, linked at 4 MiB.
# The user code model is small (default); no kernel-only flags such as
# -mcmodel=kernel, -mno-red-zone, or -mgeneral-regs-only apply, except
# that the freestanding init has no FP code and keeps the general-regs
# restriction to stay independent of FPU state handling, which arrives
# with a later phase.

INIT_ELF := $(OUT)/userland/init.elf

USER_CFLAGS := \
	--target=$(KERNEL_TARGET) \
	$(COMMON_CFLAGS) \
	-fno-pic -fno-pie -mgeneral-regs-only \
	-I$(ROOT)/libc/include \
	-I$(ROOT)/kernel/include

USER_LDFLAGS := \
	-nostdlib -static --no-dynamic-linker \
	-z max-page-size=0x1000 -z noexecstack -z separate-code \
	--build-id=none --gc-sections

INIT_SRCS := \
	userland/init/crt0.S \
	userland/init/init.c

INIT_OBJS := $(call objs_of,$(INIT_SRCS))
INIT_C_OBJS := $(call objs_of,$(filter %.c,$(INIT_SRCS)))
INIT_S_OBJS := $(call objs_of,$(filter %.S,$(INIT_SRCS)))

$(eval $(call compile_rules,$(INIT_C_OBJS),USER_CFLAGS))
$(eval $(call assemble_rules,$(INIT_S_OBJS),USER_CFLAGS))

$(INIT_ELF): $(INIT_OBJS) $(ROOT)/userland/init/user.ld
	$(call log,LD,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(LD) $(USER_LDFLAGS) -T $(ROOT)/userland/init/user.ld -o $@ $(INIT_OBJS)

.PHONY: userland
userland: $(INIT_ELF)

-include $(INIT_OBJS:.o=.d)
