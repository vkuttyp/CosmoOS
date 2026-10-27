# Roadmap 1.2 A1's clang evaluation (tools/clang-eval/README.md): a static
# musl clang and lld for $(ARCH), cross-built on the host, and a boot that
# runs them on the installed system. Not part of the image or CI.
#
#   make clang-eval        build OUT/clang-eval/$(ARCH)/clang.tar.gz (about
#                          an hour per architecture on an 8-core host)
#   make test-clang-eval   install this build, boot it, run the compiler
#                          (QEMU_MEM as for every test; BUILD=release)

LLVM_VERSION := 19.1.7
LLVM_SHA256  := 82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501
LLVM_URL     := https://github.com/llvm/llvm-project/releases/download/llvmorg-$(LLVM_VERSION)/llvm-project-$(LLVM_VERSION).src.tar.xz
LLVM_TAR     := $(PORTS_CACHE)/llvm-project-$(LLVM_VERSION).src.tar.xz
CLANG_EVAL_DIR := $(dir $(OUT))clang-eval
CLANG_EVAL_PKG := $(CLANG_EVAL_DIR)/$(ARCH)/clang.tar.gz

$(LLVM_TAR):
	$(Q)$(PYTHON) $(ROOT)/scripts/fetch-pinned.py $@ $(LLVM_SHA256) $(LLVM_URL) \
		$(PINNED_MIRROR)/$(notdir $(LLVM_TAR))

.PHONY: clang-eval test-clang-eval
clang-eval: $(CLANG_EVAL_PKG)
$(CLANG_EVAL_PKG): $(LLVM_TAR) $(BB_LIBC) $(BB_LIBC_SO) $(BB_BUILTINS) $(ROOT)/tools/clang-eval/build.sh
	$(call log,CLANG,llvm $(LLVM_VERSION) $(BB_TARGET) (tools/clang-eval))
	$(Q)sh $(ROOT)/tools/clang-eval/build.sh $(ARCH) $(CLANG_EVAL_DIR) $(LLVM_TAR) $(BB_DIR)

test-clang-eval: $(IMAGE) $(TCC_ELF) $(CLANG_EVAL_PKG)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tools/clang-eval/clang_eval_test.py --image $(IMAGE) \
		--workdir $(OUT)/test-clang-eval --clang $(CLANG_EVAL_PKG) --tcc-src $(TCC_DIR)/src \
		--timeout $(BOOT_TIMEOUT)
