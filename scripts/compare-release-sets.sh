#!/bin/sh
# compare-release-sets.sh A B -- the two builds of one architecture's
# release set must be identical file for file (roadmap M6). Prints one line
# for the release notes; exit 1 naming any difference.
set -eu
a=$1 b=$2
status=0
n=0
for f in "$a"/* "$b"/*; do
    name=$(basename "$f")
    if ! cmp -s "$a/$name" "$b/$name"; then
        echo "compare-release-sets: $name differs between $a and $b" >&2
        status=1
    fi
done
[ $status -eq 0 ] || exit 1
n=$(ls "$a" | wc -l | tr -d ' ')
img=$(ls "$a"/cosmoos-*.img)
arch=$(basename "$img" .img); arch=${arch#cosmoos-}
sum=$(grep " $(basename "$img")\$" "$a/SHA256SUMS" | cut -d' ' -f1)
echo "$arch: two builds, $n files each, identical; $(basename "$img") sha256 $sum"
