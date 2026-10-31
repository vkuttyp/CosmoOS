#!/bin/sh
# check-reproducible.sh [ARCH] [BUILD]
#
# Build the tree twice into separate output directories and compare the
# resulting kernel ELF, loader EFI, boot archive, signed modules and init byte for byte. A difference means a
# build input leaked into the output (timestamp, absolute path, random
# seed) and must be fixed. The second build is of a `make dist` tree
# (scripts/dist.py: the generated C beside each .nova source), with a novac
# that fails if anything runs it: so the C the pinned novac made in the first
# build and the tarball's are the same, and a tarball builds with no novac
# (roadmap 2.0's Nova rule, point 7; Nova's ADR 0023). Since roadmap M6 the whole release set too: the
# signed live image, its command line and manifest, and the set
# scripts/mkrelease.py makes of them, whose SHA256SUMS must be equal.
set -eu

arch=${1:-x86_64}
build=${2:-debug}
root=$(cd "$(dirname "$0")/.." && pwd)

a="$root/out/repro-a"
b="$root/out/repro-b"
src="$root/out/repro-src"
rm -rf "$a" "$b"
python3 "$root/scripts/dist.py" --dir "$src" >/dev/null
nonovac="$root/out/repro-no-novac"
mkdir -p "$nonovac"
printf '#!/bin/sh\necho "check-reproducible: novac was run in the dist tree" >&2\nexit 1\n' > "$nonovac/novac"
chmod +x "$nonovac/novac"

# BusyBox, dropbear and OpenSSH's sftp-server too, in each tree (BB_DIR,
# DB_DIR, OSSH_DIR, MBEDTLS_DIR: by default one per architecture, shared by both trees,
# which would hide a build-path leak).
for t in "$a" "$b"; do
    if [ "$t" = "$a" ]; then tree=$root; novac=${NOVAC:-}; path=$PATH; else tree=$src; novac=$nonovac/novac; path=$nonovac:$PATH; fi
    PATH=$path NOVAC=$novac ${MAKE:-make} -C "$tree" ARCH="$arch" BUILD="$build" OUT="$t" BB_DIR="$t/busybox" DB_DIR="$t/dropbear" OSSH_DIR="$t/openssh" MBEDTLS_DIR="$t/mbedtls" TCC_DIR="$t/tcc" \
        PORTS_CACHE="$root/.cache/ports" RELEASE_DIR="$t/release" all image release-set >/dev/null
done

status=0
case "$arch" in aarch64) loader=boot/BOOTAA64.EFI ;; *) loader=boot/BOOTX64.EFI ;; esac
for f in kernel/kernel.elf $loader boot.tar modules/hello.ko modules/cosmotest.ko modules/cosmotest_dep.ko modules/cosmotest_fail.ko userland/init.elf userland/sh.elf userland/ls.elf userland/pkg.elf busybox/busybox busybox/testsuite.tgz dropbear/scp openssh/sftp-server mbedtls/ssl_client mbedtls/cosmo-fetch tcc/tcc tcc/libtcc1.a tcc/runmain.o libc/libc.a pkg/repo/INDEX pkg/repo/hello-1.1.cpk pkg/repo/fortune-1.0.cpk cmdline manifest cosmoos.img release/SHA256SUMS; do
    if cmp -s "$a/$f" "$b/$f"; then
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
