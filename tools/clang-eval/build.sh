#!/bin/sh
# Roadmap 1.2 A1's clang evaluation, the host's half: a static musl clang
# and lld for one architecture, cross-built on the host
# (tools/clang-eval/README.md; make clang-eval).
#
#   build.sh ARCH OUT SRC_TAR BB_DIR
#
# ARCH     x86_64 or aarch64
# OUT      the build directory (out/clang-eval); OUT/native is the host's
#          tablegen, shared by both architectures, OUT/ARCH the rest
# SRC_TAR  llvm-project-19.1.7.src.tar.xz (pinned: tools/clang-eval/clang.mk)
# BB_DIR   the BusyBox port's build for ARCH (out/busybox-ARCH): its musl
#          sysroot, compiler-rt builtins and `cc` wrapper
#
# Three steps: the host's llvm-tblgen and clang-tblgen; libc++,
# libc++abi and libunwind for ARCH-linux-musl, static; then clang and lld
# for ARCH-linux-musl, linked -static against musl and that libc++, one
# LLVM target (the architecture's own). The result is OUT/ARCH/clang.tar.gz:
# bin/clang, bin/ld.lld, and clang's resource directory (its own headers,
# and the BusyBox port's compiler-rt builtins where clang looks for them).
# Not part of the image: an evaluation (A1), recorded in
# docs/audit/2026-10-27-a1-clang-eval-report.md.

set -eu
ARCH=$1 OUT=$2 SRC_TAR=$3 BB_DIR=$4
T=$ARCH-linux-musl
LLVM_TARGET=$([ "$ARCH" = aarch64 ] && echo AArch64 || echo X86)
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}
SYSROOT=$BB_DIR/sysroot
BUILTINS=$BB_DIR/rt/libclang_rt.builtins.a
RES=$(clang -print-resource-dir)
A=$OUT/$ARCH
mkdir -p "$OUT" "$A"

if [ ! -f "$OUT/src/llvm/CMakeLists.txt" ]; then
    rm -rf "$OUT/src" && mkdir -p "$OUT/src"
    tar -xJf "$SRC_TAR" -C "$OUT/src" --strip-components=1
fi
SRC=$OUT/src

# 1. The host's tablegen (LLVM_NATIVE_TOOL_DIR for the cross builds).
if [ ! -x "$OUT/native/bin/clang-tblgen" ]; then
    cmake -G Ninja -S "$SRC/llvm" -B "$OUT/native" -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=X86 \
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF > "$OUT/native.log" 2>&1
    ninja -C "$OUT/native" -j "$JOBS" llvm-tblgen llvm-min-tblgen clang-tblgen >> "$OUT/native.log" 2>&1
fi

# Wrappers for the target, as the BusyBox port's cc is (ports/busybox/cc.in):
# the host's clang for $T against the musl sysroot; a link is static, with
# musl's start files around the objects and libc++, libc++abi, libunwind,
# libc and the builtins after them, and the image at 4 MiB: lld's default
# 2 MiB is below the kernel's user window (USER_LO; ports/busybox/cc.in).
RT=$A/runtimes-install
# The one Linux UAPI header the build needs (libc++'s atomic.cpp: its
# futex waits); the musl sysroot has none. Linux's values.
LX=$A/linux-include
mkdir -p "$LX/linux"
cat > "$LX/linux/futex.h" <<EOF
/* What libc++'s atomic.cpp takes from <linux/futex.h> (tools/clang-eval/build.sh). */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
EOF
cat > "$A/cc" <<EOF
#!/bin/sh
link=1
for a in "\$@"; do case "\$a" in -c|-S|-E|-M|-MM) link=0 ;; esac; done
F="--target=$T -nostdinc -isystem $SYSROOT/include -isystem $LX -isystem $RES/include -ffile-prefix-map=$OUT=clang-eval"
if [ \$link = 1 ]; then
    exec clang \$F -fuse-ld=lld -static -Wl,--image-base=0x400000 -nostdlib $SYSROOT/lib/crt1.o $SYSROOT/lib/crti.o "\$@" \\
        -L$SYSROOT/lib -lc $BUILTINS $SYSROOT/lib/crtn.o
fi
exec clang \$F "\$@"
EOF
cat > "$A/c++" <<EOF
#!/bin/sh
link=1
for a in "\$@"; do case "\$a" in -c|-S|-E|-M|-MM) link=0 ;; esac; done
F="--target=$T -nostdinc -nostdinc++ -isystem $RT/include/c++/v1 -isystem $SYSROOT/include -isystem $LX -isystem $RES/include -ffile-prefix-map=$OUT=clang-eval"
if [ \$link = 1 ]; then
    exec clang++ \$F -fuse-ld=lld -static -Wl,--image-base=0x400000 -nostdlib $SYSROOT/lib/crt1.o $SYSROOT/lib/crti.o "\$@" \\
        -L$RT/lib -L$SYSROOT/lib -lc++ -lc++abi -lunwind -lc $BUILTINS $SYSROOT/lib/crtn.o
fi
exec clang++ \$F "\$@"
EOF
chmod +x "$A/cc" "$A/c++"

