#!/bin/sh
# check-kernel-elf.sh OBJDUMP kernel.elf
#
# Post-link sanity checks that catch linker-script regressions before they
# turn into confusing boot failures:
#   - every PT_LOAD is either writable or executable, never both (W^X)
#   - a PT_NOTE segment exists (the loader reads the cosmoboot note there)
set -eu

objdump=$1
elf=$2

phdrs=$("$objdump" -p "$elf")

if ! printf '%s\n' "$phdrs" | grep -q '^ *NOTE '; then
    echo "check-kernel-elf: $elf has no PT_NOTE segment" >&2
    exit 1
fi

# objdump -p prints "LOAD off ... flags rwx" style lines followed by
# "         filesz ... memsz ... flags r-x". Extract the flags of each LOAD.
printf '%s\n' "$phdrs" | awk '
    /^ *LOAD / { inload = 1; next }
    inload && /flags/ {
        inload = 0
        for (i = 1; i <= NF; i++) if ($i == "flags") f = $(i + 1)
        if (f ~ /w/ && f ~ /x/) { print "check-kernel-elf: PT_LOAD with flags " f " violates W^X"; bad = 1 }
    }
    END { exit bad ? 1 : 0 }
'
