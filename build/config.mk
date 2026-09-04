# Build configuration knobs. Override on the command line or in the
# environment. Everything here describes the TARGET build, never the host.

# Target architecture. Directory names under kernel/arch/ are the valid set.
ARCH ?= x86_64

# debug: -O1 -g, assertions, self-tests run at boot.
# release: -O2 -g, assertions kept (they are cheap and this is a kernel),
#          self-tests off.
BUILD ?= debug

ifeq ($(filter $(BUILD),debug release),)
$(error BUILD must be debug or release, got '$(BUILD)')
endif

# Boot-time self-tests. Default follows BUILD; force with SELFTEST=0/1.
ifeq ($(BUILD),debug)
SELFTEST ?= 1
else
SELFTEST ?= 0
endif

# CRASH_TEST=1 makes kernel_main fault on purpose after the self-tests so
# the panic path and the harness's failure detection can be verified.
# Never enable in a build you intend to run for real.
CRASH_TEST ?= 0

# Output tree. Never inside the source directories.
OUT ?= $(ROOT)/out/$(ARCH)-$(BUILD)

# V=1 shows full command lines.
V ?= 0
ifeq ($(V),0)
Q := @
log = @printf '  %-8s %s\n' '$(1)' '$(patsubst $(ROOT)/%,%,$(2))'
else
Q :=
log =
endif

# QEMU test-run settings.
QEMU_MEM ?= 256M
QEMU_SMP ?= 4
QEMU_ACCEL ?= tcg
QEMU_EXTRA ?=

# Reproducibility: fixed epoch unless the caller provides one.
SOURCE_DATE_EPOCH ?= 0
export SOURCE_DATE_EPOCH

# Build identity, derived from the tree so equal sources give equal builds.
BUILD_ID := $(shell cd $(ROOT) && git describe --always --dirty 2>/dev/null || echo unknown)
