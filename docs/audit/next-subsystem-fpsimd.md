# NEXT SUBSYSTEM

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is the fifth such report
(`next-subsystem.md` named the Intel NIC, `-usb.md` the USB host stack,
`-ahci.md` AHCI, `-console.md` the machine's own console; all four
record their outcomes). Nothing in this one is implemented.

**Subsystem: floating point and SIMD — the registers every machine has,
that no program on this one may use.**

## Problem

`printf("%f", 3.14)` prints `?`.

That is not a placeholder in a young libc; it is the visible end of a
rule that runs through the whole tree. The kernel is built
`-mgeneral-regs-only`, which is correct and deliberate — kernel code has
no business in the vector registers. But so is **everything else**:
`libc/libc.mk` builds the C library that way, `build/arch/*.mk` builds
every user program that way, and `libc/src/printf.c` writes `?` for
`%f`, `%e` and `%g` because there is nothing else it could do.

On x86-64 that is a build choice over a kernel that is ready: 
`kernel/arch/x86_64/fpu.c` allocates a per-thread area, switches it
eagerly on every context switch between two owners, swaps it around a
guest entry, and the Linux personality's signal frame already carries
the 512-byte FXSAVE image (`compat/linux/signal.c:115`).

On AArch64 it is not a choice. `CPACR_EL1.FPEN` is left at its reset
value, so **every FP/SIMD instruction traps, at EL0 and EL1 alike**;
`kernel/arch/aarch64/fpu.c` is four no-ops that keep `thread->fpu` NULL,
and `compat/linux/signal.c:208` says in a comment that the arm64 signal
frame carries no `fpsimd_context` because there is no state to put in
one. The three rules `arch/fpu.h` states — the kernel never touches
these registers, a thread observes only its own, a guest starts from
reset — hold on that architecture the way a rule about swimming holds in
a desert.

What that costs, concretely: `hello_musl`, the tree's canary for "a real
statically linked Linux binary, not one we built to suit ourselves", is
compiled only on x86-64 (`tests/linux/linux.mk`). Any aarch64 libc
worth the name reaches for NEON in `memcpy` and `strlen` within a few
instructions of `_start`, and would take `SIGILL` for it. The Linux
personality on half the machines this project targets can run only
programs built to avoid the registers real programs are built to use.

## Current implementation

- **The seam** (`kernel/include/arch/fpu.h`): `arch_fpu_alloc(t)`,
  `arch_fpu_free(t)`, `arch_fpu_state_size()`, the switch hook the arch
  layer calls from its own `arch_context_switch`, and three test hooks
  (`arch_test_fpu_switch/set/get`). The header states the kernel,
  process and guest rules and says outright that architectures with no
  user floating point "implement these as no-ops that leave the pointer
  NULL".
