#!/usr/bin/env bash
# Boot an already-built image repeatedly across CPU counts and keep every
# boot's serial log and verdict, for docs/audit/2026-10-05-two-cpu-validation-report.md.
#
#   tools/two-cpu-matrix.sh <arch> <tag> <reps> <smp>...
#
# Each boot runs `gmake test` with the image built beforehand (nothing is
# rebuilt between boots: build first, then run this, then leave the tree
# alone). Per boot it keeps out/matrix/<tag>-<arch>-smp<N>-<i>.{serial,result}
# and appends one row to out/matrix/<tag>-<arch>.tsv:
#   smp  rep  verdict  seconds  failed-tests
# The verdict is the harness's own `boot-test:` line, never inferred.
set -u
arch=$1 tag=$2 reps=$3
shift 3
dir=out/matrix
mkdir -p "$dir"
tsv="$dir/$tag-$arch.tsv"
for smp in "$@"; do
    for i in $(seq 1 "$reps"); do
        base="$dir/$tag-$arch-smp$smp-$i"
        log="$base.serial"
        gmake --no-print-directory ARCH="$arch" QEMU_SMP="$smp" BOOT_LOG="$log" test > "$base.result" 2>&1
        rc=$?
        verdict=$(grep -E '^boot-test: (PASS|FAIL)' "$base.result" | tail -1)
        [ -n "$verdict" ] || verdict="NO-VERDICT rc=$rc"
        secs=$(printf '%s\n' "$verdict" | grep -oE '[0-9]+\.[0-9]+s' | head -1)
        failed=$(grep -ohE '^SELFTEST: [a-z0-9_-]+ +\.\.\. FAIL' "$log" 2>/dev/null |
                 awk '{print $2}' | sort -u | tr '\n' ' ')
        printf '%s\t%s\t%s\t%s\t%s\n' "$smp" "$i" "${verdict%% *in*}" "${secs:-?}" "$failed" >> "$tsv"
        printf '%s smp=%s rep=%s: %s\n' "$arch" "$smp" "$i" "$verdict"
    done
done
