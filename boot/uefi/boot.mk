# UEFI loader component build. Produces a PE/COFF EFI application.

LOADER_EFI := $(OUT)/boot/$(LOADER_EFI_NAME)

LOADER_SRCS := \
	boot/uefi/main.c \
	boot/uefi/console.c \
	boot/uefi/string.c \
	boot/uefi/memory.c \
	boot/uefi/elf.c \
	boot/uefi/arch/$(ARCH)/cpu.c \
	boot/uefi/arch/$(ARCH)/paging.c \
	boot/uefi/arch/$(ARCH)/serial.c

LOADER_ASM_SRCS :=
ifeq ($(ARCH),aarch64)
LOADER_ASM_SRCS += boot/uefi/arch/aarch64/el2_stub.S
endif

LOADER_OBJS := $(call objs_of,$(LOADER_SRCS))
LOADER_ASM_OBJS := $(call objs_of,$(LOADER_ASM_SRCS))

$(eval $(call compile_rules,$(LOADER_OBJS),LOADER_CFLAGS))
$(eval $(call assemble_rules,$(LOADER_ASM_OBJS),LOADER_CFLAGS))

LOADER_ANALYZE := $(patsubst %.o,%.analyzed,$(LOADER_OBJS))

$(LOADER_EFI): $(LOADER_OBJS) $(LOADER_ASM_OBJS)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(LDLINK) $(LOADER_LDFLAGS) /out:$@ $(LOADER_OBJS) $(LOADER_ASM_OBJS)

boot: $(LOADER_EFI)

-include $(LOADER_OBJS:.o=.d)