# 2. libc++, libc++abi and libunwind for $T, static (the C++ runtime clang
# and lld are linked with; not shipped). CMake's checks only compile (a
# static-library try-compile), so what musl lacks is said here: glibc's
# __cxa_thread_atexit_impl (libc++abi then keeps its own).
if [ ! -f "$RT/lib/libc++.a" ]; then
    cmake -G Ninja -S "$SRC/runtimes" -B "$A/runtimes" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=$ARCH \
        -DCMAKE_C_COMPILER="$A/cc" -DCMAKE_CXX_COMPILER="$A/c++" -DCMAKE_ASM_COMPILER="$A/cc" \
        -DCMAKE_AR="$(command -v llvm-ar)" -DCMAKE_RANLIB="$(command -v llvm-ranlib)" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_INSTALL_PREFIX="$RT" \
        -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
        -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF \
        -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXXABI_USE_COMPILER_RT=ON \
        -DLIBUNWIND_USE_COMPILER_RT=ON -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXX_INCLUDE_TESTS=OFF \
        -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF \
        -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF \
        > "$A/runtimes.log" 2>&1
    ninja -C "$A/runtimes" -j "$JOBS" install >> "$A/runtimes.log" 2>&1
fi

# 3. clang and lld for $T: static, one LLVM target, the target's defaults
# (lld, compiler-rt, libc++, musl's loader by the triple).
if [ ! -x "$A/llvm/bin/clang-19" ] || [ ! -x "$A/llvm/bin/lld" ]; then
    cmake -G Ninja -S "$SRC/llvm" -B "$A/llvm" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=$ARCH \
        -DCMAKE_C_COMPILER="$A/cc" -DCMAKE_CXX_COMPILER="$A/c++" -DCMAKE_ASM_COMPILER="$A/cc" \
        -DCMAKE_AR="$(command -v llvm-ar)" -DCMAKE_RANLIB="$(command -v llvm-ranlib)" \
        -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD=$LLVM_TARGET \
        -DLLVM_HOST_TRIPLE=$T -DLLVM_DEFAULT_TARGET_TRIPLE=$T -DLLVM_NATIVE_TOOL_DIR="$OUT/native/bin" \
        -DLLVM_TABLEGEN="$OUT/native/bin/llvm-tblgen" -DCLANG_TABLEGEN="$OUT/native/bin/clang-tblgen" \
        -DLLVM_ENABLE_LIBCXX=ON -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
        -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_THREADS=ON \
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
        -DLLVM_INCLUDE_DOCS=OFF -DCLANG_INCLUDE_TESTS=OFF -DCLANG_ENABLE_ARCMT=OFF \
        -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_PLUGIN_SUPPORT=OFF -DLLVM_ENABLE_PLUGINS=OFF \
        -DCLANG_DEFAULT_LINKER=lld -DCLANG_DEFAULT_RTLIB=compiler-rt -DCLANG_DEFAULT_CXX_STDLIB=libc++ \
        -DCLANG_DEFAULT_UNWINDLIB=libunwind -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=ON \
        -DLLVM_APPEND_VC_REV=OFF -DLLVM_PARALLEL_LINK_JOBS=2 > "$A/llvm.log" 2>&1
    ninja -C "$A/llvm" -j "$JOBS" clang lld >> "$A/llvm.log" 2>&1
fi

# The package: stripped binaries, clang's own headers, and in the per-target
# runtime directory, named by the normalized triple clang looks under
# (lib/clang/19/lib/ARCH-unknown-linux-musl): the BusyBox port's builtins,
# compiler-rt's crtbegin and crtend objects (clang_rt.crtbegin.o, built here
# from the same source with compiler-rt's CRT flags) and libunwind.a,
# which a link with clang's defaults asks for.
P=$A/pkg
NT=$ARCH-unknown-linux-musl
RD=$P/lib/clang/19/lib/$NT
rm -rf "$P" && mkdir -p "$P/bin" "$RD"
llvm-objcopy --strip-all "$A/llvm/bin/clang-19" "$P/bin/clang"
llvm-objcopy --strip-all "$A/llvm/bin/lld" "$P/bin/lld"
ln -s lld "$P/bin/ld.lld"
# clang's default configuration (a file beside the binary, read on every
# run): CosmoOS's user window starts above lld's default 2 MiB image base,
# so a program is linked at 4 MiB, as tcc and the ports link theirs.
printf '%s\n' '-Wl,--image-base=0x400000' > "$P/bin/clang.cfg"
cp -R "$A/llvm/lib/clang/19/include" "$P/lib/clang/19/include"
cp "$BUILTINS" "$RD/libclang_rt.builtins.a"
# The BusyBox port's archive takes every file of lib/builtins, crtbegin.c
# and crtend.c with them: a link with clang_rt.crtbegin.o would pull crtend
# from the archive too (duplicate __CTOR_LIST_END__ and the rest).
llvm-ar d "$RD/libclang_rt.builtins.a" crtbegin.o crtend.o
for f in crtbegin crtend; do
    "$A/cc" -c -O2 -fPIC -std=c11 -DCRT_HAS_INITFINI_ARRAY -DEH_USE_FRAME_REGISTRY \
        -o "$RD/clang_rt.$f.o" "$SRC/compiler-rt/lib/builtins/$f.c"
done
cp "$RT/lib/libunwind.a" "$RD/libunwind.a"
(cd "$P" && find . -print | LC_ALL=C sort | tar -czf "$A/clang.tar.gz" --no-recursion -T -)
ls -l "$P/bin/clang" "$P/bin/lld" "$A/clang.tar.gz"
