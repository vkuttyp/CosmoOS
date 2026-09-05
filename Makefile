# CosmoOS top-level build.
#
#   make [ARCH=x86_64] [BUILD=debug|release] [V=1]   build loader + kernel
#   make image        FAT boot image with loader and kernel
#   make run          boot the image under QEMU on the terminal (serial)
#   make test         automated QEMU boot test with PASS/FAIL exit code
#   make test-crash   build a deliberately faulting kernel, verify panic path
#   make host-test    native unit tests of kernel algorithms under ASan/UBSan
#   make analyze      clang static analyzer over all target sources
#   make reproducible build twice into separate trees and compare outputs
#   make compile-commands  compile_commands.json for clangd with cross flags
#   make check-tools  verify the cross toolchain is usable
#   make clean        remove $(OUT)
#
# Host and target are separate concepts throughout. See docs/build/.

ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

include $(ROOT)/build/config.mk
include $(ROOT)/build/toolchain.mk
include $(ROOT)/build/rules.mk

.PHONY: all kernel boot modules image run test test-crash analyze reproducible compile-commands check-tools clean help
.DEFAULT_GOAL := all

include $(ROOT)/kernel/kernel.mk
include $(ROOT)/boot/uefi/boot.mk
include $(ROOT)/libc/libc.mk
include $(ROOT)/userland/userland.mk
include $(ROOT)/pkg/pkg.mk
include $(ROOT)/tests/linux/linux.mk
include $(ROOT)/build/module.mk
include $(ROOT)/tests/host/host.mk

all: kernel boot libc userland pkg ports linux-tests modules

IMAGE := $(OUT)/cosmoos.img

image: $(IMAGE)

# The boot archive: init plus the boot-time and test modules, in the
# order the kernel loads them (dependencies first). See
# scripts/mkbootarchive.py and docs/kernel/module/.
BOOT_ARCHIVE := $(OUT)/boot.tar
BOOT_ARCHIVE_ENTRIES = init=$(INIT_ELF) $(USER_ARCHIVE_ENTRIES) sbin/pkg=$(PKG_ELF) $(PKG_ARCHIVE_ENTRIES) $(LINUX_TEST_ARCHIVE_ENTRIES) $(MODULE_ARCHIVE_ENTRIES)

$(BOOT_ARCHIVE): $(USER_ARCHIVE_DEPS) $(PKG_ELF) $(PKG_INDEX) $(LINUX_TEST_ELFS) $(MODULE_KOS) $(ROOT)/scripts/mkbootarchive.py
	$(call log,ARCHIVE,$@)
	$(Q)$(PYTHON) $(ROOT)/scripts/mkbootarchive.py $@ $(BOOT_ARCHIVE_ENTRIES)

$(IMAGE): $(KERNEL_ELF) $(LOADER_EFI) $(BOOT_ARCHIVE) $(ROOT)/scripts/mkimage.sh
	$(call log,IMAGE,$@)
	$(Q)$(ROOT)/scripts/mkimage.sh $@ $(LOADER_EFI) $(KERNEL_ELF) $(BOOT_ARCHIVE)

run: $(IMAGE)
	$(Q)QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(ROOT)/scripts/qemu-run.sh $(IMAGE)

test: $(IMAGE)
	$(Q)QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" HAVE_MUSL=$(HAVE_MUSL) \
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --image $(IMAGE) --log $(OUT)/boot-test.log

# Build a deliberately crashing kernel into a sibling output tree and
# verify that the panic path reports properly and the harness sees FAIL.
test-crash:
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) \
		CRASH_TEST=1 OUT=$(OUT)-crash image
	$(Q)QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --expect-panic \
		--image $(OUT)-crash/cosmoos.img --log $(OUT)-crash/boot-test-crash.log

analyze: $(KERNEL_ANALYZE) $(LOADER_ANALYZE) $(MODULE_ANALYZE) $(PKG_ANALYZE)
	@echo "static analysis: clean"

reproducible:
	$(Q)$(ROOT)/scripts/check-reproducible.sh $(ARCH) $(BUILD)

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

clean:
	$(Q)rm -rf $(OUT)

help:
	@sed -n '2,16p' $(ROOT)/Makefile | sed 's/^# \{0,1\}//'
	@echo
	@echo "ARCH=$(ARCH) BUILD=$(BUILD) OUT=$(OUT)"
	@echo "HOST=$(HOST_OS)/$(HOST_ARCH) CC=$(CC) LD=$(LD)"
