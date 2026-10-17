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

# --- v1.0.2's everyday commands, and the /proc files they read ----------------
expect md5sum 900150983cd24fb0d6963f7d28e17f72 "$(printf abc | md5sum | cut -d' ' -f1)"
dd if=/dev/zero of=z bs=512 count=3 2> /dev/null
expect dd 1536 "$(stat -c %s z)"
cmp d1 d1
expect cmp-same 0 "$?"
cmp d1 d2 > /dev/null
expect cmp-differ 1 "$?"
expect hexdump "00000000  41 42" "$(printf AB | hexdump -C | head -n 1 | cut -c1-15)"
expect du ft "$(du -s ft | cut -f2)"
expect whoami root "$(whoami)"
clear > cl
if [ -s cl ]; then ok clear; else bad clear "wrote nothing"; fi
expect less "x y" "$(printf 'x\ny\n' | less | xargs)"   # not a terminal: less copies
expect uptime 1 "$(uptime | grep -c 'load average: [0-9]*\.[0-9][0-9], ')"
expect free 1 "$(free | awk '/^Mem:/ { print ($2 > 0 && $4 > 0 && $4 <= $2) }')"
expect df / "$(df / | tail -n 1 | awk '{ print $NF }')"
expect proc-mounts / "$(awk '$2 == "/" { print $2 }' /proc/mounts)"
expect proc-cmdline 1 "$(tr '\0' ' ' < /proc/$$/cmdline | grep -c 'ash.sh')"
expect proc-stat "$$" "$(cut -d' ' -f1 /proc/$$/stat)"
# /proc/mounts names a mount by the directory the walk reached (a relative
# target is not recorded as typed) and escapes a space as Linux does.
mkdir "sp ace" rel
mount -t ramfs none "sp ace" && mount -t ramfs none rel
expect mounts-escaped 1 "$(grep -c " $T/sp\\\\040ace ramfs " /proc/mounts)"
expect mounts-relative 1 "$(grep -c " $T/rel ramfs " /proc/mounts)"
umount "sp ace"
umount rel
# A stopped process is T in its stat.
sleep 30 &
sp=$!
kill -STOP $sp
st=
for i in 1 2 3 4 5 6 7 8 9 10; do st=$(cut -d' ' -f3 /proc/$sp/stat); [ "$st" = T ] && break; sleep 0.1; done
expect proc-stat-stopped T "$st"
kill -CONT $sp
kill $sp
wait $sp 2>/dev/null
top -b -n 1 > top.txt
expect top-mem 1 "$(grep -c '^Mem: ' top.txt)"
expect top-load 1 "$(grep -c '^Load average: ' top.txt)"
expect top-self 1 "$(grep -c 'top -b -n 1' top.txt)"
# Keystrokes through a pipe, as a terminal would send them. A pipe has no
# window size: without $LINES and $COLUMNS vi asks the terminal for its
# cursor position (ESC [ 6 n) and reads the answer from stdin, eating the
# first keystroke. After an ESC, vi reads on until 50 ms pass with no
# input, to tell a lone ESC from an escape sequence; a pipe already at
# end of file answers that read with EOF, which vi takes as its input
# gone (as on Linux), so the writer stays open past the ESC.
{ printf 'ihello\033'; sleep 1; printf ':wq\r'; } | LINES=24 COLUMNS=80 vi v.txt > /dev/null 2>&1
expect vi hello "$(cat v.txt)"
# ping (the ping unit): an echo to the loopback address over a raw ICMP
# socket, and the socket refused without PRIV_NET (as Linux refuses one
# without CAP_NET_RAW).
# Three echoes: ping sends each after the first from SIGALRM (setitimer).
expect ping-lo 1 "$(ping -c 3 127.0.0.1 | grep -c '3 packets transmitted, 3 packets received')"
expect ping-noperm 1 "$(priv -d net ping -c 1 127.0.0.1 2>&1 | grep -c 'permission denied')"

