#!/bin/sh
# Roadmap 1.2 A1's acceptance, the guest's half (tests/boot/selfhost_test.py
# runs it on the installed system; docs/userland/testing.md, "test-selfhost").
#
#   selfhost.sh DIR programs   static, dynamic and dlopen programs
#   selfhost.sh DIR tests2     tcc's tests/tests2, as its Makefile runs them
#   selfhost.sh DIR rebuild    tcc rebuilt from its source by cc; the rebuilt
#                              compiler builds the same hello, and itself
#
# DIR holds what the harness served: tinycc.tar.gz (the source the
# installed tcc was built from: the pinned tarball with ports/tcc's patches), hello.c, dl-main.c, dl-greet.c, tests2-skip.txt (and
# tests2-skip-<arch>.txt) and this script. Each command prints `selfhost: <what>: PASS ...` lines, or
# `selfhost: <what>: FAIL ...` and exits 1. No make on the system yet
# (roadmap A2), so the Makefile's rules are spelled out here.

set -u
ARCH=$(uname -m)
fail() { echo "selfhost: $*"; exit 1; }

src() {
    # The source, unpacked once.
    if [ ! -d "$D/tinycc" ]; then
        mkdir -p "$D/tinycc" && (cd "$D/tinycc" && tar -xzf ../tinycc.tar.gz --strip-components=1) ||
            fail "source: FAIL: tar -xzf tinycc.tar.gz"
    fi
}

programs() {
    cd "$D" || exit 1
    cc -static -o hello-static hello.c || fail "static: FAIL: cc -static"
    ./hello-static static || fail "static: FAIL: exit $?"
    cc -o hello-dynamic hello.c || fail "dynamic: FAIL: cc"
    ./hello-dynamic dynamic || fail "dynamic: FAIL: exit $?"
    # A static program has no loader: dlopen needs a dynamic one.
    cc -shared -fPIC -o libgreet.so dl-greet.c || fail "dlopen: FAIL: cc -shared"
    cc -o dl-main dl-main.c -ldl || fail "dlopen: FAIL: cc dl-main.c"
    ./dl-main "$D/libgreet.so" || fail "dlopen: FAIL: exit $?"
    echo "selfhost: programs: PASS"
}

# The Makefile's per-test settings (tests/tests2/Makefile at the pinned
# commit), for this configuration: x86_64 or arm64, Linux, musl,
# --config-bcheck=no --config-backtrace=no, CC not gcc (so no GEN-ALWAYS).
flags() {
    case $1 in
    76_dollars_in_identifiers) echo -fdollars-in-identifiers ;;
    22_floating_point | 24_math_library) echo -lm ;;
    60_errors_and_warnings | 96_nodata_wanted | 139_arm64_errors | 125_atomic_misc | 128_run_atexit) echo -dt ;;
    104_inline) echo 104+_inline.c ;;
    120_alias) echo 120+_alias.c ;;
    106_versym | 124_atomic_counter | 144_tls) echo -pthread ;;
    esac
}
args() {
    case $1 in
    31_args) echo "arg1 arg2 arg3 arg4 arg5" ;;
    esac
}
norun() {
    case $1 in
    42_function_pointer | 120_alias | 106_versym | 108_constructor | 144_tls) return 0 ;;
    esac
    return 1
}

