#!/bin/sh
# matrix.sh - the validation matrix (AGENTS.md, "Validation matrix"), in the order that fails soonest
# (docs/development.md, "Running the matrix"): the steps that boot nothing first, then, once the machine is quiet,
# the boots. One verdict line per step; the exit status is 0 only if every step passed.
#
#   MAKE              the GNU make to run (default: gmake if there is one, else make)
#   MATRIX_ARCHES     the architectures (default "x86_64 aarch64")
#   MATRIX_JOBS       -j for the builds (default 6)
#   MATRIX_LOAD       a boot waits until the one-minute load average is under this (default: the CPU count), and
#                     fails unrun if it is not within half an hour
#   MATRIX_LOAD_WAIT  that wait's limit in seconds (default 1800)
#   MATRIX_LOGS       where each step's output goes (default out/matrix)
#   MATRIX_BOOTS=0    stop after the steps that boot nothing
#   MATRIX_REMOTE     an ssh destination that runs x86_64 while this machine runs aarch64 (optional)
#   MATRIX_REMOTE_DIR its directory there (default /root/cosmoos-ci): run.sh runs one command in a container on tree/
#
# A boot test started while the builds' and the analyser's load is still falling fails its timing bounds
# (docs/testing/flakes.md, process-user), and a static failure found after the boots costs the boots again.
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
if [ -z "${MAKE:-}" ]; then
    if command -v gmake >/dev/null 2>&1; then MAKE=gmake; else MAKE=make; fi
fi
arches=${MATRIX_ARCHES:-x86_64 aarch64}
jobs=${MATRIX_JOBS:-6}
logs=${MATRIX_LOGS:-out/matrix}
mkdir -p "$logs" || exit 2
failed=0

ncpu() { getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4; }
load1() {
    if [ -r /proc/loadavg ]; then cut -d' ' -f1 /proc/loadavg; else sysctl -n vm.loadavg | awk '{print $2}'; fi
}
limit=${MATRIX_LOAD:-$(ncpu)}

# step LABEL command...: run it, keep its output, print its verdict.
step() {
    label=$1
    shift
    "$@" >"$logs/$label.log" 2>&1
    rc=$?
    what=$(grep -h -E 'boot-test: (PASS|FAIL)|static analysis: ' "$logs/$label.log" | tail -1 | cut -c1-100)
    if [ $rc -eq 0 ]; then
        echo "PASS  $label  $what"
    else
        echo "FAIL  $label  (exit $rc; $logs/$label.log)  $what"
        failed=1
    fi
    return $rc
}

# Wait, at most half an hour, for the load to fall under the limit; fail if it does not. (A function's variables are
# the script's: this one must not be the boots' loop variable.)
calm() {
    waited=0
    while [ "$(load1 | awk -v l="$limit" '{print ($1 >= l) ? 1 : 0}')" = 1 ]; do
        [ $waited -lt "${MATRIX_LOAD_WAIT:-1800}" ] || return 1
        sleep 30
        waited=$((waited + 30))
    done
}

# boot LABEL command...: a step that boots, run only on a quiet machine. A boot under load fails its timing bounds
# and proves nothing, so a load that never falls is a failure of the step, not a reason to boot anyway.
boot() {
    if calm; then
        step "$@"
    else
        echo "FAIL  $1  (not run: the load average stayed at or above $limit for ${MATRIX_LOAD_WAIT:-1800} s)"
        failed=1
    fi
}

