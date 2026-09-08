#!/bin/sh
# check-fpregs.sh - The kernel does not touch the floating-point or vector
# registers (docs/kernel/arch/design.md, "FPU and SIMD state").
#
# The build flag -mgeneral-regs-only says so to the compiler; this says it
# to the built image, which is where a hand-written assembly file, an
# inline asm block or a compiler-emitted copy would show up. Neither
# architecture can enforce the rule in hardware: ring 0 and EL1 are
# exactly where a thread's state is saved and restored.
#
# Usage: check-fpregs.sh <kernel.elf> [objdump]
set -eu
elf=${1:?usage: check-fpregs.sh <kernel.elf> [objdump]}
objdump=${2:-llvm-objdump}
command -v "$objdump" >/dev/null 2>&1 || objdump=objdump
command -v "$objdump" >/dev/null 2>&1 || { echo "check-fpregs: no objdump; skipped"; exit 0; }

# The functions that may touch them: the state save and restore, the
# guest swap in the hypervisor backend, and -- in a build with
# self-tests -- the hooks that put a known pattern in the registers.
# Adding a name here is a decision someone has to make on purpose.
# fpu_probe_thread is here because inlining moves instructions into the
# caller: on x86-64 the helpers that load and store the pattern end up
# inside the self-test's thread function. On AArch64 the same helpers are
# assembly and cannot move, which is the argument for keeping them there.
allow='aarch64_fpu_area_save|aarch64_fpu_area_restore|aarch64_fpu_probe_|x86_fpu_area_save|x86_fpu_area_restore|arch_test_fpu_set|arch_test_fpu_get|xmm_load|xmm_store|fpu_probe_thread'

"$objdump" -d "$elf" | awk -v allow="$allow" '
    # "ffffffff80001234 <name>:" starts a function.
    /^[0-9a-f]+ <.*>:$/ {
        fn = $0; sub(/^[0-9a-f]+ </, "", fn); sub(/>:$/, "", fn)
        ok = (fn ~ allow)
        next
    }
    ok { next }
    # Everything after the last tab is the mnemonic and its operands;
    # what follows a comment marker is the disassembler talking.
    /\t/ {
        n = split($0, parts, "\t")
        text = parts[n]
        sub(/[#;].*$/, "", text)
        # x86 names its vector registers with a sigil; AArch64 does not,
        # so there the whole operand list is fair game.
        if (text ~ /%(xmm|ymm|zmm)[0-9]+/ || text ~ /%st\(/ ||
            text ~ /(^|[ ,[{])(v|q|d|s)[0-9]+([.,)}\]]|$)/) {
            print "check-fpregs: " fn ": " text
            bad++
        }
    }
    END { if (bad) { print "check-fpregs: " bad " instruction(s) outside the allowed functions"; exit 1 } }
'
echo "check-fpregs: $(basename "$elf") touches no vector register outside the state save and restore"
