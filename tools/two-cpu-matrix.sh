#!/usr/bin/env bash
# Boot an already-built image repeatedly across CPU counts and keep every
# boot's serial log and verdict, for docs/audit/2026-10-05-two-cpu-validation-report.md.
#
#   tools/two-cpu-matrix.sh <arch> <tag> <reps> <smp>...
#
# Each boot runs `gmake test` with the image built beforehand (nothing is
# rebuilt between boots: build first, then run this, then leave the tree
# alone). Per boot it keeps out/matrix/<tag>-<arch>-smp<N>-<i>.{serial,result}
# and writes one row to out/matrix/<tag>-<arch>.tsv:
#   smp  rep  verdict  seconds  failed-tests
# A rerun of the same tag replaces that boot's row along with its logs, so
# the table always describes the logs on disk; one tag may still be split
# over several invocations with different CPU counts.
# The verdict (PASS, FAIL or NO-VERDICT) is the harness's own `boot-test:`
# line, never inferred. Exits 1 if any boot did not PASS.
set -u
arch=$1 tag=$2 reps=$3
shift 3
dir=out/matrix
mkdir -p "$dir"
tsv="$dir/$tag-$arch.tsv"
touch "$tsv"
status=0
for smp in "$@"; do
    for i in $(seq 1 "$reps"); do
        base="$dir/$tag-$arch-smp$smp-$i"
        log="$base.serial"
        gmake --no-print-directory ARCH="$arch" QEMU_SMP="$smp" BOOT_LOG="$log" test > "$base.result" 2>&1
        rc=$?
        verdict=$(grep -E '^boot-test: (PASS|FAIL)' "$base.result" | tail -1)
        [ -n "$verdict" ] || verdict="NO-VERDICT rc=$rc"
        word=$(printf '%s\n' "$verdict" | grep -oE 'PASS|FAIL|NO-VERDICT' | head -1)
        [ "$word" = PASS ] || status=1
        secs=$(printf '%s\n' "$verdict" | grep -oE '[0-9]+\.[0-9]+s' | head -1)
        failed=$(grep -ohE '^SELFTEST: [a-z0-9_-]+ +\.\.\. FAIL' "$log" 2>/dev/null |
                 awk '{print $2}' | sort -u | tr '\n' ' ')
        # Drop a previous row for this boot, then add this one.
        awk -F'\t' -v s="$smp" -v r="$i" '!($1 == s && $2 == r)' "$tsv" > "$tsv.tmp" && mv "$tsv.tmp" "$tsv"
        printf '%s\t%s\t%s\t%s\t%s\n' "$smp" "$i" "$word" "${secs:-?}" "$failed" >> "$tsv"
        printf '%s smp=%s rep=%s: %s\n' "$arch" "$smp" "$i" "$verdict"
    done
done
exit "$status"