# --- shell completeness S2: the everyday applets --------------------------------
printf 'one\ntwo\nthree\n' > l3
expect more "one two three" "$(more l3 | xargs)"   # not a terminal: more copies
expect nl "1	one" "$(nl l3 | head -n 1 | sed 's/^ *//')"
expect od "0000000 141 142 012" "$(printf 'ab\n' | od -b | head -n 1 | tr -s ' ')"
printf 'xx\001visible-text\002yy' > bin.dat
expect strings visible-text "$(strings bin.dat)"
expect rev cba "$(echo abc | rev)"
expect tac "three two one" "$(tac l3 | xargs)"
expect yes "y y y" "$(yes | head -n 3 | xargs)"
expect cal "February 2024" "$(cal 2 2024 | head -n 1 | sed 's/^ *//; s/ *$//')"
# /bin/time, the applet: ash parses a bare `time` as its own reserved word.
expect time 1 "$( { /bin/time -p sleep 0.2; } 2>&1 | grep -c '^real ')"
timeout 1 sleep 10
expect timeout 143 "$?"
# watch redraws every second until timeout ends it: the line at least once.
expect watch 1 "$(timeout 3 watch -n 1 echo watched-line 2>&1 | grep -m 1 -c watched-line)"
nohup sh -c 'echo nohup-ran' > nohup.out 2>&1
expect nohup nohup-ran "$(cat nohup.out)"
install -m 0640 l3 inst.txt
expect install "640 three" "$(stat -c %a inst.txt) $(tail -n 1 inst.txt)"
mkdir rp
expect realpath "$T/l3" "$(realpath ./rp/../l3)"
seq 1 10 > ten
split -l 4 ten part.
expect split "4 4 2" "$(wc -l < part.aa | tr -d ' ') $(wc -l < part.ab | tr -d ' ') $(wc -l < part.ac | tr -d ' ')"
expect fold "abc def gh" "$(echo abcdefgh | fold -w 3 | xargs)"
printf '1\n2\n' > c1; printf 'a\nb\n' > c2
expect paste "1+a 2+b" "$(paste c1 c2 | tr '\t' '+' | xargs)"
printf 'a\nb\nc\n' > s1; printf 'b\nc\nd\n' > s2
expect comm "b c" "$(comm -12 s1 s2 | xargs)"
expect cksum "4182447022 12" "$(printf 'hello cksum\n' | cksum)"
expect base64 aGVsbG8= "$(printf hello | base64)"
expect base64-d hello "$(echo aGVsbG8= | base64 -d)"
echo '/Td6WFoAAATm1rRGBMANCSEBHAAAAAAAAAAAAJbfEfMBAAhoZWxsbyB4egoAAAAAwUk6+mNSFFoAASkJZJIcHR+2830BAAAAAARZWg==' | base64 -d > h.xz
expect unxz "hello xz" "$(unxz -c h.xz)"
expect xz-d "hello xz" "$(xz -dc h.xz)"
echo 'UEsDBAoAAAAAACirT12sADrHBwAAAAcAAAAFAAAAei50eHR6aXBwZWQKUEsBAh4DCgAAAAAAKKtPXawAOscHAAAABwAAAAUAAAAAAAAAAQAAAKSBAAAAAHoudHh0UEsFBgAAAAABAAEAMwAAACoAAAAAAA==' | base64 -d > z.zip
mkdir uz && (cd uz && unzip -q ../z.zip)
expect unzip zipped "$(cat uz/z.txt)"
seq 1 1000 > big
bzip2 -k big
expect bzip2 "$(cat big | md5sum)" "$(bunzip2 -c big.bz2 | md5sum)"
gzip -c big > big.gz
expect zcat "$(cat big | md5sum)" "$(zcat big.gz | md5sum)"
mt=$(mktemp)
expect mktemp 1 "$( [ -f "$mt" ] && case "$mt" in /tmp/tmp.*) echo 1;; esac)"
rm -f "$mt"
expect setsid 1 "$(setsid sh -c 'ps_sid=$(cut -d" " -f6 /proc/$$/stat); [ "$ps_sid" = "$$" ] && echo 1')"
# pgrep, pkill, killall and pidof find a background sleep through /proc.
sleep 61 &
sp=$!
expect pgrep "$sp" "$(pgrep -f 'sleep 61')"
expect pidof 1 "$(pidof sleep | tr ' ' '\n' | grep -c "^$sp\$")"
pkill -f 'sleep 61'
wait $sp
expect pkill 143 "$?"
sleep 62 &
sp=$!
killall sleep
wait $sp
expect killall 143 "$?"
# logger: /dev/log, through syslogd, into the kernel's log.
tag=bbtest-$$
logger -t "$tag" "logged through syslogd"
got=0
for i in 1 2 3 4 5 6 7 8 9 10; do
    got=$(dmesg | grep -c "syslog: \[user.notice\] $tag: logged through syslogd")
    [ "$got" = 1 ] && break
    sleep 0.2
