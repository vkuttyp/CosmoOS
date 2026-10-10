#!/bin/sh
# check-kernel-elf.sh OBJDUMP kernel.elf
#
# Post-link sanity checks that catch linker-script regressions before they
# turn into confusing boot failures:
#   - every PT_LOAD is either writable or executable, never both (W^X)
#   - a PT_NOTE segment exists (the loader reads the cosmoboot note there)
#   - schedule()'s restore cannot re-enter schedule() (below)
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

# schedule() ends by restoring its caller's interrupt state, and a restore
# that enables interrupts is a preemption point. Until the restore-loop unit
# that point entered schedule() again from schedule()'s own tail, one level
# per pending reschedule, and the stack stayed bounded only because every
# link was a tail call; this script checked each link, after lockdep's
# raw-pairing wrapper kept a frame and a one-CPU boot double faulted
# (docs/audit/2026-10-06-lockdep-irq-pairing-report.md). schedule_internal
# now takes that reschedule by looping and restores with
# arch_irq_restore_nopoint (docs/kernel/scheduler/invariants.md S31), so
# what is checked is that structure, not code generation: schedule_internal
# must exist and must reach none of the preempting restores or the
# preemption itself, by call or by jump; so must schedule_pass, the loop's
# body, when the compiler keeps it out of line (inlined, it is
# schedule_internal's own code); and the no-point restore must not reach
# preempt_point. The check reads these functions' own code: a preempting
# restore added further down, in a function they call, is the S31
# assertion's to catch at run time. The per-link tail-call checks are gone, with the
# reasons in docs/audit/2026-10-06-sched-restore-loop-report.md: a frame on
# any of those links is now one level per restore point, not per
# reschedule. A required symbol that is missing, or a disassembly that
# fails, is a failure, never a pass; schedule_pass and
# arch_irq_restore_nopoint are skipped only when the symbol table confirms
# them absent (inlined; the latter is inline without lockdep).
syms=$("$objdump" -t "$elf")
never_reaches() {
    required=$1 caller=$2
    shift 2
    if ! printf '%s\n' "$syms" | grep -E "[[:space:]]$caller\$" >/dev/null; then
        if [ "$required" = required ]; then
            echo "check-kernel-elf: $caller is not in the symbol table; schedule()'s restore is unchecked" >&2
            exit 1
        fi
        return 0
    fi
    if ! body=$("$objdump" -d --no-show-raw-insn --disassemble-symbols="$caller" "$elf"); then
        echo "check-kernel-elf: cannot disassemble $caller to check schedule()'s restore" >&2
        exit 1
    fi
    if ! printf '%s\n' "$body" | grep -E "<$caller>:" >/dev/null; then
        echo "check-kernel-elf: the disassembly of $caller is empty; schedule()'s restore is unchecked" >&2
        exit 1
    fi
    for callee in "$@"; do
        if printf '%s\n' "$body" | grep -E "<$callee>\$" >/dev/null; then
            echo "check-kernel-elf: $caller reaches $callee; a reschedule pending at schedule()'s" \
                 "restore would enter schedule() again from inside it (S31)" >&2
            exit 1
        fi
    done
}
never_reaches required schedule_internal arch_irq_restore arch_irq_restore_hw preempt_point sched_preempt
never_reaches optional schedule_pass arch_irq_restore arch_irq_restore_hw preempt_point sched_preempt
never_reaches required arch_irq_write_hw preempt_point
never_reaches optional arch_irq_restore_nopoint arch_irq_restore arch_irq_restore_hw preempt_point

# Hot loops (fbcon_draw_*, page_local_*; compiler.h __page_local) are functions of their own, aligned and small
# enough that none spans a 4 KiB page: under QEMU's TCG a hot loop across a
# page boundary runs an unchained translation block every iteration, and
# one such placement made a CI-built kernel scroll eleven times slower
# (kernel/core/fbcon.c, FBCON_DRAW). Check the outcome, not the attributes.
"$objdump" -t "$elf" | awk '
    # Hex by hand: neither BSD awk nor mawk has strtonum. Only the low 32
    # bits of the address: the page boundaries are all in it.
    function hex(s,   i, v) { v = 0; s = tolower(s); for (i = 1; i <= length(s); i++) v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1; return v }
    ($NF ~ /^fbcon_draw_/ || $NF ~ /^page_local_/) && $(NF - 1) ~ /^[0-9a-f]+$/ {
        n++
        addr = $1; size = $(NF - 1)
        lo = hex(substr(addr, length(addr) - 7))
        sz = hex(size)
        if (sz == 0 || int(lo / 4096) != int((lo + sz - 1) / 4096)) {
            print "check-kernel-elf: " $NF " (" sz " bytes at 0x" addr ") spans a page boundary"
            bad = 1
        }
    }
    END {
        if (n < 3) { print "check-kernel-elf: expected at least the three fbcon_draw_ functions, found " n; bad = 1 }
        exit bad ? 1 : 0
    }
'
