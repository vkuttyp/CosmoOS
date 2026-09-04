#!/bin/sh
# check-tools.sh CC LD LDLINK OBJCOPY PYTHON
#
# Verify that the cross toolchain and test tools are present and that the
# compiler can target both the kernel and the UEFI loader triples.
set -u

cc=$1; ld=$2; ldlink=$3; objcopy=$4; python=$5
status=0

need() {
    if command -v "$1" >/dev/null 2>&1; then
        printf '  ok       %s (%s)\n' "$1" "$(command -v "$1")"
    else
        printf '  MISSING  %s%s\n' "$1" "${2:+ - $2}"
        status=1
    fi
}

echo "toolchain:"
need "$cc" "install clang (LLVM >= 15)"
need "$ld" "install lld"
need "$ldlink" "install lld"
need "$objcopy" "install llvm"
echo "image and test tools:"
need mformat "install mtools"
need mcopy "install mtools"
need qemu-system-x86_64 "install qemu"
need "$python" "install python3"

echo "firmware:"
here=$(cd "$(dirname "$0")" && pwd)
if fw=$("$here/find-firmware.sh" x86_64 2>/dev/null); then
    printf '  ok       %s\n' "$fw"
else
    printf '  MISSING  UEFI firmware - install ovmf (Debian/Ubuntu), edk2-ovmf (Fedora/Arch), or brew qemu; or set OVMF_CODE\n'
    status=1
fi

echo "compiler targets:"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo 'int f(void){return 0;}' > "$tmp/t.c"
for triple in x86_64-unknown-none-elf x86_64-unknown-windows; do
    if "$cc" --target=$triple -ffreestanding -c "$tmp/t.c" -o "$tmp/t.o" 2>"$tmp/err"; then
        printf '  ok       %s\n' "$triple"
    else
        printf '  FAIL     %s: %s\n' "$triple" "$(head -n 1 "$tmp/err")"
        status=1
    fi
done

if [ $status -ne 0 ]; then
    echo "check-tools: missing prerequisites; see scripts/setup-dev-*.sh" >&2
fi
exit $status
