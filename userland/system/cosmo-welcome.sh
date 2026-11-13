#!/bin/sh
# cosmo-welcome - the console's welcome screen (docs/userland/design.md, "The console's session"): the screen
# cleared, the CosmoOS logo and the machine's facts beside it, and where to start. /sbin/cosmo-console runs it on
# the live system, and /etc/profile after a login on the console. Every fact is read from what any user may read
# (uname, /proc, stty, ip, pkg); one that cannot be read is left out, never guessed.

esc=$(printf '\033')
logo="$esc[1;36m"   # bright cyan
label="$esc[1;34m"  # bright blue
off="$esc[0m"

facts() {
    printf '%s%s%s@%s%s\n' "$esc[1;32m" "$(id -un 2>/dev/null || echo root)" "$off" "$esc[1;32m" "$(hostname)$off"
    echo "------------"
    v=$(uname -v)   # CosmoOS 0.0.1 vX.Y.Z
    printf '%sOS:%s %s %s\n' "$label" "$off" "CosmoOS ${v##* }" "$(uname -m)"
    printf '%sKernel:%s build %s\n' "$label" "$off" "${v##* }"
    if read up _ < /proc/uptime 2>/dev/null; then
        s=${up%%.*}
        if [ "$s" -ge 3600 ]; then u="$((s / 3600)) h $((s % 3600 / 60)) min"; else u="$((s / 60)) min"; fi
        printf '%sUptime:%s %s\n' "$label" "$off" "$u"
    fi
    n=$(pkg list 2>/dev/null | wc -l)
    printf '%sPackages:%s %s installed\n' "$label" "$off" "$n"
    sh=$(cat /etc/console-shell 2>/dev/null)
    printf '%sShell:%s %s\n' "$label" "$off" "${sh:-/bin/cosmo-sh}"
    size=$(stty size 2>/dev/null)
    [ -n "$size" ] && printf '%sConsole:%s %s columns, %s rows\n' "$label" "$off" "${size#* }" "${size% *}"
    cpus=$(grep -c '^processor' /proc/cpuinfo 2>/dev/null)
    printf '%sCPU:%s %s x %s\n' "$label" "$off" "$(uname -m)" "${cpus:-1}"
    total=$(awk '/^MemTotal:/ { print int($2 / 1024) }' /proc/meminfo 2>/dev/null)
    avail=$(awk '/^MemAvailable:/ { print int($2 / 1024) }' /proc/meminfo 2>/dev/null)
    [ -n "$total" ] && printf '%sMemory:%s %s MiB / %s MiB\n' "$label" "$off" "$((total - ${avail:-0}))" "$total"
    addr=$(ip addr 2>/dev/null | awk '$1 == "inet" && $2 !~ /^127\./ { sub("/.*", "", $2); print $2; exit }')
    printf '%sIP:%s %s\n' "$label" "$off" "${addr:-none}"
}

# The logo, five rows, each padded to the same width so the facts line up beside it.
logo_rows='   ____
  / ___|___  ___ _ __ ___   ___
 | |   / _ \/ __| '"'"'_ ` _ \ / _ \
 | |__| (_) \__ \ | | | | | (_) |
  \____\___/|___/_| |_| |_|\___/'

printf '%s[2J%s[H\n' "$esc" "$esc"
facts > /tmp/.welcome.$$
i=0
while :; do
    i=$((i + 1))
    row=$(printf '%s\n' "$logo_rows" | sed -n "${i}p")
    fact=$(sed -n "${i}p" /tmp/.welcome.$$)
    [ -z "$row" ] && [ -z "$fact" ] && break
    printf '%s%-34s%s %s\n' "$logo" "$row" "$off" "$fact"
done
rm -f /tmp/.welcome.$$
printf '\n %sdmesg%s the kernel'"'"'s log   %spkg%s packages   %scosmo-upgrade --check%s a newer release   %spoweroff%s\n\n' \
    "$esc[1m" "$off" "$esc[1m" "$off" "$esc[1m" "$off" "$esc[1m" "$off"