done
expect logger 1 "$got"
# A second syslogd leaves the running one's /dev/log alone (a unix socket
# is found by its node; unlinking it would cut the first off), and never
# deletes a file at its path.
expect syslogd-served 1 "$(/sbin/syslogd 2>&1 | grep -c 'already served (another syslogd holds it)')"
# The same endpoint by another spelling is the same instance.
expect syslogd-alias 1 "$(/sbin/syslogd /dev/../dev/log 2>&1 | grep -c 'already served (another syslogd holds it)')"
logger -t "$tag" "still served"
got=0
for i in 1 2 3 4 5 6 7 8 9 10; do
    got=$(dmesg | grep -c "syslog: \[user.notice\] $tag: still served")
    [ "$got" = 1 ] && break
    sleep 0.2
done
expect syslogd-still-served 1 "$got"
echo keep > notasock
/sbin/syslogd "$T/notasock" > sd.out 2>&1
expect syslogd-file "1 keep" "$(grep -c 'is not a socket' sd.out) $(cat notasock)"

# --- shell completeness S3: process tools on /proc ------------------------------
# ps and top name a running sleep with non-zero sizes (vsize and rss in its
# stat), and its status carries them (VmSize, VmRSS).
sleep 63 &
sp=$!
expect ps-sleep "$sp S 1" "$(ps -o pid,stat,vsz,rss | awk -v p=$sp '$1 == p { print $1, substr($2, 1, 1), ($3 > 0 && $4 > 0) }')"
expect ps-ruser root "$(ps -o pid,ruser | awk -v p=$sp '$1 == p { print $2 }')"   # status's Uid:
# init is the console shell's parent (this script's grandparent; init is
# not pid 1 in a self-test build, whose tests spawn processes first).
ip=$(ps -o pid,ppid | awk -v p=$PPID '$1 == p { print $2 }')
expect ps-init "${ip:-none} S init" "$(ps -o pid,stat,comm | awk '$3 == "init" { print $1, substr($2, 1, 1), $3 }')"
expect ps-shell "$$ S" "$(ps -o pid,stat | awk -v p=$$ '$1 == p { print $1, substr($2, 1, 1) }')"
top -bn1 > top3.txt
expect top-sleep 1 "$(awk -v p=$sp '$1 == p { print ($5 + 0 > 0) }' top3.txt)"
expect status-vm "1 1" "$(awk '/^VmSize:/ { s = ($2 > 0) } /^VmRSS:/ { r = ($2 > 0) } END { print s, r }' /proc/$sp/status)"
expect status-state "S (sleeping)" "$(sed -n 's/^State:\t//p' /proc/$sp/status)"
kill $sp
wait $sp 2>/dev/null
# free says what sysctl says: the same total, and a free figure within
# 2 MiB of one read just before it (the two are separate processes, and
# memory moves between them).
kb=$(( $(sysctl vm.page_size | awk '{ print $NF }') / 1024 ))
pt=$(sysctl vm.pages_total | awk '{ print $NF }')
pf=$(sysctl vm.pages_free | awk '{ print $NF }')
expect free-total $((pt * kb)) "$(free | awk '/^Mem:/ { print $2 }')"
ff=$(free | awk '/^Mem:/ { print $4 }')
d=$((ff - pf * kb))
echo "BBTEST: note free-free: free $ff kB, sysctl $((pf * kb)) kB just before"
if [ "${d#-}" -le 2048 ]; then ok free-free; else bad free-free "free ${ff} kB, sysctl $((pf * kb)) kB"; fi
expect uptime-up 1 "$(uptime | grep -c ' up ')"
expect kill-l KILL "$(/bin/kill -l 9)"
expect kill-l-list 1 "$(/bin/kill -l | grep -c 'HUP')"
# A process's own environment, cwd, root and executable.
expect environ 1 "$(env S3TAG=s3 cat /proc/self/environ | tr '\0' '\n' | grep -c '^S3TAG=s3$')"
expect exe /bin/busybox "$(readlink /proc/self/exe)"
expect cwd /tmp "$(cd /tmp && readlink /proc/self/cwd)"
expect root / "$(readlink /proc/self/root)"
# Another user's process does not exist for an unprivileged one (P1), so
# neither does its environment.
expect environ-cross 1 "$(priv -u 1000 -- cat /proc/1/environ 2>&1 | grep -c 'No such file')"
# PRIV_PROC is the rule (P5), not PRIV_SYS_ADMIN: root without sys_admin
# reads a uid-1000 process's environment, and without proc too it cannot.
priv -u 1000 -- sleep 64 &
pp=$!   # priv, which spawns the sleep as its child
up=
for i in 1 2 3 4 5 6 7 8 9 10; do up=$(pgrep -P $pp); [ -n "$up" ] && break; sleep 0.1; done
expect environ-privproc 1 "$(priv -d sys_admin -- cat /proc/$up/environ | tr '\0' '\n' | grep -c '^PATH=')"
expect environ-noprivproc 0 "$(priv -d sys_admin,proc -- cat /proc/$up/environ 2>/dev/null | wc -c)"
kill $up
wait
# version and cpuinfo: uname's release, a block per CPU.
expect version "$(uname -r)" "$(cut -d' ' -f3 /proc/version)"
expect cpuinfo "$(sysctl hw.ncpu | awk '{ print $NF }')" "$(grep -c '^processor' /proc/cpuinfo)"

