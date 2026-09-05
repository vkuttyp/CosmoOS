# Toolchain selection and per-target compiler flags.
#
# One LLVM toolchain cross-compiles every target from any host: clang is a
# native cross compiler and lld links ELF (kernel) and PE/COFF (UEFI loader)
# without a per-target binutils build. Set LLVM_PREFIX to point at a
# specific installation, e.g. LLVM_PREFIX=/opt/homebrew/opt/llvm/bin/ or
# LLVM_PREFIX=/usr/lib/llvm-18/bin/.

HOST_OS   := $(shell uname -s)
HOST_ARCH := $(shell uname -m)

LLVM_PREFIX ?=
CC      := $(LLVM_PREFIX)clang
LD      := $(LLVM_PREFIX)ld.lld
LDLINK  := $(LLVM_PREFIX)lld-link
OBJCOPY := $(LLVM_PREFIX)llvm-objcopy
AR      := $(LLVM_PREFIX)llvm-ar
NM      := $(LLVM_PREFIX)llvm-nm
OBJDUMP := $(LLVM_PREFIX)llvm-objdump
PYTHON  ?= python3

# Per-architecture target triples and code-model flags.
include $(ROOT)/build/arch/$(ARCH).mk

# Warnings shared by every target. Warnings are errors; the constitution
# says so and it keeps the tree honest.
COMMON_WARNINGS := \
	-Wall -Wextra -Werror \
	-Wshadow -Wvla -Wundef -Wpointer-arith -Wwrite-strings \
	-Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations \
	-Wimplicit-fallthrough -Wcast-qual -Wformat=2 -Wdate-time

# Flags shared by every freestanding target.
COMMON_CFLAGS := \
	-std=c11 -ffreestanding -nostdlib -nostdinc \
	-fno-stack-protector -fno-omit-frame-pointer -fno-strict-aliasing \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ffile-prefix-map=$(ROOT)=/cosmo \
	-DCOSMO_BUILD_ID=\"$(BUILD_ID)\" \
	-DCOSMO_BUILD_TYPE=\"$(BUILD)\" \
	-DCONFIG_SELFTEST=$(SELFTEST) \
	-DCONFIG_CRASH_TEST=$(CRASH_TEST) \
	-DCONFIG_MODULE_SIG_ENFORCE=$(MODULE_SIG_ENFORCE) \
	$(COMMON_WARNINGS)

ifeq ($(BUILD),debug)
COMMON_CFLAGS += -O1 -g -DCONFIG_DEBUG=1
else
COMMON_CFLAGS += -O2 -g -DCONFIG_DEBUG=0
endif

# Freestanding headers come from clang's own resource directory (stdint.h,
# stddef.h, stdbool.h, stdarg.h). -nostdinc removed everything else.
CLANG_RESOURCE_INC := $(shell $(CC) -print-resource-dir 2>/dev/null)/include
COMMON_CFLAGS += -isystem $(CLANG_RESOURCE_INC)

# --- kernel ---
KERNEL_CFLAGS := \
	--target=$(KERNEL_TARGET) \
	$(COMMON_CFLAGS) \
	$(KERNEL_ARCH_CFLAGS) \
	-I$(ROOT)/kernel/include \
	-I$(ROOT)/drivers/include \
	-I$(ROOT)/boot/protocol

KERNEL_LDFLAGS := \
	-nostdlib -static --no-dynamic-linker \
	-z max-page-size=0x1000 -z noexecstack -z separate-code \
	--build-id=sha1 --gc-sections \
	$(KERNEL_ARCH_LDFLAGS)

# --- UEFI loader ---
LOADER_CFLAGS := \
	--target=$(LOADER_TARGET) \
	$(COMMON_CFLAGS) \
	$(LOADER_ARCH_CFLAGS) \
	-fshort-wchar \
	-I$(ROOT)/boot/uefi \
	-I$(ROOT)/boot/protocol

LOADER_LDFLAGS := \
	/subsystem:efi_application /entry:efi_main /nodefaultlib \
	/Brepro /timestamp:0 /nologo $(LOADER_ARCH_LDFLAGS)
