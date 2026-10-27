# TinyCC, the C compiler on the target (roadmap 1.2 A1; ports/tcc/README.md):
# built by the project's clang against the musl sysroot the BusyBox port
# builds (ports/busybox/busybox.mk), a program linked against the system's
# libc.so (ports/busybox/cc-dyn.in) that compiles, assembles and links for
# the architecture in hand, against musl's headers,
# start files and libraries installed under /usr (TCC_ARCHIVE_ENTRIES).
# The source is the `mob` branch at a pinned commit (0.9.28rc: 0.9.27 and the
# AArch64 fixes since), fetched once into $(PORTS_CACHE) through
# scripts/fetch-pinned.py from TinyCC's GitHub mirror (repo.or.cz serves its
# snapshots behind a bot check).

TCC_COMMIT := e23dabed154a8495f3ac1ff3cc9d04fa1c207893
TCC_SHA256 := d2470fdeed862ddb18f4ec4e6a405fa984580c3d85c38492516749c61931c30e
TCC_URL    := https://github.com/TinyCC/tinycc/archive/$(TCC_COMMIT).tar.gz
TCC_TAR    := $(PORTS_CACHE)/tinycc-$(TCC_COMMIT).tar.gz

# One build per architecture beside $(OUT), shared by the build variants as
# BusyBox's is.
TCC_DIR    ?= $(dir $(OUT))tcc-$(ARCH)
TCC_ELF    := $(TCC_DIR)/tcc
TCC_LIBTCC1 := $(TCC_DIR)/libtcc1.a
# What tcc -run starts a program with (tccrun.c: _runmain).
TCC_RUNMAIN := $(TCC_DIR)/runmain.o
TCC_CPU    := $(if $(filter aarch64,$(ARCH)),arm64,x86_64)
# AArch64's __clear_cache (lib/armflush.c) calls a builtin only tcc has;
# built by clang, it does the cache maintenance itself. And AArch64 Linux's
# wchar_t is unsigned int, as musl has it, not tcc's int (README.md).
TCC_PATCHES := $(ROOT)/ports/tcc/armflush-clang.patch $(ROOT)/ports/tcc/wchar-arm64.patch

$(TCC_TAR):
	$(Q)$(PYTHON) $(ROOT)/scripts/fetch-pinned.py $@ $(TCC_SHA256) $(TCC_URL) \
		$(PINNED_MIRROR)/$(notdir $(TCC_TAR))