# --- shell completeness S4: ownership and su ------------------------------------
echo own > own.txt
chown 1000:1000 own.txt
expect chown "1000 1000" "$(stat -c '%u %g' own.txt)"
chgrp 0 own.txt
expect chgrp "1000 0" "$(stat -c '%u %g' own.txt)"
# chown -h changes the link itself; its target keeps its owner.
ln -s own.txt own.lnk
chown -h 1001:1001 own.lnk
expect chown-h "1001 1001 1000 0" "$(stat -c '%u %g' own.lnk) $(stat -L -c '%u %g' own.lnk)"
# Through a link without -h, the target changes. (Both ids named: given an
# owner alone, BusyBox's chown fills in the group from its stat of the name,
# which for a link is the link's.)
chown 1002:0 own.lnk
expect chown-follow "1001 1002" "$(stat -c '%u' own.lnk) $(stat -L -c '%u' own.lnk)"
# A change of owner clears a program's set-user-id bit, and its
# set-group-id bit when it is group-executable, as Linux does.
chmod 6755 own.txt
chown 0 own.txt
expect chown-setid 755 "$(stat -c %a own.txt)"
# Refused: a process of another uid, and root without sys_admin.
expect chown-user 1 "$(priv -u 1000 -- chown 1000 own.txt 2>&1 | grep -c 'Operation not permitted')"
expect chown-noadmin 1 "$(priv -d sys_admin -- chown 5 own.txt 2>&1 | grep -c 'Operation not permitted')"
expect chown-unchanged "0 0" "$(stat -c '%u %g' own.txt)"
# su: to root, a no-op that works; to another uid with PRIV_CRED, and
# refused without it. The user exists for this test only.
cp /etc/passwd passwd.keep
cp /etc/group group.keep
echo 'tester:x:1000:1000:tester:/tmp:/bin/sh' >> /etc/passwd
echo 'tester:x:1000:' >> /etc/group
expect su-root 0 "$(su -c 'id -u')"
expect su-other "1000 1000" "$(su -c 'echo $(id -u) $(id -g)' tester)"
expect su-nocred 1 "$(priv -d cred -- su -c 'id -u' tester 2>&1 | grep -c 'not permitted')"
cp passwd.keep /etc/passwd
cp group.keep /etc/group

