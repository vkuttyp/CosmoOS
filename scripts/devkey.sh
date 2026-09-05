#!/bin/sh
# devkey.sh ensure DIR
#
# Make sure DIR holds a developer signing key pair (dev.key, dev.pub), creating
# one with scripts/modsign.py keygen if it does not. The build calls this for
# SIGNING=dev (build/config.mk); DIR is $COSMO_KEYDIR, outside the source tree
# by default ($HOME/.config/cosmoos/keys), so no private key can ever be
# committed. The pair is per developer machine: every module and package built
# here is signed with it and only kernels built here trust it. Release signing
# never goes through this script (docs/kernel/module/design.md, "Security").
#
# Concurrency: two builds may race here. The pair is generated into a
# private temporary directory and moved into place atomically, and the
# check afterwards accepts only a complete pair, so both builds end up
# signing with the same key that the ring embeds.
set -eu

cmd=${1:-}
dir=${2:-}
if [ "$cmd" != ensure ] || [ -z "$dir" ]; then
    echo "usage: devkey.sh ensure DIR" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd -P)
root=$(cd "$here/.." && pwd -P)

umask 077
mkdir -p "$dir"
dir=$(cd "$dir" && pwd -P)   # resolved: relative paths and symlinks cannot slip a key into the tree
case "$dir" in
    "$root"|"$root"/*)
        echo "devkey: refusing to put a private key inside the source tree ($dir)" >&2
        exit 1 ;;
esac

if [ -f "$dir/dev.key" ] && [ -f "$dir/dev.pub" ]; then
    exit 0
fi
tmp=$(mktemp -d "$dir/.devkey.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
"${PYTHON:-python3}" "$here/modsign.py" keygen --out-key "$tmp/dev.key" --out-pub "$tmp/dev.pub" >/dev/null
chmod 600 "$tmp/dev.key"
chmod 644 "$tmp/dev.pub"
# The key first, then the public half: a reader that sees dev.pub also
# sees the matching dev.key. A racing generator that got there first wins
# (mv onto an existing file is atomic; ours is discarded by the check).
if [ ! -f "$dir/dev.key" ]; then
    mv "$tmp/dev.key" "$dir/dev.key"
    mv "$tmp/dev.pub" "$dir/dev.pub"
    echo "devkey: new development signing key in $dir (private key never leaves this machine)"
fi
[ -f "$dir/dev.key" ] && [ -f "$dir/dev.pub" ]