# The x86_64 half on another machine, beside this one's aarch64 half. It is started detached there and its output
# fetched when this half is done, so a connection that drops in between (seen twice on 2026-11-12) loses nothing.
rssh() { ssh -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=30 "$MATRIX_REMOTE" "$@"; }
remote=0
if [ -n "${MATRIX_REMOTE:-}" ]; then
    rdir=${MATRIX_REMOTE_DIR:-/root/cosmoos-ci}
    arches=aarch64
    rcmd="MATRIX_ARCHES=x86_64 MATRIX_JOBS=4 MATRIX_BOOTS=${MATRIX_BOOTS:-1} sh scripts/matrix.sh"
    # The tree as it stands, without what is built from it.
    if COPYFILE_DISABLE=1 tar -c --no-xattrs --exclude ./out --exclude ./.git -f - . 2>"$logs/remote-copy.log" |
        rssh "mkdir -p $rdir/tree && cd $rdir/tree && \
            find . -mindepth 1 -maxdepth 1 ! -name out -exec rm -rf {} + && tar -x -f -" 2>>"$logs/remote-copy.log" &&
        rssh "cd $rdir && rm -f matrix.out && \
            (nohup sh -c \"./run.sh '$rcmd'; echo remote-exit \\\$?\" >matrix.out 2>&1 </dev/null &)" </dev/null; then
        remote=1
    else
        echo "FAIL  remote  (the tree could not be copied to $MATRIX_REMOTE or the run started)"
        failed=1
    fi
fi

for a in $arches; do
    step "build-$a-debug" $MAKE -j"$jobs" ARCH="$a" BUILD=debug
    step "build-$a-release" $MAKE -j"$jobs" ARCH="$a" BUILD=release
    # The images test-chaos and test-harness-retry boot, as those targets build them: built here, a build error is
    # found before any boot and their load is gone before the boots' load check.
    step "build-$a-chaos" $MAKE -j"$jobs" ARCH="$a" BUILD=debug SCHED_CHAOS=1 OUT="$root/out/$a-debug-chaos" image
    step "build-$a-hbreak" $MAKE -j"$jobs" ARCH="$a" BUILD=debug HARNESS_BREAK=1 OUT="$root/out/$a-debug-hbreak" image
done
step cee-check $MAKE cee-check
for a in $arches; do
    step "host-test-$a" $MAKE -j"$jobs" ARCH="$a" BUILD=debug host-test
    step "fuzz-$a" $MAKE -j"$jobs" ARCH="$a" BUILD=debug fuzz
    step "analyze-$a" $MAKE -j"$jobs" ARCH="$a" BUILD=debug analyze
done

if [ $failed -ne 0 ]; then
    echo "matrix: a step that boots nothing failed; the boots were not run"
elif [ "${MATRIX_BOOTS:-1}" != 0 ]; then
    for a in $arches; do
        for n in 1 2 4; do
            boot "test-$a-smp$n" $MAKE ARCH="$a" BUILD=debug QEMU_SMP=$n test
        done
        for t in test-smp2 test-chaos test-harness-retry; do
            boot "$t-$a" $MAKE ARCH="$a" BUILD=debug $t
        done
        boot "test-release-$a" $MAKE ARCH="$a" BUILD=release test
    done
fi

# The other machine's verdicts: ask every half minute, for at most MATRIX_REMOTE_WAIT seconds (default three hours),
# until its last line is there.
if [ $remote -eq 1 ]; then
    left=${MATRIX_REMOTE_WAIT:-10800}
    : >"$logs/remote.log"
    while [ "$left" -gt 0 ]; do
        if rssh "cat $rdir/matrix.out" >"$logs/remote.tmp" 2>/dev/null </dev/null; then
            mv "$logs/remote.tmp" "$logs/remote.log"
            grep -q '^remote-exit ' "$logs/remote.log" && break
        fi
        sleep 30
        left=$((left - 30))
    done
    grep -E '^(PASS|FAIL)  |^matrix: ' "$logs/remote.log" | sed 's/^/remote: /'
    if ! grep -q '^matrix: PASS' "$logs/remote.log"; then
        grep -q '^remote-exit ' "$logs/remote.log" || echo "FAIL  remote  (no result from $MATRIX_REMOTE; $logs/remote.log)"
        failed=1
    fi
fi

if [ $failed -eq 0 ]; then
    echo "matrix: PASS"
else
    echo "matrix: FAIL"
fi
exit $failed
