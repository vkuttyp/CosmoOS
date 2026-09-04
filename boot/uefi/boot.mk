# UEFI loader component build. Produces a PE/COFF EFI application.

LOADER_EFI := $(OUT)/boot/BOOTX64.EFI

LOADER_SRCS := \
	boot/uefi/main.c \
	boot/uefi/console.c \
	boot/uefi/string.c \
	boot/uefi/memory.c \
	boot/uefi/cpu.c \
	boot/uefi/elf.c \
	boot/uefi/paging.c

LOADER_OBJS := $(call objs_of,$(LOADER_SRCS))

$(eval $(call compile_rules,$(LOADER_OBJS),LOADER_CFLAGS))

LOADER_ANALYZE := $(patsubst %.o,%.analyzed,$(LOADER_OBJS))

$(LOADER_EFI): $(LOADER_OBJS)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(LDLINK) $(LOADER_LDFLAGS) /out:$@ $(LOADER_OBJS)

boot: $(LOADER_EFI)

-include $(LOADER_OBJS:.o=.d)
