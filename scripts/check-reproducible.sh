#!/bin/sh
# check-reproducible.sh [ARCH] [BUILD]
#
# Build the tree twice into separate output directories and compare the
# resulting kernel ELF, loader EFI, boot archive, signed modules and init byte for byte. A difference means a
# build input leaked into the output (timestamp, absolute path, random
# seed) and must be fixed. Since roadmap M6 the whole release set too: the
# signed live image, its command line and manifest, and the set
# scripts/mkrelease.py makes of them, whose SHA256SUMS must be equal.
set -eu

arch=${1:-x86_64}
build=${2:-debug}
root=$(cd "$(dirname "$0")/.." && pwd)

a="$root/out/repro-a"
b="$root/out/repro-b"
rm -rf "$a" "$b"

# BusyBox and dropbear too, in each tree (BB_DIR, DB_DIR: by default one
# per architecture, shared by both trees, which would hide a build-path leak).
for t in "$a" "$b"; do
    make -C "$root" ARCH="$arch" BUILD="$build" OUT="$t" BB_DIR="$t/busybox" DB_DIR="$t/dropbear" \
        RELEASE_DIR="$t/release" all image release-set >/dev/null
done

status=0
case "$arch" in aarch64) loader=boot/BOOTAA64.EFI ;; *) loader=boot/BOOTX64.EFI ;; esac
# The self-test fixtures exist only when SELFTEST=1 (debug builds): absent
# from both trees is fine for them and for nothing else.
optional=" modules/cosmotest.ko modules/cosmotest_dep.ko modules/cosmotest_fail.ko busybox/testsuite.tgz "
for f in kernel/kernel.elf $loader boot.tar modules/hello.ko modules/cosmotest.ko modules/cosmotest_dep.ko modules/cosmotest_fail.ko userland/init.elf userland/sh.elf userland/ls.elf userland/pkg.elf busybox/busybox busybox/testsuite.tgz libc/libc.a pkg/repo/INDEX pkg/repo/hello-1.1.cpk pkg/repo/fortune-1.0.cpk cmdline manifest cosmoos.img release/SHA256SUMS; do
    if [ ! -e "$a/$f" ] && [ ! -e "$b/$f" ] && [ "${optional#* $f }" != "$optional" ]; then
        printf '  absent   %s\n' "$f"
    elif cmp -s "$a/$f" "$b/$f"; then
        printf '  same     %s\n' "$f"
    else
        printf '  DIFFERS  %s\n' "$f"
        status=1
    fi
done

if [ $status -eq 0 ]; then
    echo "reproducible: yes"
else
    echo "reproducible: NO" >&2
fi
exit $status