# --- shell completeness S5: power and clock -------------------------------------
# A signal to init needs sys_admin (security S20): root without it keeps
# PRIV_PROC and uid 0, so only that rule refuses it; the probe is allowed.
# ($ip is init's pid, from the S3 section.) A broken rule would restart
# this machine here.
expect kill-init-noadmin 1 "$(priv -d sys_admin -- kill -TERM $ip 2>&1 | grep -c 'Operation not permitted')"
expect kill-init-probe 0 "$(priv -d sys_admin -- kill -0 $ip 2>&1 | wc -l)"
expect reboot-f-noadmin 1 "$(priv -d sys_admin -- reboot -f 2>&1 | grep -c 'Operation not permitted')"
# reboot and halt without -f signal pid 1, which is init except in a
# self-test build (CI's test-busybox is a release build).
if [ "$ip" = 1 ]; then
    expect reboot-noadmin 1 "$(priv -d sys_admin -- reboot 2>&1 | grep -c 'Operation not permitted')"
    expect halt-noadmin 1 "$(priv -d sys_admin -- halt 2>&1 | grep -c 'Operation not permitted')"
else
    echo "BBTEST: note reboot-noadmin, halt-noadmin: init is pid $ip, not 1 (a self-test build); not run"
fi
# date -s: refused (BusyBox reports it and exits 0) without sys_admin and
# to uid 1000, the clock unmoved; as root a day ahead and back.
now=$(date +%s)
expect date-noadmin 1 "$(priv -d sys_admin -- date -s @$((now + 86400)) 2>&1 | grep -c "can't set date: Operation not permitted")"
expect date-user 1 "$(priv -u 1000 -- date -s @$((now + 86400)) 2>&1 | grep -c "can't set date: Operation not permitted")"
d=$(( $(date +%s) - now ))
if [ "$d" -ge 0 ] && [ "$d" -le 5 ]; then ok date-unmoved; else bad date-unmoved "moved by $d s"; fi
date -s @$((now + 86400)) > /dev/null
d=$(( $(date +%s) - now ))
date -s @$(( $(date +%s) - 86400 )) > /dev/null
if [ "$d" -ge 86400 ] && [ "$d" -le 86405 ]; then ok date-set; else bad date-set "moved by $d s, not a day"; fi
# hwclock reads /dev/rtc0 (QEMU has an RTC); writing it needs sys_admin
# (EACCES, as Linux's RTC_SET_TIME).
expect hwclock-read 1 "$(hwclock -u | grep -c '[0-9][0-9]:[0-9][0-9]:[0-9][0-9]')"
expect hwclock-noadmin 1 "$(priv -d sys_admin -- hwclock -w 2>&1 | grep -c 'Permission denied')"

