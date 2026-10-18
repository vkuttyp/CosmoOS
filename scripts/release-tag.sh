#!/bin/sh
# release-tag.sh TAG -- in a release job's checkout, put back the tag as it
# was pushed and require `git describe` (BUILD_ID, build/config.mk) to name
# it exactly (docs/development.md, "Releases").
#
# actions/checkout fetches a pushed tag by its commit, which leaves an
# annotated tag lightweight in the clone; `git describe` skips lightweight
# tags, so v1.0.4 was built as v1.0.3-123-g1ec37307 and its cosmo-upgrade
# package said 1.0.3-123. Fetching the tag's own ref brings the tag object.
set -eu

tag=${1:?usage: release-tag.sh TAG}
if printf '%s' "$tag" | grep -Eqv '^v[0-9]+\.[0-9]+\.[0-9]+(-rc[0-9]+)?$'; then
    echo "release-tag: '$tag' is not vX.Y.Z[-rcN]" >&2
    exit 1
fi
git fetch -q --force origin "refs/tags/$tag:refs/tags/$tag"
kind=$(git cat-file -t "refs/tags/$tag")
if [ "$kind" != tag ]; then
    echo "release-tag: $tag is a lightweight tag ($kind); a release is tagged with git tag -a" >&2
    exit 1
fi
described=$(git describe --always --dirty)
if [ "$described" != "$tag" ]; then
    echo "release-tag: git describe says '$described', not '$tag'" >&2
    exit 1
fi
echo "release-tag: $tag (annotated); git describe names it"