tests2() {
    src
    T="$D/tests2"
    rm -rf "$T" && cp -r "$D/tinycc/tests/tests2" "$T" && cd "$T" || fail "tests2: FAIL: copy"
    # The tests include config.h-free headers only; -I. as the Makefile's
    # TCCFLAGS for an installed tcc (-I$(TOP)) gives the test directory.
    TCC="tcc -I."
    pass=0 failed="" skipped=0
    for c in $(ls ??_*.c ???_*.c 2>/dev/null | sort); do
        t=${c%.c}
        case $t in *+*) continue ;; esac
        if cat "$D/tests2-skip.txt" "$D/tests2-skip-$ARCH.txt" 2>/dev/null | awk -v t="$t" '$1 == t { f = 1 } END { exit !f }'; then
            skipped=$((skipped + 1))
            continue
        fi
        f=$(flags "$t")
        case $t in
        46_grep)
            # Its pattern holds a `$`, so it is passed as one word here.
            $TCC $f -run "$c" '[^* ]*[:a:d: ]+\:\*-/: $' 46_grep.c > "$t.output" 2>&1 ;;
        146_tls_extern)
            ($TCC -c "$c" -o "$t-main.o" && $TCC -c "$c" -DDEFS -o "$t-defs.o" &&
                cc -no-pie -Wl,-z,noexecstack "$t-main.o" "$t-defs.o" -o "$t.exe" && "./$t.exe") > "$t.output" 2>&1 ;;
        148_linker_symbols)
            (for n in 1 2 3 4 5 6; do
                echo; echo "--- TEST $n ---"; F=$t-$n
                $TCC "$c" -DTEST=$n+100 -c -fcommon -o "$F.o" &&
                    $TCC "$c" -DTEST=$n+200 -shared -o "$F.so" -fPIC &&
                    $TCC "$c" -DTEST=$n -Wl,-rpath=. -o "$F.exe" "$F.o" "$F.so" -no-pie &&
                    "./$F.exe"
            done) > "$t.output" 2>&1 ;;
        *)
            if norun "$t"; then
                ($TCC $f "$c" -o "$t.exe" && "./$t.exe" $(args "$t")) > "$t.output" 2>&1
            else
                $TCC $f -run "$c" $(args "$t") > "$t.output" 2>&1
            fi ;;
        esac
        if diff -Nbu "$t.expect" "$t.output" > "$t.diff" 2>&1; then
            pass=$((pass + 1))
        else
            failed="$failed $t"
            echo "selfhost: tests2: $t differs:"
            head -40 "$t.diff"
        fi
    done
    [ -z "$failed" ] || fail "tests2: FAIL:$failed ($pass passed, $skipped excluded)"
    echo "selfhost: tests2: PASS: $pass passed, $skipped excluded (tests2-skip.txt)"
}

rebuild() {
    src
    cd "$D/tinycc" || exit 1
    # The port's configuration (ports/tcc/tcc.mk), configure run here.
    ./configure --cc=cc --prefix=/usr --config-musl --config-bcheck=no --config-backtrace=no \
        --sysincludepaths=/usr/lib/tcc/include:/usr/include --libpaths=/usr/lib/tcc:/usr/lib \
        --crtprefix=/usr/lib --elfinterp=$(ls /lib/ld-musl-*.so.1) > configure.log 2>&1 ||
        { tail -20 configure.log; fail "rebuild: FAIL: configure"; }
    cc -DC2STR conftest.c -o c2str && ./c2str include/tccdefs.h tccdefs_.h || fail "rebuild: FAIL: c2str"
    # The Makefile's native tcc: tcc.c (ONE_SOURCE, the whole compiler) and
    # its libraries, linked against libc.so and with -run's code mapped
    # read-only, as the installed one is (ports/tcc/tcc.mk).
    cc -O2 -DCONFIG_RUNMEM_RO=1 -o tcc1 tcc.c -I. -lm -ldl -lpthread || fail "rebuild: FAIL: cc tcc.c (stage 1)"
    ./tcc1 -O2 -DCONFIG_RUNMEM_RO=1 -o tcc2 tcc.c -I. -lm -ldl -lpthread || fail "rebuild: FAIL: tcc1 tcc.c (stage 2)"
    cmp tcc1 tcc2 || fail "rebuild: FAIL: tcc1 and tcc2 differ"
    cc -o "$D/hello-cc" "$D/hello.c" && ./tcc1 -o "$D/hello-tcc1" "$D/hello.c" || fail "rebuild: FAIL: hello"
    cmp "$D/hello-cc" "$D/hello-tcc1" || fail "rebuild: FAIL: hello from cc and from the rebuilt tcc differ"
    "$D/hello-tcc1" dynamic || fail "rebuild: FAIL: the rebuilt tcc's hello: exit $?"
    echo "selfhost: rebuild: PASS: tcc1 $(wc -c < tcc1) bytes, tcc2 the same; hello $(wc -c < "$D/hello-cc") bytes from both"
}

case ${2:-} in
programs | tests2 | rebuild) D=$1; "$2" ;;
*) echo "usage: selfhost.sh DIR programs|tests2|rebuild"; exit 2 ;;
esac
