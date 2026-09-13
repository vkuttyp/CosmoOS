# NEXT SUBSYSTEM — `__thread`, and the TLS image a program brings with it

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: the compiler already emits thread-local storage, the linker
already places it, and nothing in this system loads it.** `__thread int x;`
compiles on both architectures today -- it produces `.tdata`/`.tbss` and
local-exec relocations, `R_X86_64_TPOFF32` and
`R_AARCH64_TLSLE_ADD_TPREL_HI12`/`LO12_NC` -- and `kernel/process/elf.c`
skips the `PT_TLS` segment that describes where the image goes
(`if (ph.p_type != PT_LOAD) continue;`). So a program that uses a
thread-local variable **links, loads and runs**, and every access reads or
writes whatever happens to lie at the thread pointer plus the linker's
offset. That is silent corruption of someone else's memory, not a
diagnosable failure, and the errno/TLS unit named this one as the real
answer it was the prerequisite for.

**It also has a latent collision to fix, which is why this report exists
now rather than later.** The two architectures put thread-local variables
on opposite sides of the thread pointer, and only one of them agrees with
the 128-byte block libc installed in #120:

| | where the compiler puts the first `__thread` variable | agrees with `struct __cosmo_tcb`? |
| --- | --- | --- |
| x86-64 (psABI variant II) | **below** the thread pointer: `TPOFF` is negative, and `%fs:0` must hold a pointer to itself | **yes.** `self` at offset 0 is exactly what the ABI asks for, and the image grows downward into space nothing uses |
| AArch64 (variant I) | **above** it: `TPREL(x)` is positive and the ABI reserves 16 bytes at the thread pointer, so the first variable sits at **TP + 16** | **no.** Offsets 16..127 are `reserved[112]`, which `cosmo/tcb.h` promises to a program's own per-thread storage |

The x86-64 layout was right by accident -- the `self` word exists because
the architecture cannot read its own FS base, and the psABI wants the same
word for the same reason. The AArch64 one was not, and nothing detects it
today because nothing places a TLS image at all.

## Problem

- **A thread-local variable is silently wrong.** No diagnostic, no link
  error, no fault on a well-formed program: the access lands at
  `TP ± offset` in memory that belongs to libc's block (AArch64) or to
  whatever precedes the block (x86-64). The first symptom would be a
  corrupted `errno` or tid on one architecture and unrelated damage on the
  other.
- **libc still has shared state it cannot fix without this.** `strerror`'s
  buffer and `getcwd(NULL)`'s storage are the last two things invariant L8
  names, and the errno unit left them because moving them into the 112
  reserved bytes only trades one hand-rolled slot allocator for another.
  With `__thread` they are two keywords.
- **Every program that wants per-thread state must go through libc.**
  `cosmo_tcb_install` and the reserved prefix exist because there was no
  other way; a program with three thread-local counters has to lay them
  out by hand inside a block whose layout is documented in a header it
  does not own.
- **The reserved-prefix rule is a promise this unit must break.** `tcb.h`
  says a program's own storage starts at `COSMO_TCB_SIZE` and that the
  prefix is permanent. On AArch64 that space is exactly where the ABI puts
  `__thread`. Whatever this unit does, that sentence changes -- and it is
  better changed by the unit that understands why than by the first person
  whose `__thread` variable eats their `errno`.
- **`vmctl` is now the shape of program that will want it.** Four vCPU
  threads, each with per-thread device state that is currently reached
  through arrays indexed by a vCPU number -- which is what a program writes
  when the language's own mechanism does not work.

## Current implementation

**The compiler and linker are ready.** Verified by building
`__thread int a = 7; __thread long b;` for both targets:

```
aarch64: mrs x8, TPIDR_EL0            R_AARCH64_TLSLE_ADD_TPREL_HI12  a
         add x9, x8, #0x0, lsl #12    R_AARCH64_TLSLE_ADD_TPREL_LO12_NC a
x86-64:  movq %fs:0x0, %rcx           R_X86_64_TPOFF32                a
         movl (%rcx), %eax
```

