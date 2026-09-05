# Guest images for the virtualization tests
# (docs/kernel-services/virtualization/testing.md). Each is one assembly
# file linked as a flat binary at 0x1000; the kernel self-tests and vmctl
# load them from the boot archive under /boot/tests/hv/.

HV_TEST_OUT := $(OUT)/tests/hv
HV_GUESTS := guest_pio guest_irq guest_cpuid guest_pm guest_shutdown guest_spin

define hv_guest_rule
$(HV_TEST_OUT)/$(1).bin: $(ROOT)/tests/hv/$(1).S
	$$(call log,AS,$$<)
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(CC) --target=x86_64-unknown-none-elf -c $$< -o $$@.o
	$$(call log,BIN,$$@)
	$$(Q)$$(LD) --image-base=0 -Ttext=0x1000 --oformat=binary -o $$@ $$@.o
endef
$(foreach g,$(HV_GUESTS),$(eval $(call hv_guest_rule,$(g))))

HV_GUEST_BINS := $(foreach g,$(HV_GUESTS),$(HV_TEST_OUT)/$(g).bin)
HV_ARCHIVE_ENTRIES := $(foreach g,$(HV_GUESTS),tests/hv/$(g).bin=$(HV_TEST_OUT)/$(g).bin)

.PHONY: hv-guests
hv-guests: $(HV_GUEST_BINS)
