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
set -eu

cmd=${1:-}
dir=${2:-}
if [ "$cmd" != ensure ] || [ -z "$dir" ]; then
    echo "usage: devkey.sh ensure DIR" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)

case "$dir" in
    "$here"/..*|"$(cd "$here/.." && pwd)"*)
        echo "devkey: refusing to put a private key inside the source tree ($dir)" >&2
        exit 1 ;;
esac

if [ -f "$dir/dev.key" ] && [ -f "$dir/dev.pub" ]; then
    exit 0
fi
umask 077
mkdir -p "$dir"
"${PYTHON:-python3}" "$here/modsign.py" keygen --out-key "$dir/dev.key" --out-pub "$dir/dev.pub"
chmod 600 "$dir/dev.key"
chmod 644 "$dir/dev.pub"
echo "devkey: new development signing key in $dir (private key never leaves this machine)"