- **x86-64** (`kernel/arch/x86_64/fpu.c`): CPU policy (`XSAVE` where
  available, `FXSAVE` otherwise), a per-thread 64-byte-aligned area,
  explicit ownership (`t->fpu != NULL`), eager switching with no lazy
  ownership — a decision from the critical-fix pass, made against
  cross-thread leakage — and `arch_user_fpu_image_save/restore` for the
  Linux frame. Tests: `fpu-switch` (two threads with different register
  contents), `hv-guest-fpu` (a guest's registers do not leak into the
  owner's).
- **AArch64** (`kernel/arch/aarch64/fpu.c`, 46 lines): no-ops.
  `arch_test_fpu_set` returns false, `arch_test_fpu_switch` returns true
  with the reason "no thread can own vector state: nothing to leak".
  `context.c` says the same in a comment where the switch hook would be.
- **Userland**: `-mgeneral-regs-only` in `build/arch/aarch64.mk` (twice:
  kernel and user), `build/arch/x86_64.mk` (twice) and `libc/libc.mk`.
  `userland/init/init.c` has one deliberate SSE fragment in inline
  assembly, which is what the x86 FPU test exercises through a real
  process. `libc/src/printf.c` prints `?` for the three float
  conversions and says why.
- **The Linux personality**: `linux_signal_frame` builds an x86 frame
  with `fpstate` when the arch offers a 512-byte image, and an arm64
  frame with an `esr_context` carrying syndrome 0 and no
  `fpsimd_context`.
- **The trap path**: on AArch64, EC `0x07` (FP/SIMD access trapped) is
  mapped to `ARCH_TRAP_INVALID_OPCODE`, which for a user thread is
  `SIGILL` and for the kernel is a panic. That is the right answer while
  the feature is off and exactly the wrong one once it is on.

## Why it matters

1. **Real programs.** The Linux personality exists so that software
   built for Linux runs here. On AArch64 it can run only software built
   to avoid SIMD, which no real toolchain produces by default. The
   canary the tree already has (`hello_musl`) cannot even be built for
   that architecture today.
2. **`%f`.** The first thing anyone notices about a system that cannot
   do arithmetic with a decimal point. It is also the smallest possible
   consumer, which makes it the honest test of whether the rest works.
3. **A rule that is only half-enforced is the shape this tree has been
   caught by before.** `arch/fpu.h` states three rules; AArch64 satisfies
   them by making the feature unavailable, which is not the same thing.
   The rules have never been tested on that architecture, and the moment
   FPEN is set they all become real at once — including the guest rule,
   in an EL2 backend that has never had vector state to swap.
4. **The architectural question: eager, or lazy?** x86-64 chose eager
   deliberately, and the reason is written down: no lazy ownership, so a
   thread cannot observe another's registers through a fault it did not
   take. AArch64 hands you lazy for nothing — `FPEN` traps the first use
   and the trap is the natural place to allocate. Two architectures with
   two policies is a fork in a rule the header states once. This unit
   either keeps one rule for both, or produces the measurement that
   justifies two.
5. **It is the last thing between the console the machine just gained
   and ordinary software on it.** A person can now look at the screen
   and type; what they can run is still only what was compiled to
   pretend the last thirty years of instruction sets do not exist.

## Proposed design

Five steps, in the order they should be built.

### 1. AArch64: real FP/SIMD state (`kernel/arch/aarch64/fpu.c`)

`struct arch_fpu_state` is the 512 bytes of `Q0`–`Q31` plus `FPSR` and
`FPCR`, 16-byte aligned. `arch_fpu_alloc` gives a thread one, zeroed,
with `FPCR` at its architectural reset value; the switch hook in
`context.c` saves the outgoing owner's registers and restores the
incoming owner's, exactly as x86-64's does; `arch_fpu_free` releases it.
`CPACR_EL1.FPEN` is set to allow EL0 and **not** EL1, so the kernel rule
is enforced by hardware rather than by the compiler alone: kernel code
that touches a vector register still traps, which is the property x86-64
gets from `-mgeneral-regs-only` plus review and AArch64 can get from the
machine.

The trap at EC `0x07` stops being an invalid opcode for user threads.
What it becomes is the question in §4 above: with eager allocation it is
"this thread should already own state" — a kernel bug, and a fault. With
lazy allocation it is the ordinary path: allocate, enable, retry. The
plan builds **eager first**, because it is the rule already written
down, and lets the benchmark below argue for lazy if it can.

### 2. The signal frames

A handler that uses FP must not corrupt the FP the interrupted code was
using, which means the frame carries the state and `sigreturn` restores
it. x86-64 does this already. AArch64 gains an `fpsimd_context` in the
`mc->reserved` area of `struct lx_sigcontext_a64` — the ABI's magic,
size, `fpsr`/`fpcr` and 32 vector registers — before the `esr_context`
and the terminator, and `sigreturn` restores from it. The native signal
path gets the same treatment through the same accessors
(`arch_user_fpu_image_size/save/restore`), which on AArch64 describe the
`fpsimd_context` body rather than an FXSAVE image.

### 3. The guest rule

`arch/hv.h`'s EL2 backend swaps the guest's vector state with the owner
thread's around every entry, as the VMX and SVM backends do. Until now
there has been nothing to swap; `hv-guest-fpu` becomes a real test on a
second architecture rather than a trivially satisfied one.

### 4. Userland stops pretending

`-mgeneral-regs-only` comes off the user flags in `build/arch/*.mk` and
off `libc/libc.mk`. Every program in the tree is then compiled by a
toolchain that uses SSE or NEON wherever it likes — which is the real
test, and a far broader one than any self-test: `sh`, `pkg`, `init`,
`cosmofs`'s userland tools and the whole boot's user-mode work start
executing vector instructions on every string operation.

`printf` gets `%f`, `%e` and `%g` for real: fixed-precision conversion
of a `double`, no `long double`, no `%a`, no libm. The bound is
deliberate — correct rounding of the last digit is a rabbit hole and the
consumer here is diagnostics, not numerics.

### 5. The canary

`hello_musl` on AArch64 wherever a cross musl exists (the CI runner
installs `musl-tools`; whether that provides an aarch64 compiler on the
aarch64 job is step 5's first question, and if it does not, the report's
claim becomes "a hand-built static binary with a NEON `memcpy`", which
is the same test with more work).

## Affected files

| File | Change |
| --- | --- |
| `kernel/arch/aarch64/fpu.c` | the state, the switch, the trap path; the test hooks become real |
| `kernel/arch/aarch64/context.c`, `cpu.c` | the switch hook; `CPACR_EL1.FPEN` for EL0 at CPU bring-up |
| `kernel/arch/aarch64/trap.c` | EC `0x07` is no longer an invalid opcode |
| `kernel/arch/aarch64/include/arch/*.h` | the image accessors' AArch64 shape |
| `kernel/arch/aarch64/hv_el2*.c` | the guest rule for vector state |
| `compat/linux/signal.c`, `compat/linux/abi_a64.h` | `fpsimd_context` in the frame and in `sigreturn` |
| `kernel/process/signal.c` | the native frame, through the same accessors |
| `build/arch/aarch64.mk`, `build/arch/x86_64.mk`, `libc/libc.mk` | user flags lose `-mgeneral-regs-only` |
| `libc/src/printf.c` | `%f`, `%e`, `%g` |
| `kernel/core/selftest.c`, `kernel/arch/aarch64/testhooks.c` | `fpu-switch` becomes real on AArch64; new tests below |
| `tests/linux/linux.mk`, `tests/linux/lxsig.c` | the canary and an FP signal case |
| `docs/kernel/arch/aarch64/*`, `docs/kernel/arch/design.md`, `docs/compat/linux/*`, `docs/libc/*` | the rules, now enforced on both |

## New APIs

- **None in `arch/fpu.h`.** It was written for exactly this and says so;
  the point of the unit is that AArch64 stops implementing it as no-ops.
  If something has to change shape, that is a finding worth the report.
- `arch_user_fpu_image_size/save/restore` gain an AArch64 meaning (the
  `fpsimd_context` body). The generic signal code keeps calling what it
  already calls.
- `libc`: `%f`, `%e`, `%g` in `printf`, and whatever minimal
  `double`-to-decimal helper they need, private to the library.
- No syscall, no user-visible interface, no new kernel object.

## Migration plan

1. **State and switching on AArch64**, with FPEN on and userland still
   built without FP. Nothing in the boot uses a vector register yet, so
   this step is proved by the tests alone: `fpu-switch` on AArch64, and
   a kernel that still faults if it touches a vector register itself.
2. **The signal frames**, both native and Linux, both architectures.
   Proved by a handler that clobbers vector registers and an interrupted
   loop that notices if they change.
3. **The guest rule**, so `hv-guest-fpu` means something on AArch64.
4. **Userland without the flag**, which turns the whole boot into the
   test, plus `%f`. This is the step with the blast radius, and it comes
   after the three that make it safe.
5. **The canary**: a real Linux binary with a real libc on AArch64.

Each step is a boot that either passes the chain or does not; nothing is
removed at any step, and a machine whose CPU lacks FP/SIMD — none exists
in the matrix — would keep the current behaviour by leaving FPEN alone.

## Tests

**`fpu-switch` on AArch64** (the existing test, no longer trivially
true): two threads set different patterns in `Q0`–`Q31`, yield to each
other repeatedly, and each finds its own pattern intact.

**`fpu-signal`** (new, both architectures): a user thread computes in
vector registers in a loop; a signal arrives; the handler fills every
vector register with a different pattern and returns; the interrupted
loop's registers are unchanged and its result is right. This is where FP
bugs live, and it is one test on both architectures because the frames
differ and the property does not.

**`fpu-exec`** (new): a thread that has never used FP has no state
(`thread->fpu == NULL`), and one that has, has it; after `exec` the
state is the architectural reset value and not the previous program's.
`fork` and `clone` copy it.

**`hv-guest-fpu` on AArch64** (the existing x86 test, second
architecture): a guest fills the vector registers, the host thread's are
unchanged.

**The kernel rule, still enforced**: a self-test that executes a vector
instruction from kernel context must fault, not corrupt a thread's
state. On AArch64 that is `FPEN` denying EL1; on x86-64 it stays a
review property of the build flags, which the test states.

**The whole boot** (step 4): every user program compiled with SIMD.
`sh`, `pkg`, `init`, the userland tools and the shell test exercise NEON
and SSE string operations continuously; a broken save or restore shows
up as corrupted output somewhere in a boot that runs hundreds of
processes.

**`%f`**: the libc's host test (`tests/host/test_libc.c`) gains the
float conversions, including the ones that catch a naive implementation
— zero, negative zero, a value that rounds up into another digit,
infinity and NaN, and a `%g` that must choose the exponent form.

**The canary**: `hello_musl` on AArch64, run under the personality by
`/etc/rc.test`, printing from a libc that uses NEON before it prints
anything.

**Shapes**: both architectures, `QEMU_SMP=1` and 4 (a thread's state
must survive migration between CPUs), release, `test-crash`, `analyze`,
`reproducible`. The chain grows by no steps; it grows a great deal of
coverage inside the steps it has.

## Benchmarks

Two questions, both of which decide code:

1. **What does owning FP state cost a context switch?** Time a
   ping-pong between two threads that own state and two that do not, on
   both architectures. That number, times the number of threads that
   own state once libc uses SIMD (which is: all of them), is what eager
   switching costs the system. If it is large, lazy switching earns its
   security argument a rebuttal and the report is wrong about §4 — which
   is the outcome worth having either way.
2. **What does SIMD buy the programs that now use it?** `memcpy` and
   `strlen` throughput before and after, in a user program, on both
   architectures. If NEON and SSE buy nothing measurable at these sizes,
   that is worth knowing before every program in the tree grows a
   dependency on register state the kernel must now preserve — and §21
   would say the userland flag should go back on, with the kernel
   support kept for the Linux personality, which needs it regardless.

## Risks

- **The blast radius is the whole userland.** Step 4 changes the machine
  code of every program at once. That is the point — nothing else tests
  save and restore as thoroughly — but it means a bug in step 1 shows up
  as an unrelated program misbehaving three steps later. Hence the
  order: state, signals, guests, and only then the flag.
- **Signal frames are where this goes wrong.** A handler that runs with
  the interrupted thread's vector registers, or a `sigreturn` that
  restores the wrong ones, produces corruption a long way from the
  cause. The test above is written before the code.
- **Lazy state and SMP migration.** If the measurement argues for lazy,
  the state must follow a thread between CPUs, and "the CPU that last
  owned these registers" becomes a thing to get right. Eager has no such
  problem, which is part of why it is first.
- **The EL2 backend.** Guest entry becomes a place where two owners of
  the same registers meet. The existing x86 backends are the model.
- **`%f` is a rabbit hole** — correct rounding, subnormals, `%a`, long
  double. The bound is stated in the design and the host test enforces
  it: fixed precision, `double` only, no libm.
- **CPU support**: FP/SIMD is architecturally mandatory on AArch64 in
  the profiles this project targets and on x86-64 (SSE2 is in the base
  ABI), so there is no fallback path to write. SVE is explicitly not in
  scope; a machine with SVE runs the FP/SIMD state fine.
- **Size**: about 250 lines for the AArch64 state and switch, 200 for
  the signal frames on both, 100 for the guest rule, 250 for `%f` and
  its host test, and 300 of tests — roughly 1 100, spread thin across
  many files rather than concentrated in a new one.

## Alternatives considered

- **GICv3** (`docs/kernel/arch/aarch64/design.md`, "Future
  extensibility") — a third interrupt-controller backend behind a seam
  two backends already prove, for machines this project cannot test on
  yet. It is the right unit the day real ARM hardware arrives; it makes
  nothing run that does not run today.
- **ASID allocation** instead of the full TLB invalidate per context
  switch — a measurable optimisation of something that works, and the
  measurement comes first (§21). A good unit, and a smaller one; it
  would pair naturally with this unit's context-switch benchmark, which
  is the same measurement from the other side.
- **A HID report-descriptor parser** (mice, and keyboards that refuse
  the boot protocol) — named by the console unit, a few hundred lines,
  and it needs an input subsystem to deliver anything to, which needs a
  second input device to be worth its interface. Circular until someone
  wants a pointer.
- **GPU, Wi-Fi, Bluetooth** — §60 says later, and nothing has changed.
- **NCQ** — measured and refused by the AHCI unit.
- **Nothing: keep `-mgeneral-regs-only` everywhere.** Defensible for the
  kernel forever, and for userland exactly as long as nobody wants to
  run a program somebody else compiled.
