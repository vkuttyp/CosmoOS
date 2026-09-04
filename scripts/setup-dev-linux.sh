#!/bin/sh
# setup-dev-linux.sh
#
# Install the CosmoOS development prerequisites on a Debian/Ubuntu ARM64 or
# x86-64 Linux host (the primary environment is an ARM64 Ubuntu VM under
# Parallels). Idempotent. Requires sudo.
set -eu

if ! command -v apt-get >/dev/null 2>&1; then
    echo "setup-dev-linux: only Debian/Ubuntu (apt) is scripted; install the equivalents of:" >&2
    echo "  clang lld llvm make mtools qemu-system-x86 ovmf python3 git" >&2
    exit 1
fi

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    clang lld llvm make git \
    mtools qemu-system-x86 ovmf \
    python3

echo
echo "Installed. Verify with: make check-tools"
