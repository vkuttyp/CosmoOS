# Guest images for the virtualization tests
# (docs/kernel-services/virtualization/testing.md). Each is one assembly
# file linked as a flat binary at 0x1000; the kernel self-tests and vmctl
# load them from the boot archive under /boot/tests/hv/.

HV_TEST_OUT := $(OUT)/tests/hv
# A guest is written for the architecture that will run it, so the images
# live in tests/hv/$(ARCH)/ and only that set is built.
ifeq ($(ARCH),x86_64)
HV_GUESTS := guest_pio guest_irq guest_cpuid guest_pm guest_shutdown guest_spin guest_fpu
else
HV_GUESTS := guest_wfi guest_hvc guest_mmio guest_sysreg guest_spin guest_irq guest_timer guest_ctimer guest_gicd guest_gicc guest_gic guest_sgi guest_mmio_widths guest_uart guest_uart_rx guest_uart_wfi guest_uart_poll guest_ptimer guest_timer_wfi
endif

define hv_guest_rule
$(HV_TEST_OUT)/$(1).bin: $(ROOT)/tests/hv/$(ARCH)/$(1).S
	$$(call log,AS,$$<)
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(CC) --target=$(ARCH)-unknown-none-elf -c $$< -o $$@.o
	$$(call log,BIN,$$@)
	$$(Q)$$(LD) --image-base=0 -Ttext=0x1000 --oformat=binary -o $$@ $$@.o
endef
$(foreach g,$(HV_GUESTS),$(eval $(call hv_guest_rule,$(g))))

HV_GUEST_BINS := $(foreach g,$(HV_GUESTS),$(HV_TEST_OUT)/$(g).bin)
HV_ARCHIVE_ENTRIES := $(foreach g,$(HV_GUESTS),tests/hv/$(g).bin=$(HV_TEST_OUT)/$(g).bin)

# The machine's device tree, written by the same code the owner uses
# (tools/fdt), so the kernel's tests hand a guest the blob vmctl would:
# two vCPUs, 8 MiB at COSMO_HVM_RAM_BASE. mkdtb is a host program; the
# compiler builds for the host when given no target.
ifeq ($(ARCH),aarch64)
HV_MKDTB := $(HV_TEST_OUT)/mkdtb
$(HV_MKDTB): $(ROOT)/tools/fdt/mkdtb.c $(ROOT)/tools/fdt/fdt.c $(ROOT)/tools/fdt/fdt.h $(ROOT)/kernel/include/uapi/cosmo/hv_machine.h
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) -std=c11 -O2 -I$(ROOT)/kernel/include/uapi -I$(ROOT)/tools/fdt $< $(ROOT)/tools/fdt/fdt.c -o $@
HV_DTB := $(HV_TEST_OUT)/virt.dtb
$(HV_DTB): $(HV_MKDTB)
	$(call log,DTB,$@)
	$(Q)$(HV_MKDTB) $@ 2 8 "console=ttyAMA0"
HV_GUEST_BINS += $(HV_DTB)
HV_ARCHIVE_ENTRIES += tests/hv/virt.dtb=$(HV_DTB)
endif

.PHONY: hv-guests
hv-guests: $(HV_GUEST_BINS)
