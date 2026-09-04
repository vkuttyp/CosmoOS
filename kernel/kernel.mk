# Kernel component build.
#
# Generic sources get only the generic include path. Architecture sources
# additionally get their private include directory. That asymmetry is what
# enforces Invariant 1 at build time: generic code cannot include an
# architecture header because it is not on its search path.

KERNEL_ELF := $(OUT)/kernel/kernel.elf
KERNEL_MAP := $(OUT)/kernel/kernel.map

include $(ROOT)/kernel/arch/$(ARCH)/arch.mk

KERNEL_GENERIC_SRCS := \
	kernel/core/main.c \
	kernel/core/bootinfo.c \
	kernel/core/console.c \
	kernel/core/log.c \
	kernel/core/panic.c \
	kernel/core/printf.c \
	kernel/core/string.c \
	kernel/core/shutdown.c \
	kernel/core/selftest.c \
	kernel/interrupt/interrupt.c

KERNEL_GENERIC_OBJS := $(call objs_of,$(KERNEL_GENERIC_SRCS))
KERNEL_ARCH_OBJS    := $(call objs_of,$(KERNEL_ARCH_SRCS))
KERNEL_OBJS         := $(KERNEL_GENERIC_OBJS) $(KERNEL_ARCH_OBJS)

KERNEL_ARCH_C_OBJS := $(call objs_of,$(filter %.c,$(KERNEL_ARCH_SRCS)))
KERNEL_ARCH_S_OBJS := $(call objs_of,$(filter %.S,$(KERNEL_ARCH_SRCS)))

$(eval $(call compile_rules,$(KERNEL_GENERIC_OBJS),KERNEL_CFLAGS))
$(eval $(call compile_rules,$(KERNEL_ARCH_C_OBJS),KERNEL_CFLAGS))
$(eval $(call assemble_rules,$(KERNEL_ARCH_S_OBJS),KERNEL_CFLAGS))

$(KERNEL_ARCH_OBJS) $(patsubst %.o,%.analyzed,$(KERNEL_ARCH_C_OBJS)): \
	EXTRA_CFLAGS := -I$(ROOT)/kernel/arch/$(ARCH)/include

KERNEL_ANALYZE := $(patsubst %.o,%.analyzed,$(KERNEL_GENERIC_OBJS) $(KERNEL_ARCH_C_OBJS))

$(KERNEL_ELF): $(KERNEL_OBJS) $(KERNEL_LINKER_SCRIPT)
	$(call log,LD,$@)
	$(Q)$(LD) $(KERNEL_LDFLAGS) -T $(KERNEL_LINKER_SCRIPT) -Map=$(KERNEL_MAP) -o $@ $(KERNEL_OBJS)
	$(Q)$(ROOT)/scripts/check-kernel-elf.sh $(OBJDUMP) $@

kernel: $(KERNEL_ELF)

-include $(KERNEL_OBJS:.o=.d)
