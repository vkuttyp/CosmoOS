#!/bin/sh
# check-kernel-elf.sh OBJDUMP kernel.elf
#
# Post-link sanity checks that catch linker-script regressions before they
# turn into confusing boot failures:
#   - every PT_LOAD is either writable or executable, never both (W^X)
#   - a PT_NOTE segment exists (the loader reads the cosmoboot note there)
#   - the scheduler's restore chain is tail calls (below)
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

# schedule() ends by restoring interrupts, and that restore's preemption
# point may enter schedule() again ("one more trip"). The recursion is
# bounded only because every link is a tail call, so each frame is gone
# before the next is made. A link compiled as a call grows the stack by a
# frame per resumption of a busy thread: lockdep's raw-pairing wrapper did,
# and a one-CPU boot double faulted on the idle thread's stack
# (docs/audit/2026-10-06-lockdep-irq-pairing-report.md). Only a symbol the
# symbol table confirms absent is skipped (the wrapper is inline without
# lockdep); a disassembly that fails is a failure, never a pass.
syms=$("$objdump" -t "$elf")
tail_call() {
    caller=$1 callee=$2
    if ! printf '%s\n' "$syms" | grep -E "[[:space:]]$caller\$" >/dev/null; then
        return 0
    fi
    if ! body=$("$objdump" -d --no-show-raw-insn --disassemble-symbols="$caller" "$elf"); then
        echo "check-kernel-elf: cannot disassemble $caller to check the restore chain" >&2
        exit 1
    fi
    if ! printf '%s\n' "$body" | grep -E "<$caller>:" >/dev/null; then
        echo "check-kernel-elf: the disassembly of $caller is empty; the restore chain is unchecked" >&2
        exit 1
    fi
    if printf '%s\n' "$body" | grep -E "[[:space:]](call|callq|bl|blr)[[:space:]].*<$callee>" >/dev/null; then
        echo "check-kernel-elf: $caller calls $callee instead of tail-calling it;" \
             "the preempt-at-restore recursion through schedule() would grow the stack" >&2
        exit 1
    fi
}
tail_call schedule_internal arch_irq_restore
tail_call schedule_internal arch_irq_restore_hw
tail_call arch_irq_restore arch_irq_restore_hw
tail_call arch_irq_restore_hw preempt_point
tail_call preempt_point sched_preempt
tail_call sched_preempt schedule_internal
