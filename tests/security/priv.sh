# Roadmap M5's privilege checks (acceptance d; docs/kernel/security/testing.md,
# "test-security, stage priv"), run by BusyBox ash as root from init's shell:
# make test-security reads the KEY=VALUE lines. Every command is named by
# its path: /sbin/mount, /sbin/priv and the rest are native, `kill` inside
# `sh -c` is ash's builtin, i.e. the Linux door.
P=/sbin/priv
M=/tmp/m5-mnt
mkdir -p $M
echo "privs-root=$($P)"

# mount: root with every privilege; without PRIV_MOUNT; asking for it back
# from inside; a process given PRIV_MOUNT alone; an unprivileged user.
/sbin/mount none $M ramfs; echo "mount-root=$?"
/sbin/umount $M; echo "umount-root=$?"
$P -d mount -- /sbin/mount none $M ramfs; echo "mount-dropped=$?"
echo "privs-dropped=$($P -d mount -- $P)"
$P -d mount -- $P -k all -- /sbin/mount none $M ramfs; echo "mount-regain=$?"
echo "privs-regain=$($P -d mount -- $P -k all -- $P)"
$P -d mount -- /bin/sh -c "$P -k all -- /sbin/mount none $M ramfs"; echo "mount-regain-sh=$?"
$P -k mount -- /sbin/mount none $M ramfs; echo "mount-granted=$?"
echo "privs-granted=$($P -k mount -- $P)"
$P -k mount -- /sbin/umount $M; echo "umount-granted=$?"
$P -u 1000 -- /sbin/mount none $M ramfs; echo "mount-user=$?"
echo "privs-user=$($P -u 1000 -- $P)"

# sysctl write: kernel.hostname needs PRIV_SYS_ADMIN.
/sbin/sysctl kernel.hostname=m5-root; echo "sysctl-root=$?"
$P -d sys_admin -- /sbin/sysctl kernel.hostname=m5-dropped; echo "sysctl-dropped=$?"
$P -k sys_admin -- /sbin/sysctl kernel.hostname=m5-granted; echo "sysctl-granted=$?"
echo "hostname-now=$(/sbin/sysctl kernel.hostname)"

# kill another user's process: PRIV_PROC, through both doors.
rm -f /tmp/m5-victim
$P -u 1000 -- /bin/sh -c 'echo $$ > /tmp/m5-victim; exec /bin/sleep 60' &
holder=$!
n=0
while [ ! -s /tmp/m5-victim ] && [ $n -lt 100 ]; do /bin/sleep 0.1; n=$((n + 1)); done
v=$(cat /tmp/m5-victim)
echo "victim=$v"
$P -d proc -- /sbin/kill $v; echo "kill-dropped=$?"
$P -d proc -- /bin/sh -c "kill $v"; echo "kill-dropped-linux=$?"
$P -u 1001 -- /bin/sh -c "kill $v"; echo "kill-user=$?"
$P -k proc -- /sbin/kill $v; echo "kill-granted=$?"
wait $holder
echo "victim-gone=$?"

# What an unprivileged process can read of the kernel: no addresses in
# /proc or the sysctls, and no kernel log.
$P -u 1000 -- /sbin/dmesg > /dev/null; echo "dmesg-user=$?"
$P -u 1000 -- /bin/sh -c 'cat /proc/*/status /proc/*/limits; ls -l /proc/*/fd; /sbin/sysctl -a' > /tmp/m5-seen 2>&1
echo "seen-bytes=$(wc -c < /tmp/m5-seen)"
echo "seen-kernel-addresses=$(grep -c -E 'ffff[0-9a-f]{12}' /tmp/m5-seen)"
echo "priv-done"
