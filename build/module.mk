# Kernel module build rules (Phase 5). See docs/kernel/module/.
#
# A module is an ET_REL object: its sources are compiled with the kernel
# flags (same code model, same freestanding rules, plus -DCOSMO_MODULE_BUILD),
# merged with `ld.lld -r`, and signed by scripts/modsign.py with the
# development key. The archive entries below name where each module goes
# in the boot archive: modules/ entries load at boot in this order,
# tests/ entries are loaded by the self-tests only.
#
# To add a module: list its sources in MODULE_<name>_SRCS, add <name> to
# MODULES, and add its archive entry.

MODULE_CFLAGS := $(KERNEL_CFLAGS) -DCOSMO_MODULE_BUILD=1
MODULE_LDFLAGS := -r --no-dynamic-linker -z noexecstack --build-id=none
MODULE_OUT := $(OUT)/modules

MODSIGN_KEY ?= $(ROOT)/tools/keys/cosmo-dev.key
MODSIGN := $(PYTHON) $(ROOT)/scripts/modsign.py

MODULES := hello virtio virtio_blk virtio_rng virtio_console virtio_net cosmotest cosmotest_dep cosmotest_fail

MODULE_hello_SRCS          := modules/hello/hello.c
MODULE_virtio_SRCS         := drivers/virtio/virtio.c drivers/virtio/virtqueue.c drivers/virtio/virtio_pci.c
MODULE_virtio_blk_SRCS     := drivers/virtio/virtio_blk.c
MODULE_virtio_rng_SRCS     := drivers/virtio/virtio_rng.c
MODULE_virtio_console_SRCS := drivers/virtio/virtio_console.c
MODULE_virtio_net_SRCS     := drivers/virtio/virtio_net.c
MODULE_cosmotest_SRCS      := tests/modules/cosmotest.c
MODULE_cosmotest_dep_SRCS  := tests/modules/cosmotest_dep.c
MODULE_cosmotest_fail_SRCS := tests/modules/cosmotest_fail.c

# Boot order is dependency order: virtio before its device drivers.
MODULE_ARCHIVE_ENTRIES := \
	modules/hello.ko=$(MODULE_OUT)/hello.ko \
	modules/virtio.ko=$(MODULE_OUT)/virtio.ko \
	modules/virtio_blk.ko=$(MODULE_OUT)/virtio_blk.ko \
	modules/virtio_rng.ko=$(MODULE_OUT)/virtio_rng.ko \
	modules/virtio_console.ko=$(MODULE_OUT)/virtio_console.ko \
	modules/virtio_net.ko=$(MODULE_OUT)/virtio_net.ko \
	tests/cosmotest.ko=$(MODULE_OUT)/cosmotest.ko \
	tests/cosmotest_dep.ko=$(MODULE_OUT)/cosmotest_dep.ko \
	tests/cosmotest_fail.ko=$(MODULE_OUT)/cosmotest_fail.ko

# $(1) module name
define module_rules
MODULE_$(1)_OBJS := $$(call objs_of,$$(MODULE_$(1)_SRCS))
$$(eval $$(call compile_rules,$$(MODULE_$(1)_OBJS),MODULE_CFLAGS))
$$(MODULE_OUT)/$(1).ko: $$(MODULE_$(1)_OBJS) $$(ROOT)/scripts/modsign.py $$(MODSIGN_KEY)
	$$(call log,MODLD,$$@)
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(LD) $$(MODULE_LDFLAGS) -o $$@.unsigned $$(MODULE_$(1)_OBJS)
	$$(Q)$$(MODSIGN) sign --key $$(MODSIGN_KEY) --in $$@.unsigned --out $$@
	$$(Q)$$(PYTHON) $$(ROOT)/scripts/check-module-elf.py $$@ $$(ARCH)
MODULE_KOS += $$(MODULE_OUT)/$(1).ko
MODULE_ANALYZE += $$(patsubst %.o,%.analyzed,$$(MODULE_$(1)_OBJS))
MODULE_ALL_OBJS += $$(MODULE_$(1)_OBJS)
endef

MODULE_KOS :=
MODULE_ANALYZE :=
MODULE_ALL_OBJS :=
$(foreach m,$(MODULES),$(eval $(call module_rules,$(m))))

.PHONY: modules
modules: $(MODULE_KOS)

-include $(MODULE_ALL_OBJS:.o=.d)
