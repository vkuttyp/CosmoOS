#!/bin/sh
# cosmo-console - the console's session (docs/userland/design.md, "The console's session"). init runs it, once
# /etc/rc has run, in place of the console shell (/etc/console-shell, else /bin/cosmo-sh).
#
# The live system, the image the machine booted from, has no accounts to log in to: the welcome screen, then the
# console shell, as before. An installed system asks for root's password at its first console boot, once, and then
# shows the login screen; a login runs root's shell (/etc/passwd), whose /etc/profile shows the welcome. Logging out
# brings the login screen back, as on other systems: `poweroff` ends the boot.

shell=$(cat /etc/console-shell 2>/dev/null)
case "$shell" in
/*) ;;
*) shell=/bin/cosmo-sh ;;
esac

# The root is a disk's (cosmofs) only on an installed system; the live system's is in memory.
if ! awk '$2 == "/" && $3 == "cosmofs" { found = 1 } END { exit !found }' /proc/mounts 2>/dev/null; then
    /bin/cosmo-welcome
    exec "$shell"
fi

esc=$(printf '\033')

# Passwords are kept in /etc/shadow, which only root reads; /etc/passwd says so with its "x". A system installed
# before the login has neither: its root's field was "*", no password at all.
if [ ! -f /etc/shadow ]; then
    (umask 077 && echo 'root:*:0:0:99999:7:::' > /etc/shadow)
fi
chmod 600 /etc/shadow
if ! grep -q '^root:x:' /etc/passwd; then
    sed -i 's/^root:[^:]*:/root:x:/' /etc/passwd
fi

while :; do
    if grep -q '^root:[*!]*:' /etc/shadow; then
        printf '%s[2J%s[H' "$esc" "$esc"
        echo "Welcome to CosmoOS. root has no password yet: choose one now."
        echo "The console asks for it at each login. (ssh logs in with a key, not a password.)"
        echo
        passwd root
        chmod 600 /etc/shadow
        continue
    fi
    printf '%s[2J%s[H' "$esc" "$esc"
    v=$(uname -v)
    printf '%s[1;36mCosmoOS %s%s (%s) - %s\n\n' "$esc" "${v##* }" "$esc[0m" "$(uname -m)" "$(hostname)"
    addr=$(ip addr 2>/dev/null | awk '$1 == "inet" && $2 !~ /^127\./ { sub("/.*", "", $2); print $2; exit }')
    [ -n "$addr" ] && printf 'IP address: %s   (ssh root@%s with a key)\n\n' "$addr" "$addr"
    login
done
