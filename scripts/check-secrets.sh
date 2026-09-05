#!/bin/sh
# check-secrets.sh
#
# Fail if the repository tracks signing material it must never hold:
#   - any private key file (*.key, or a hex seed under a keys directory),
#   - the public half of a key whose private half was once committed
#     (revoked: a kernel must never trust it again).
# Run by `make check-tools` (so CI runs it before building) and `make
# check-secrets`. See docs/kernel/module/design.md, "Security".
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
status=0

# Public keys revoked because their private half leaked: tools/keys/REVOKED
# (shared with scripts/gen-keyring.py, which refuses to compile them in).
REVOKED_PUBS=$(grep -Eo '^[0-9a-fA-F]{64}' tools/keys/REVOKED | tr 'A-F' 'a-f')
if [ -z "$REVOKED_PUBS" ]; then
    echo "check-secrets: tools/keys/REVOKED lists no key; refusing to run blind" >&2
    exit 1
fi

if ! command -v git >/dev/null 2>&1 || ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "check-secrets: not a git checkout; skipping" >&2
    exit 0
fi

tracked=$(git ls-files)

keys=$(printf '%s\n' "$tracked" | grep -E '\.key$')
if [ -n "$keys" ]; then
    printf 'check-secrets: private key file(s) tracked:\n%s\n' "$keys" >&2
    status=1
fi

# Anything under a keys directory that is not a README or a .pub is suspect.
odd=$(printf '%s\n' "$tracked" | grep -E '(^|/)keys/' | grep -vE '(\.pub|README\.md|/REVOKED)$')
if [ -n "$odd" ]; then
    printf 'check-secrets: unexpected file(s) under a keys directory:\n%s\n' "$odd" >&2
    status=1
fi

# A 64-hex-character line in any tracked .pub is a public key; compare it
# with the revoked list. Any tracked .pub is checked, wherever it lives.
for f in $(printf '%s\n' "$tracked" | grep -E '\.pub$'); do
    for hex in $(grep -Eo '^[0-9a-fA-F]{64}$' "$f" | tr 'A-F' 'a-f'); do
        for r in $REVOKED_PUBS; do
            if [ "$hex" = "$r" ]; then
                echo "check-secrets: $f holds a revoked public key ($hex)" >&2
                status=1
            fi
        done
    done
done

if [ $status -eq 0 ]; then
    echo "check-secrets: no signing secret or revoked key tracked"
fi
exit $status