# --- shell completeness S6: mounting ----------------------------------------------
# BusyBox mount and umount over the Linux mount(2) and umount2(2) doors: a
# ramfs and a procfs by hand, a read-only mount, the refusals; mkfs.cosmofs,
# blkid and lsblk on the NVMe scratch disk the harness gives each run.
mkdir m1 m2 m3 m4
mount -t ramfs none m1 && echo in > m1/f
expect mount-ramfs "in 1" "$(cat m1/f) $(grep -c " $T/m1 ramfs rw " /proc/mounts)"
expect mount-list 1 "$(mount | grep -c "^ramfs on $T/m1 type ramfs (rw)$")"
mount -t proc proc m2
expect mount-proc "$$" "$(cut -d' ' -f1 m2/$$/stat)"
mount -t ramfs -o ro none m3
expect mount-ro 1 "$( (touch m3/f) 2>&1 | grep -c 'Read-only file system')"
expect mount-noprv 1 "$(priv -d mount -- mount -t ramfs none m4 2>&1 | grep -c 'ermission denied')"
expect mount-noprv-none 0 "$(grep -c " $T/m4 " /proc/mounts)"
expect umount-noprv 1 "$(priv -d mount -- umount m1 2>&1 | grep -c 'Operation not permitted')"
expect umount-noprv-kept 1 "$(grep -c " $T/m1 " /proc/mounts)"
expect mount-remount 1 "$(mount -o remount,ro m1 2>&1 | grep -c 'Invalid argument')"
expect mount-nofs 1 "$(mount -t nosuchfs none m4 2>&1 | grep -c 'No such device')"
expect mount-nodev 1 "$(mount -t cosmofs /dev/nosuchdisk m4 2>&1 | grep -c 'No such file or directory')"
umount m1 && umount m2 && umount m3
expect umount-all 0 "$(grep -c " $T/m[123] " /proc/mounts)"
expect umount-gone "" "$(ls m1)"
if lsblk | grep -q '^nvme0n1 '; then
    mkfs.cosmofs -L s6scratch /dev/nvme0n1 > /dev/null
    expect blkid-label 1 "$(blkid /dev/nvme0n1 | grep -c '^/dev/nvme0n1: LABEL="s6scratch" UUID="[0-9a-f-]*" TYPE="cosmofs"$')"
    mount -t cosmofs /dev/nvme0n1 m4 && echo kept > m4/f && umount m4 && mount -t cosmofs -o ro /dev/nvme0n1 m4
    expect mount-cosmofs "kept 1" "$(cat m4/f) $(grep -c "^/dev/nvme0n1 $T/m4 cosmofs ro " /proc/mounts)"
    expect lsblk-mounted 1 "$(lsblk | grep -c "^nvme0n1 .* disk $T/m4$")"
    # A device is one mount's at a time (vfs V38): the second mount is
    # refused before cosmofs reads it, and the first is still there.
    mkdir m5
    expect mount-twice "1 0 1" "$(mount -t cosmofs /dev/nvme0n1 m5 2>&1 | grep -c 'busy') $(grep -c " $T/m5 " /proc/mounts) $(grep -c " $T/m4 cosmofs" /proc/mounts)"
    expect mkfs-busy 1 "$(mkfs.cosmofs /dev/nvme0n1 2>&1 | grep -c 'busy')"
    umount m4
    expect mkfs-badlabel 2 "$(mkfs.cosmofs -L 'a-label-of-17-chr' /dev/nvme0n1 > /dev/null 2>&1; echo $?)"
else
    echo "BBTEST: note blkid-label, mount-cosmofs, lsblk-mounted, mount-twice, mkfs-busy: no nvme0n1 here; not run"
fi
expect lsblk-root-disk 1 "$(lsblk | grep -c '^NAME  *SIZE RO TYPE MOUNTPOINTS$')"

cd /
rm -rf "$T"
echo "BBTEST: $n checks, $fails failed"
if [ "$fails" = 0 ]; then echo "BBTEST: PASS"; else echo "BBTEST: FAIL"; fi