Both are the **local-exec** model, which is the only one a system with no
dynamic linking needs: the offsets are resolved at link time, there is no
`__tls_get_addr`, no GOT entry and no relocation left for a loader to
process. The sections are `.tdata` (initialised) and `.tbss` (zero), and
the linker emits a `PT_TLS` program header giving their address, `filesz`,
`memsz` and alignment.

**The loader ignores it.** `kernel/process/elf.c` handles `PT_INTERP`,
`PT_NOTE`, `PT_GNU_STACK` and `PT_LOAD`, and continues past everything
else. `.tdata` does live in the image -- it is inside a `PT_LOAD` segment
-- but nothing copies it anywhere per thread, and `.tbss` occupies no file
space at all.

**The thread pointer exists and is installed for every thread**
(`SYS_set_tls`, the unit before last): a `.bss` block for the first thread,
a page beside the stack for every thread `cosmo_thread_start` makes, and
`cosmo_tcb_install` for a thread made outside libc's wrapper. What is
behind that pointer is libc's 128-byte `struct __cosmo_tcb`: `self`, `err`,
`tid`, `reserved[112]`.

**What libc still shares**, from invariants L8: `strerror`'s static buffer
and `getcwd(NULL)`'s storage.

## Why it matters

- **It is the unit the last one was built for.** The errno/TLS report said
  so in as many words: "this design is deliberately the one that does not
  block it -- the thread pointer it adds is what `PT_TLS` would use."
- **It turns a silent wrong answer into a working feature.** Today the
  failure mode of `__thread` is memory corruption; there is no middle
  state where it merely does not compile.
- **It finishes invariant L8.** `strerror` and `getcwd(NULL)` are the only
  named gaps left, and both become one keyword each.
- **It removes a documented promise that is false on one architecture.**
  The reserved prefix and AArch64's TLS want the same bytes.
- **It is small, because the hard part is already done.** Local-exec TLS
  needs no dynamic linker, no lazy allocation and no per-object bookkeeping:
  a template to copy, a per-thread image to copy it into, and a thread
  pointer that already exists.

## Proposed design

### The kernel reads the template; libc places the image

`PT_TLS` describes a *template*, not an allocation: `(address, filesz,
memsz, align)`. The loader gains four lines -- recognise the segment,
bounds-check it like `PT_LOAD`, and record it on the process -- and
nothing else. Placement stays in libc, for the same reason the block's
layout did: **what lives behind the thread pointer is the library's
business**, and a kernel that placed the image would be choosing the ABI
variant on libc's behalf.

libc learns the template through `struct cosmo_procinfo`, which already
carries per-process facts and is already how a program asks about itself:

```c
    uint64_t tls_addr;    /* the template's address in the image, 0 if none */
    uint32_t tls_filesz;  /* initialised bytes to copy */
    uint32_t tls_memsz;   /* total bytes, the rest zero */
    uint32_t tls_align;   /* the alignment the ABI demands of the image */
```

An older program that never asks gets what it gets today.

### The layout, which differs by architecture because the ABI does

Both architectures keep libc's block; what changes is where it sits
relative to the thread pointer.

```
x86-64 (variant II)            AArch64 (variant I)
  low                            low
  [ libc block        ]          [ libc block        ]
  [ TLS image (memsz) ]          [ 16-byte ABI head  ] <- TP
  [ TCB: self at TP+0 ] <- TP    [ TLS image (memsz) ]
  high                           high
```

- **x86-64 needs no change to the block at all.** `self` at `TP+0` is what
  `%fs:0` must hold, `TPOFF` is negative, and the image goes immediately
  below the thread pointer. `__errno_location` stays one load and an
  offset.
- **AArch64 moves the block below the thread pointer**, leaving the ABI's
  16 reserved bytes at `TP` and the image above them. The accessor becomes
  `TP - sizeof(struct __cosmo_tcb)` instead of `TP` -- still one register
  read and one constant offset, and still no branch.

The consequence for the public rule: **`reserved[112]` stops being a
program's per-thread storage and becomes libc's own**, because
`__thread` is now the way a program gets per-thread storage. That is a
better bargain than it sounds -- the reserved space existed only because
the language's mechanism did not work.

