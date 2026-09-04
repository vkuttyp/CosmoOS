#!/bin/sh
# setup-dev-macos.sh
#
# Secondary convenience only: the primary development environment is the
# ARM64 Linux VM (see docs/development.md). This installs enough on macOS
# to build and boot-test under QEMU/TCG. Requires Homebrew.
#
# Apple's clang can target x86_64-unknown-none-elf and
# x86_64-unknown-windows but ships no lld; Homebrew llvm provides both.
set -eu

if ! command -v brew >/dev/null 2>&1; then
    echo "setup-dev-macos: Homebrew is required (https://brew.sh)" >&2
    exit 1
fi

brew install llvm make mtools qemu

echo
echo "Build with Homebrew's LLVM and GNU make 4:"
echo "  export LLVM_PREFIX=\"$(brew --prefix llvm)/bin/\""
echo "  alias make=gmake"
echo "Verify with: gmake check-tools"
