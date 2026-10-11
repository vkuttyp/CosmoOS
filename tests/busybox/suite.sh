#!/bin/sh
# Roadmap M3: BusyBox's own testsuite, the files decision 8 names
# (docs/userland/testing.md, "BusyBox"). runtest wants a writable tree with
# the binary and its .config one directory above it, so both are copied to
# /tmp. Each file's run is bracketed by BBSUITE lines; runtest's own
# PASS/FAIL/SKIPPED/UNTESTED lines sit between them, and
# tests/boot/busybox_test.py reads them against its list of exclusions.
R=/tmp/bbsuite
rm -rf "$R"
mkdir -p "$R" && cd "$R" || { echo "BBSUITE: setup failed"; exit 1; }
tar -xzf /boot/tests/busybox/testsuite.tgz || { echo "BBSUITE: setup failed (tar)"; exit 1; }
cp /bin/busybox "$R/busybox" && cp /boot/tests/busybox/busybox.config "$R/.config" || { echo "BBSUITE: setup failed (cp)"; exit 1; }
# testing.sh compares results with cmp, which is not an M3 applet: a
# stand-in on runtest's PATH ($bindir) answers with cmp's status, through
# diff (0 the same, 1 different; testing.sh discards the output).
printf '#!/bin/sh\nexec diff "$@" > /dev/null 2>&1\n' > "$R/cmp" && chmod 755 "$R/cmp"
cd "$R/testsuite" || exit 1
for f in cut sed grep tr sort uniq head tail expr seq basename dirname wc xargs tar; do
    echo "BBSUITE: begin $f"
    if [ -n "$BBSUITE_VERBOSE" ]; then ./runtest -v "$f" 2>&1; else ./runtest "$f" 2>&1; fi
    echo "BBSUITE: end $f $?"
done
cd /
rm -rf "$R"
echo "BBSUITE: done"