### Per-thread images

- **The first thread**: `__libc_start` allocates image + block together,
  copies `filesz` bytes from the template and zeroes the rest, then
  installs the thread pointer. The block is no longer a `.bss` object,
  because its size now depends on the program's `memsz` -- which brings
  back the failure path the errno unit deliberately removed, so the same
  answer applies: **a failure here writes one line to file descriptor 2
  and exits 127**, since a process with no TLS cannot run its own `main`.
  A program with no `PT_TLS` keeps the static block and no allocation.
- **Every thread `cosmo_thread_start` makes**: the mapping already carries
  a page for the block; it carries image + block instead, rounded to the
  template's alignment. A `memsz` larger than a page grows the mapping
  rather than failing.
- **A thread made outside libc's wrapper**: `cosmo_tcb_install` gains the
  same job -- it is given storage and told how much -- and the contract in
  `cosmo/thread.h` gains a sentence: a raw thread that wants `__thread`
  must install a block *and* an image, which `cosmo_tcb_install` does when
  the caller's buffer is large enough.

### Scope

**Local exec only**, which is all a static system can generate: no
`__tls_get_addr`, no dynamic TLS, no `PT_TLS` in a shared object because
there are none, and no lazy allocation. `_Thread_local` (C11) is the same
mechanism and comes free.

## Affected files

| file | change |
| --- | --- |
| `kernel/process/elf.c` | recognise and bounds-check `PT_TLS`; record the template |
| `kernel/include/kernel/process.h` | the template on `struct process` |
| `kernel/include/uapi/cosmo/syscall.h` | four fields on `struct cosmo_procinfo` |
| `kernel/syscall/native.c` | fill them |
| `libc/src/tcb.c` | allocate image + block; the AArch64 accessor's offset; `cosmo_tcb_install` takes an image |
| `libc/include/cosmo/tcb.h` | the layout, and the reserved-prefix rule that changes |
| `libc/src/stdlib.c` | `__libc_start` places the first thread's image |
| `libc/src/thread.c` | image + block in the thread's mapping |
| `libc/src/errno.c`, `-/unistd.c` | `strerror` and `getcwd(NULL)` become `_Thread_local` |
| `libc/include/cosmo/thread.h` | the raw-thread contract gains the image |
| `userland/tests/thrtest.c` | the new steps |
| `docs/libc/invariants.md` | L8 finished; the reserved-prefix rule |
| `docs/kernel/process/design.md`, `-/testing.md` | §13's named consumer; the loader's new segment |
| `README.md` | Status entry |

## New APIs

No new syscall. Four fields on an existing structure, one changed libc
function (`cosmo_tcb_install`), and a language feature that needed no API
at all.

## Migration plan

1. **The loader and the template**, with a kernel self-test: a binary with
   `PT_TLS` loads, the template is recorded, and a malformed one (memsz <
   filesz, outside the file, absurd alignment) is refused the way a bad
   `PT_LOAD` is. Nothing uses it yet.
2. **The layout change on AArch64 alone**, before any image exists: the
   block moves below the thread pointer and the accessor gains its offset.
   The whole suite is the regression test, and doing it first means the
   step that adds images is not also a layout change.
3. **Images for every thread libc makes**, first thread included.
4. **`cosmo_tcb_install` takes an image**, for threads libc did not make.
5. **`strerror` and `getcwd(NULL)` become `_Thread_local`**, which is the
   proof the feature works from inside the library that provides it.
6. **The tests**, then the bug-proofs.
7. **The documents**, including the reserved-prefix rule and L8.

## Tests

1. **A thread-local variable is per-thread**: three threads each write
   their own id to the same `__thread` variable in a loop and read it
   back; a shared one loses this immediately. The mirror of the `errno`
   test, and for the same reason -- it is the property the unit exists for.
2. **`.tdata` is initialised per thread**: a `__thread int x = 0x5eed;`
   reads its initialiser in *every* thread, not just the first -- which is
   what distinguishes a copied template from a single shared image.
3. **`.tbss` is zero per thread**: a large `__thread` array is zero in a
   new thread even after an earlier thread filled it, which catches an
   implementation that maps the same image twice.