# configure for the target, out of the source tree: a Linux-ABI, musl tcc
# whose include, library and start-file paths are the installed system's
# (/usr/lib/tcc for its own headers and libtcc1.a, then musl's under /usr),
# its ELF interpreter musl's loader, no bounds checker or backtraces (they
# need tcc's own runtime support, not built here), and configure's own
# -Wall -O2 kept: --extra-cflags replaces them. CONFIG_RUNMEM_RO=1: tcc -run
# maps the code it made read and execute, its data read and write (tccrun.c;
# by default only on macOS), since the kernel maps no page writable and
# executable at once (vm_user_protect). The build's one host
# tool, c2str (it turns include/tccdefs.h into a C string), is built for
# the host by $(CC) first, so make does not build it for the target.
# tcc's makefiles have variables of their own named as ours are (ARCH is
# arm64 to tcc), so the sub-makes get none of this make's command-line
# variables (MAKEFLAGS and the rest unset in their environment): `make
# ARCH=aarch64`, as CI and make
# reproducible run it, would otherwise override tcc's ARCH and build no
# compiler objects.
# libtcc1.a, which tcc builds with itself, is built here with the target's
# clang (tcc's own <cpu>-libtcc1-usegcc switch): the tcc just built cannot
# run on the build host.
# Patches go on with git apply, as BusyBox's do (ports/busybox/busybox.mk):
# the CI image has git and no patch.
$(TCC_ELF) $(TCC_LIBTCC1) $(TCC_RUNMAIN) &: $(TCC_TAR) $(TCC_PATCHES) $(BB_LIBC) $(BB_LIBC_SO) $(BB_BUILTINS) $(BB_CC_DYN) \
		$(ROOT)/ports/tcc/tcc.mk
	$(call log,TCC,$(TCC_COMMIT) $(BB_TARGET))
	$(Q)rm -rf $(TCC_DIR)/src $(TCC_DIR)/build
	$(Q)mkdir -p $(TCC_DIR)/src $(TCC_DIR)/build
	$(Q)tar -xzf $(TCC_TAR) -C $(TCC_DIR)/src --strip-components=1
	$(Q)for p in $(TCC_PATCHES); do \
		(cd $(TCC_DIR)/src && GIT_CEILING_DIRECTORIES=$(TCC_DIR) git apply -p1 $$p) || exit 1; done
	$(Q)cd $(TCC_DIR)/build && $(TCC_DIR)/src/configure --cc=$(BB_CC_DYN) --ar="$(AR)" --cpu=$(TCC_CPU) \
		--targetos=Linux --config-musl --config-bcheck=no --config-backtrace=no --prefix=/usr \
		--sysincludepaths=/usr/lib/tcc/include:/usr/include --libpaths=/usr/lib/tcc:/usr/lib \
		--crtprefix=/usr/lib --elfinterp=/lib/$(LDSO_NAME) \
		--extra-cflags="-Wall -O2 -DCONFIG_RUNMEM_RO=1 -ffile-prefix-map=$(TCC_DIR)=tcc" > $(TCC_DIR)/configure.log 2>&1 \
		|| { tail -40 $(TCC_DIR)/configure.log; exit 1; }
	$(Q)$(CC) -DC2STR $(TCC_DIR)/src/conftest.c -o $(TCC_DIR)/build/c2str.exe
	$(Q)env -u MAKEFLAGS -u MFLAGS -u MAKEOVERRIDES $(MAKE) -s -C $(TCC_DIR)/build tcc > $(TCC_DIR)/build.log 2>&1 \
		|| { tail -40 $(TCC_DIR)/build.log; exit 1; }
	$(Q)env -u MAKEFLAGS -u MFLAGS -u MAKEOVERRIDES $(MAKE) -s -C $(TCC_DIR)/build libtcc1.a $(TCC_CPU)-libtcc1-usegcc=yes >> $(TCC_DIR)/build.log 2>&1 \
		|| { tail -40 $(TCC_DIR)/build.log; exit 1; }
	$(Q)$(OBJCOPY) --strip-all --remove-section=.note --remove-section=.comment $(TCC_DIR)/build/tcc $(TCC_ELF)
	$(Q)cp $(TCC_DIR)/build/libtcc1.a $(TCC_LIBTCC1)
	$(Q)cp $(TCC_DIR)/build/runmain.o $(TCC_RUNMAIN)

.PHONY: tcc
tcc: $(TCC_ELF) $(TCC_LIBTCC1) $(TCC_RUNMAIN)

# On the system: /usr/bin/tcc and cc naming it; tcc's own headers and
# libtcc1.a and runmain.o under /usr/lib/tcc; musl's headers under /usr/include and its
# start files and libraries under /usr/lib -- libc.a for -static, libc.so a
# link to the system's /lib/libc.so for a dynamic link, and musl's empty
# libm, libpthread, ... so a -lm finds what libc already holds.
TCC_MUSL_STUBS := crypt dl m pthread resolv rt util xnet
TCC_ARCHIVE_ENTRIES := usr/bin/tcc=$(TCC_ELF) usr/bin/cc@tcc usr/lib/tcc/libtcc1.a=$(TCC_LIBTCC1) usr/lib/tcc/runmain.o=$(TCC_RUNMAIN) \
	usr/lib/tcc/include/=$(TCC_DIR)/src/include/ usr/include/=$(BB_SYSROOT)/include/ \
	$(foreach f,crt1.o crti.o crtn.o Scrt1.o rcrt1.o libc.a,usr/lib/$(f)=$(BB_SYSROOT)/lib/$(f)) \
	usr/lib/libc.so@../../lib/libc.so \
	$(foreach l,$(TCC_MUSL_STUBS),usr/lib/lib$(l).a=$(BB_SYSROOT)/lib/lib$(l).a)
# rc.test's first program for it (tests/tcc/hello.c).
TCC_ARCHIVE_ENTRIES += tests/tcc/hello.c=$(ROOT)/tests/tcc/hello.c
TCC_ARCHIVE_DEPS := $(TCC_ELF) $(TCC_LIBTCC1) $(TCC_RUNMAIN) $(BB_LIBC) $(ROOT)/tests/tcc/hello.c
