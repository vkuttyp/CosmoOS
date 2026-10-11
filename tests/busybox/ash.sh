#!/bin/sh
# Roadmap M3's scripted test (docs/userland/testing.md, "BusyBox"): BusyBox
# ash runs this, and every applet of ports/busybox/applets and the shell's
# own features are each checked once (an applet ash also has as a builtin
# -- echo, printf, test, [, true, false, pwd -- through /bin). Each check prints "BBTEST: ok NAME"
# or "BBTEST: FAIL NAME: what"; the last line is the verdict, which
# tests/boot/busybox_test.py requires.
T=/tmp/bbtest
rm -rf "$T"
mkdir -p "$T" && cd "$T" || { echo "BBTEST: FAIL setup"; exit 1; }
n=0
fails=0
ok() { n=$((n + 1)); echo "BBTEST: ok $1"; }
bad() { n=$((n + 1)); fails=$((fails + 1)); echo "BBTEST: FAIL $1: $2"; }
# expect NAME WANT GOT
expect() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "want [$2] got [$3]"; fi; }

# --- the shell -----------------------------------------------------------------
expect pipeline 3 "$(printf 'a\nb\nc\n' | cat | wc -l | tr -d ' ')"
echo one > f1
echo two >> f1
expect redirect-out-append "one two" "$(cat f1 | tr '\n' ' ' | sed 's/ $//')"
expect redirect-in one "$(head -n 1 < f1)"
ls /nonexistent 2> err.txt
expect redirect-stderr 1 "$(grep -c nonexistent err.txt)"
expect stderr-to-stdout 1 "$( (ls /nonexistent 2>&1) | grep -c nonexistent)"
expect command-substitution x-y "$(echo x-$(echo y))"
expect backquotes z "`echo z`"
x=outer
(x=inner; echo "$x" > sub.txt)
expect subshell "outer inner" "$x $(cat sub.txt)"
false
expect status-false 1 "$?"
true
expect status-true 0 "$?"
sh -c 'exit 7'
expect status-exit 7 "$?"
sleep 1 &
bgpid=$!
wait "$bgpid"
expect background-wait-status 0 "$?"
(sleep 1; echo bg > bg.txt) &
wait
expect background-job bg "$(cat bg.txt)"
printf '#!/bin/sh\necho "script $1"\n' > hb.sh
chmod 755 hb.sh
expect hashbang "script arg" "$(./hb.sh arg)"
nosuchcommand 2> /dev/null
expect exec-not-found 127 "$?"
printf 'x\n' > noexec
chmod 644 noexec
./noexec 2> /dev/null
expect exec-not-executable 126 "$?"
expect arithmetic 42 "$((6 * 7))"
v=
for i in 1 2 3; do v="$v$i"; done
expect for-loop 123 "$v"
case abc in a*) c=matched ;; *) c=no ;; esac
expect case matched "$c"
f() { echo "fn $1"; }
expect function "fn 1" "$(f 1)"