4. **The image does not overlap the block**: a thread fills its whole
   `__thread` array with a byte and then checks `errno`, the cached tid and
   `self` -- the collision this report is named for, asserted rather than
   argued, and the one test that would have failed on AArch64 before the
   layout moved.
5. **A raw thread with an installed block and image** gets working
   `__thread` storage; one given a buffer too small for the image is
   refused.
6. **`strerror` from two threads at once** returns each thread's own
   message, which is L8's last line.

**Bug-proofs**: the image not copied (test 2 reads zero instead of the
initialiser); the same image shared by two threads (test 1 loses values);
`.tbss` not zeroed (test 3 sees the previous thread's bytes); the AArch64
block left at the thread pointer (test 4 sees `errno` change when the
array is written -- the collision, on demand); the alignment ignored (a
`__thread` variable with `_Alignas(64)` lands misaligned, which an
assertion on its address catches); and `PT_TLS` recorded without bounds
checks (a crafted header with `memsz` smaller than `filesz`, or an
enormous alignment, must be refused at load).

## Benchmarks

A `__thread` access is a thread-pointer read and a constant offset -- the
same cost as `errno` today, and the same on both architectures. What
changes measurably is thread creation, which gains a copy of `filesz`
bytes and a zeroing of `memsz - filesz`: reported for the suite's own
programs, whose images are tens of bytes, and not gated. `errno` itself
becomes one load *further* from the thread pointer on AArch64 and is
unchanged on x86-64; the suite's per-test timing is the check that neither
moves outside its spread.

## Risks

- **The reserved-prefix rule changes, and it was documented as
  permanent.** `cosmo/tcb.h` says a program's storage starts at offset 128
  and that the prefix will not move. On AArch64 the ABI wants those bytes.
  The mitigation is that the rule is replaced by something better rather
  than merely withdrawn -- `__thread` is the supported way to get
  per-thread storage after this unit -- but any out-of-tree program using
  the reserved space breaks, and the report says so rather than hoping.
- **The first thread's block stops being a `.bss` object**, which
  reintroduces the startup failure the errno unit removed on purpose. It
  is bounded to programs that *have* a `PT_TLS` -- one that does not keeps
  the static block -- and the exit-127 policy is the same one that unit
  argued for and that later caught a real alignment bug in one line.
- **A wrong offset is silent on the architecture that is right.** x86-64
  needs no layout change at all, so an AArch64 mistake passes every x86
  test. Test 4 exists for that, and the bug-proof for it must be run on
  both architectures to mean anything -- the same "a control must differ in
  one thing" shape as the `self` word two units ago, where AArch64 passed
  the entire suite with a broken x86.
- **`memsz` is attacker-controlled in a boot archive.** The loader must
  bound it like any other segment; an image that asks for a gigabyte per
  thread should fail the mapping, not the machine.

## Alternatives considered

- **Leave `__thread` unsupported and make the compiler reject it.** There
  is no flag that does this per-target in a way that survives a future
  toolchain, and the failure today is silent corruption rather than an
  error -- so "unsupported" is not a state the system can actually be in.
- **Place the image in the kernel.** The loader knows the template and
  could allocate. It would also be choosing the ABI variant, the alignment
  and the relationship to libc's block -- all of which are the library's,
  and one of which (x86-64's `self` word) exists only because of a
  hardware limitation libc works around.
- **Keep the block at the thread pointer on AArch64 and move the TLS
  image elsewhere.** The offsets are baked into every relocation the
  linker resolved; there is nowhere else to put it.
- **Dynamic TLS (`__tls_get_addr`, TLS descriptors).** Needed only with
  dynamic linking, which this system does not have. Named so that the
  local-exec-only decision is visibly a decision.

Named and deferred: `PT_TLS` in a shared object, and with it the general
dynamic model; a `SYS_mprotect` so that a thread's stack guard costs one
call rather than three mappings (a neighbour of this work, since both
touch `cosmo_thread_start`'s mapping); and the device models under two
guest CPUs, which the vCPU-threads unit named and which remains its own
unit.
