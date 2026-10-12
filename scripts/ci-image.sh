#!/bin/sh
# ci-image.sh REPO -- make sure the CI images for ci/Dockerfile exist in the
# registry, building and pushing them only when this Dockerfile's version is
# not there yet, and print each one's digest reference for the workflow
# (roadmap M6; docs/development.md, "Continuous integration").
#
#   REPO      e.g. ghcr.io/vkuttyp/cosmoos-ci (logged in already)
#   output    lines `toolchain=REPO@sha256:...` and `litmus=REPO@sha256:...`,
#             appended to $GITHUB_OUTPUT when it is set
#
# The tag is the target name and the first 16 hex digits of the
# Dockerfile's SHA-256, so an unchanged Dockerfile never rebuilds and a
# changed one never reuses an old image.
set -eu
repo=$1
root=$(cd "$(dirname "$0")/.." && pwd)
df="$root/ci/Dockerfile"
hash=$(sha256sum "$df" | cut -c1-16)
for target in toolchain litmus; do
    ref="$repo:$target-$hash"
    # Built only when the registry says the tag is not there: any other
    # failure (the network, authentication) stops here, rather than
    # rebuilding an unchanged Dockerfile with whatever Debian serves today.
    if err=$(docker buildx imagetools inspect "$ref" 2>&1 >/dev/null); then
        echo "ci-image: $ref exists" >&2
    elif ! printf '%s' "$err" | grep -qi 'not found\|manifest unknown'; then
        echo "ci-image: cannot tell whether $ref exists: $err" >&2
        exit 1
    else
        echo "ci-image: building $ref" >&2
        docker build --target "$target" -t "$ref" -f "$df" "$root/ci"
        docker push "$ref"
    fi
    digest=$(docker buildx imagetools inspect "$ref" --format '{{json .Manifest}}' | python3 -c 'import json,sys; print(json.load(sys.stdin)["digest"])')
    line="$target=$repo@$digest"
    echo "$line"
    if [ -n "${GITHUB_OUTPUT:-}" ]; then
        echo "$line" >> "$GITHUB_OUTPUT"
    fi
done