# --- the applets ---------------------------------------------------------------
mkdir -p d/e
if [ -d d/e ]; then ok mkdir; else bad mkdir "d/e missing"; fi
rmdir d/e
if [ ! -d d/e ]; then ok rmdir; else bad rmdir "d/e still there"; fi
echo hi > a
cp a b
expect cp hi "$(cat b)"
mv b c
if [ ! -e b ] && [ "$(cat c)" = hi ]; then ok mv; else bad mv "b or c wrong"; fi
rm c
if [ ! -e c ]; then ok rm; else bad rm "c still there"; fi
expect cat "hi hi" "$(cat a a | tr '\n' ' ' | sed 's/ $//')"
ln -s a lnk
expect ln-s a "$(readlink lnk)"
expect readlink-f "$T/a" "$(readlink -f lnk)"
chmod 600 a
expect chmod 600 "$(stat -c %a a)"
touch t1
if [ -f t1 ]; then ok touch-create; else bad touch-create "t1 missing"; fi
touch -t 202001020304 t1
expect touch-time 2020-01-02 "$(date -r t1 +%Y-%m-%d)"
expect ls "a lnk" "$(ls | grep -E '^(a|lnk)$' | xargs)"
expect echo "a b" "$(/bin/echo a b)"   # the applet, not the builtin
expect printf "07-x" "$(/bin/printf '%02d-%s' 7 x)"
if /bin/test 3 -gt 2; then ok test; else bad test "3 -gt 2 false"; fi
if /bin/[ abc = abc ] && /bin/[ ! -e /nonexistent ]; then ok '['; else bad '[' "string or file test"; fi
if /bin/true; then ok true; else bad true "status"; fi
if /bin/false; then bad false "status"; else ok false; fi
expect pwd "$T" "$(/bin/pwd)"
expect env V=1 "$(V=1 env | grep '^V=')"
s0=$(date +%s)
sleep 1
s1=$(date +%s)
if [ $((s1 - s0)) -ge 1 ]; then ok sleep; else bad sleep "slept $((s1 - s0)) s"; fi
expect date 1970-01-01 "$(date -u -d @0 +%Y-%m-%d)"
seq 1 10 > n.txt
expect seq "1 2 3" "$(seq 3 | xargs)"
expect head "1 2 3" "$(head -n 3 n.txt | xargs)"
expect tail "9 10" "$(tail -n 2 n.txt | xargs)"
expect wc 10 "$(wc -l < n.txt | tr -d ' ')"
expect sort "a a b c" "$(printf 'b\na\nc\na\n' | sort | xargs)"
expect sort-n "2 10" "$(printf '10\n2\n' | sort -n | xargs)"
expect uniq "a b c" "$(printf 'a\na\nb\nc\nc\n' | uniq | xargs)"
expect cut b "$(echo a:b:c | cut -d: -f2)"
expect tr ABC "$(echo abc | tr a-z A-Z)"
expect grep 2 "$(printf 'x\ny\nx\n' | grep -c x)"
expect sed hallo "$(echo hello | sed 's/e/a/')"
expect awk 6 "$(echo 1 2 3 | awk '{ print $1 + $2 + $3 }')"
expect awk-float 0.33 "$(awk 'BEGIN { printf "%.2f", 1 / 3 }')"
mkdir -p ft/x
touch ft/x/f1 ft/f2
expect find "ft/f2 ft/x/f1" "$(find ft -type f | sort | xargs)"
expect xargs 3 "$(echo a b c | xargs -n 1 echo | wc -l | tr -d ' ')"
mkdir tsrc
echo data > tsrc/f
tar -cf t.tar tsrc && rm -rf tsrc && tar -xf t.tar
expect tar data "$(cat tsrc/f)"
tar -czf t.tgz tsrc && rm -rf tsrc && tar -xzf t.tgz
expect tar-z data "$(cat tsrc/f)"
echo zz > g
gzip g
if [ -f g.gz ] && [ ! -e g ]; then ok gzip; else bad gzip "no g.gz"; fi
gunzip g.gz
expect gunzip zz "$(cat g)"
printf 'a\n' > d1
printf 'b\n' > d2
diff d1 d2 > /dev/null
expect diff-differ 1 "$?"
diff d1 d1
expect diff-same 0 "$?"
expect basename c "$(basename /a/b/c)"
expect dirname /a/b "$(dirname /a/b/c)"
expect expr 7 "$(expr 3 + 4)"
echo teed | tee tee.txt > /dev/null
expect tee teed "$(cat tee.txt)"
printf 'abcd\n' > s5
expect stat 5 "$(stat -c %s s5)"
expect uname Linux "$(uname -s)"
expect id 0 "$(id -u)"
expect sha256sum ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad "$(printf abc | sha256sum | cut -d' ' -f1)"
expect which /bin/ls "$(which ls)"

cd /
rm -rf "$T"
echo "BBTEST: $n checks, $fails failed"
if [ "$fails" = 0 ]; then echo "BBTEST: PASS"; else echo "BBTEST: FAIL"; fi
