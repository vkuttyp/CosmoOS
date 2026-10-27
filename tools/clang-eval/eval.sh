#!/bin/sh
# Roadmap 1.2 A1's clang evaluation, the guest's half (clang_eval_test.py
# runs it on the installed system; tools/clang-eval/README.md).
#
#   eval.sh DIR STAGE
#
# DIR holds what the harness served: clang.tar.gz (tools/clang-eval/build.sh),
# tinycc.tar.gz (the source the installed tcc was built from), hello.c,
# dl-main.c, dl-greet.c and this script. Each stage prints `clang-eval: ...`
# lines: what ran, how long it took and the most memory it held.

set -u
D=$1
C=$D/clang/bin/clang
fail() { echo "clang-eval: $*"; exit 1; }

# measure WHAT CMD...: run CMD in the background, sample its VmRSS from
# /proc every 0.2 s (the kernel keeps no peak: no VmHWM, no ru_maxrss),
# and print the wall time and the largest sample of any compiler process
# (clang's driver, its cc1 and lld are processes of their own here).
measure() {
    what=$1; shift
    t0=$(date +%s)
    "$@" > "$D/$what.out" 2>&1 &
    pid=$!
    peak=0
    # Until it exits: a zombie still answers kill -0, so its State decides.
    while st=$(awk '/^State:/ { print $2 }' /proc/$pid/status 2>/dev/null) && [ -n "$st" ] && [ "$st" != Z ]; do
        # The largest compiler process now: clang's driver runs its compile
        # step (cc1) and lld as processes of their own.
        kb=$(cat /proc/[0-9]*/status 2>/dev/null | awk '/^Name:/ { n = $2 } /^VmRSS:/ && n ~ /^(clang|ld\.lld|lld|tcc)$/ && $2 > m { m = $2 } END { print m + 0 }')
        [ -n "$kb" ] && [ "$kb" -gt "$peak" ] && peak=$kb
        sleep 0.2
    done
    wait $pid
    rc=$?
    t1=$(date +%s)
    echo "clang-eval: $what: exit $rc, $((t1 - t0)) s, peak VmRSS $peak KiB"
    [ $rc = 0 ] || { tail -20 "$D/$what.out"; return 1; }
}

unpack() {
    cd "$D" || exit 1
    mkdir -p clang && (cd clang && tar -xzf ../clang.tar.gz) || fail "unpack: FAIL"
    "$C" --version > version.out 2>&1 || { cat version.out; fail "unpack: FAIL: clang --version"; }
    head -1 version.out
    "$D/clang/bin/ld.lld" --version || fail "unpack: FAIL: ld.lld --version"
    echo "clang-eval: resource dir $("$C" -print-resource-dir), builtins $("$C" -print-libgcc-file-name)"
    ls -l "$D/clang/bin/clang" "$D/clang/bin/lld"
    echo "clang-eval: unpack: PASS"
}

programs() {
    cd "$D" || exit 1
    measure hello-static "$C" -static -o hello-static hello.c || fail "programs: FAIL: clang -static"
    ./hello-static static || fail "programs: FAIL: the static hello"
    measure hello-dynamic "$C" -o hello-dynamic hello.c || fail "programs: FAIL: clang"
    ./hello-dynamic dynamic || fail "programs: FAIL: the dynamic hello"
    measure libgreet "$C" -shared -fPIC -o libgreet.so dl-greet.c || fail "programs: FAIL: clang -shared"
    measure dl-main "$C" -o dl-main dl-main.c || fail "programs: FAIL: clang dl-main.c"
    ./dl-main "$D/libgreet.so" || fail "programs: FAIL: dl-main"
    echo "clang-eval: programs: PASS"
}

# tcc.c (the whole compiler, one translation unit, about 30,000 lines once
# included): compiled by clang -O2 and by tcc, the same flags as
# test-selfhost's rebuild; the clang-built tcc then builds and runs hello.
bigger() {
    cd "$D" || exit 1
    mkdir -p tinycc && (cd tinycc && tar -xzf ../tinycc.tar.gz --strip-components=1) || fail "bigger: FAIL: tar"
    cd tinycc || exit 1
    ./configure --cc=cc --prefix=/usr --config-musl --config-bcheck=no --config-backtrace=no \
        --sysincludepaths=/usr/lib/tcc/include:/usr/include --libpaths=/usr/lib/tcc:/usr/lib \
        --crtprefix=/usr/lib --elfinterp=$(ls /lib/ld-musl-*.so.1) > configure.log 2>&1 || fail "bigger: FAIL: configure"
    cc -DC2STR conftest.c -o c2str && ./c2str include/tccdefs.h tccdefs_.h || fail "bigger: FAIL: c2str"
    measure tcc-by-tcc tcc -O2 -DCONFIG_RUNMEM_RO=1 -o tcc-by-tcc tcc.c -I. -lm -ldl -lpthread ||
        fail "bigger: FAIL: tcc tcc.c"
    measure tcc-by-clang-O0 "$C" -O0 -DCONFIG_RUNMEM_RO=1 -o tcc-by-clang-O0 tcc.c -I. -lm -ldl -lpthread ||
        fail "bigger: FAIL: clang -O0 tcc.c"
    measure tcc-by-clang-O2 "$C" -O2 -DCONFIG_RUNMEM_RO=1 -o tcc-by-clang tcc.c -I. -lm -ldl -lpthread ||
        fail "bigger: FAIL: clang -O2 tcc.c"
    ./tcc-by-clang -o "$D/hello-tcc-by-clang" "$D/hello.c" && "$D/hello-tcc-by-clang" dynamic ||
        fail "bigger: FAIL: the clang-built tcc's hello"
    echo "clang-eval: sizes: tcc-by-tcc $(wc -c < tcc-by-tcc), tcc-by-clang -O2 $(wc -c < tcc-by-clang) bytes"
    echo "clang-eval: bigger: PASS"
}

case ${2:-} in
unpack | programs | bigger) "$2" ;;
*) echo "usage: eval.sh DIR unpack|programs|bigger"; exit 2 ;;
esac
