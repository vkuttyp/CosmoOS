#!/bin/sh
# release-key.sh FILE -- write the release signing key from $KEY (the CI
# secret COSMOOS_RELEASE_KEY: an Ed25519 seed, 64 hex characters) to FILE,
# mode 0600, and check that its public half is the tools/keys/*.pub the
# release trusts (roadmap M6; docs/development.md, "Releases"). Prints the
# key id, never the key.
set -eu
out=$1
root=$(cd "$(dirname "$0")/.." && pwd)
key=$(printf '%s' "${KEY:-}" | tr -d ' \n\r')
case "$key" in
    *[!0-9a-fA-F]*|"") echo "release-key: KEY is empty or not hex" >&2; exit 1 ;;
esac
if [ ${#key} -ne 64 ]; then
    echo "release-key: KEY is ${#key} hex characters, expected 64" >&2
    exit 1
fi
set -- "$root"/tools/keys/*.pub
if [ $# -ne 1 ] || [ ! -f "$1" ]; then
    echo "release-key: expected exactly one tools/keys/*.pub, found $#" >&2
    exit 1
fi
mkdir -p "$(dirname "$out")"
umask 077
printf '%s\n' "$key" > "$out"
python3 "$root/scripts/release-key-check.py" "$out" "$1"
