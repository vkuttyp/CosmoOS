#!/usr/bin/env python3
"""mkconfig.py BUSYBOX_SRC CONFIG

Regenerate ports/busybox/busybox.config (ports/busybox/README.md): run
on a BusyBox `make defconfig` output, then `make oldconfig` with every
answer the default. Every applet outside the M3 list is turned off -- an
applet is a symbol some source file names in an `//applet:IF_<SYMBOL>(`
line -- and a few options are forced, each for the reason beside it. The
features of the applets that stay keep defconfig's choice, which is what
BusyBox's own testsuite assumes.
"""
import os
import re
import sys

# The config symbols of the applets in ports/busybox/applets (ASH is sh,
# TEST1 is "[", BUSYBOX the multi-call binary's own applet).
KEEP = set("""ASH LS CAT CP MV RM MKDIR RMDIR LN CHMOD TOUCH ECHO PRINTF TEST TEST1 TRUE FALSE PWD ENV SLEEP DATE
HEAD TAIL WC SORT UNIQ CUT TR GREP SED AWK FIND XARGS TAR GZIP GUNZIP DIFF BASENAME DIRNAME EXPR SEQ TEE STAT
READLINK UNAME ID SHA256SUM WHICH BUSYBOX
WGET NSLOOKUP STTY TTY
VI LESS TOP FREE DF DU UPTIME WHOAMI CLEAR MD5SUM DD CMP HEXDUMP
PING POWEROFF
IFCONFIG ROUTE NETSTAT ARP HOSTNAME
MORE NL OD STRINGS REV TAC YES CAL TIME TIMEOUT WATCH NOHUP KILLALL PGREP PKILL PIDOF INSTALL REALPATH SPLIT
FOLD PASTE COMM CKSUM BASE64 UNZIP BZIP2 BUNZIP2 XZ UNXZ ZCAT MKTEMP LOGGER SETSID""".split())
# M4 (decision 5); v1.0.2's everyday commands; ping over raw ICMP sockets; poweroff; shell completeness S1's
# network inspection and S2's applets

FORCE = {
    "STATIC": "y",                 # decision 6: a static binary
    "ASH_JOB_CONTROL": "y",        # roadmap M4 (decision 4): jobs, fg, bg, ^Z on the console and on a pty
    "SH_IS_ASH": "y",
    "SH_IS_HUSH": "n",
    "SH_IS_NONE": "n",
    "BASH_IS_NONE": "y",
    "BASH_IS_ASH": "n",
    "BASH_IS_HUSH": "n",
    "FEATURE_SUID": "n",           # one user (M3 scope); no setuid applets
    "FEATURE_PREFER_APPLETS": "n",  # an applet is run through /bin like any program
    "SHA1_HWACCEL": "n",           # x86 SHA-NI only; one config serves both architectures
    "SHA256_HWACCEL": "n",
    "FEATURE_LS_COLOR_IS_DEFAULT": "n",  # plain names on a terminal too; --color asks for colour
    # wget speaks https through /bin/ssl_client (ports/mbedtls), which
    # checks the server's certificate: wget-ssl-client.patch makes wget
    # run it rather than BusyBox's own TLS, which checks none. TLS is
    # selected by FEATURE_WGET_HTTPS; nothing calls it after the patch.
    "FEATURE_WGET_HTTPS": "y",
    "FEATURE_WGET_OPENSSL": "n",
    "TLS": "y",
    # BusyBox's "const pointer to globals" trick (include/libbb.h) assumes
    # the compiler reloads a const pointer after an asm barrier; clang on
    # AArch64 does not, and every applet with a G struct read it as NULL
    # (faults at 0x4). libbb.h names this switch for such toolchains.
    "EXTRA_CFLAGS": '"-DBB_GLOBAL_CONST="',
    # ifconfig's SLIP options include <linux/if_slip.h>, a kernel header
    # the musl sysroot does not carry; there is no SLIP line discipline.
    "FEATURE_IFCONFIG_SLIP": "n",
}


def main(argv):
    src, cfg = argv[1], argv[2]
    applets = set()
    for root, _, files in os.walk(src):
        for f in files:
            if f.endswith(".c"):
                with open(os.path.join(root, f), errors="replace") as fh:
                    applets.update(re.findall(r"^//applet:\s*IF_(\w+)\(", fh.read(), re.M))
    missing = KEEP - applets
    if missing:
        raise SystemExit(f"mkconfig: not applets in this BusyBox: {sorted(missing)}")
    out = []
    with open(cfg) as fh:
        for line in fh:
            m = re.match(r"(?:# )?CONFIG_(\w+)(?:=| is not set)", line)
            if m:
                name = m.group(1)
                if name in FORCE:
                    continue
                if name in applets and name not in KEEP:
                    out.append(f"# CONFIG_{name} is not set\n")
                    continue
            out.append(line)
    for name, v in FORCE.items():
        if v == "y":
            out.append(f"CONFIG_{name}=y\n")
        elif v == "n":
            out.append(f"# CONFIG_{name} is not set\n")
        else:
            out.append(f"CONFIG_{name}={v}\n")
    with open(cfg, "w") as fh:
        fh.writelines(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
