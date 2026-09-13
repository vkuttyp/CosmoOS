# NEXT SUBSYSTEM — `__thread`, and the TLS image a program brings with it

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. **This report is as built**, and
its central claim was wrong in a way only building could show.

**The report said a program using `__thread` "links, loads and runs" with
silent corruption, and that there is no state in which the feature is
merely unsupported. There is.** `userland/user.ld` declares no `PT_TLS`
and places no `.tdata`/`.tbss`, so `ld.lld` refuses the link outright:
*"has an STT_TLS symbol but doesn't have a PT_TLS segment"*. The failure
today is a **link error**, which is the good kind. What I verified was
that the *compiler* emits the relocations and the sections; I then asserted
that the link would succeed without trying it, and the first build of step
3 tried it. The linker script now places the template and declares the
segment.

Everything else the report says about the collision stands, and it was the
reason to do this unit: AArch64 puts the first `__thread` variable at
TP+16, inside the block libc installed two units ago, and x86-64 puts them
below the thread pointer where the shipped layout was already right.

Five more differences, each found by running rather than reading:

1. **The startup order gained exactly one exception.** The errno unit
   installs the thread pointer before anything else and has a bug-proof
   keeping it there -- but the block's *size* now depends on the program's
   own `PT_TLS`, so finding the auxiliary vector must precede it. With the
   old order the first thread silently got no image.
2. **An empty `PT_TLS` is the common case**, not an anomaly: `ld.lld`
   emits one for every program linked against a script that declares the
   segment, with `memsz` 0 and `p_align` 0. Refusing that alignment as
   not-a-power-of-two killed every program in the system at startup.
3. **The first thread's storage is a generous static array, not an exact
   one.** libc's own `strerror` buffer is `_Thread_local`, so every program
   has a template; if the static block were the minimum, every program
   would `mmap` at startup, and a syscall filter that does not name `mmap`
   would kill it. That is the trap `SYS_set_tls` fell into one unit ago,
   found the same way -- a filtered child died and the filter test said so.
4. **The validation is a pure function in a file of its own**
   (`libc/src/tlsscan.c`), tested on the host with tables no linker would
   emit. The report proposed a deliberately malformed binary in the boot
   archive; that would have proved one case, and this proves twelve.
5. **`getcwd(NULL)` never needed fixing.** It `malloc`s per call and hands
   the buffer to its caller, so it has been safe since the allocator took
   its lock -- invariant L8 named it for two units after it stopped being
   true. Only `strerror` had a shared buffer, which a `grep` for writable
   statics in `libc/src` confirms was the only one.

**Review has already corrected how the template reaches the program.** Two
drafts got it wrong -- one by growing `struct cosmo_procinfo`, which would
overflow an older binary's buffer because the kernel writes
`count * sizeof` with its own `sizeof`; one by declaring the template's
64-bit ELF sizes as `uint32_t`, which truncates. The answer was already in
the tree: the program headers are mapped, the loader already computes
where, and the native auxiliary vector already exists -- so the program
reads its own `PT_TLS` and the kernel learns nothing about TLS at all.

**Subsystem: the compiler already emits thread-local storage, the linker
already places it, and nothing in this system loads it.** `__thread int x;`
compiles on both architectures today -- it produces `.tdata`/`.tbss` and
local-exec relocations, `R_X86_64_TPOFF32` and
`R_AARCH64_TLSLE_ADD_TPREL_HI12`/`LO12_NC` -- and `kernel/process/elf.c`
skips the `PT_TLS` segment that describes where the image goes
(`if (ph.p_type != PT_LOAD) continue;`) -- and `userland/user.ld` declares
no `PT_TLS` and places no `.tdata`/`.tbss`, so **the link fails**:
`ld.lld` refuses an `STT_TLS` symbol with no `PT_TLS` segment. A
thread-local variable is therefore a build error today rather than silent
corruption, which is the good kind of unsupported and is what the banner
above corrects: this section originally claimed the program would link,
load and run. The errno/TLS unit named this one as the real answer it was
the prerequisite for, and that part is unchanged.

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

- **A thread-local variable does not build.** `ld.lld` refuses: the
  linker script declares no `PT_TLS` for the `STT_TLS` symbols to live in.
  Loud rather than silent, which makes this unit a feature to add rather
  than corruption to stop -- and which is the correction the banner
  records, since the first draft of this section asserted the opposite
  without trying a link.
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

## Design (as built)

### The program finds its own template, through the auxiliary vector

`PT_TLS` describes a *template*, not an allocation: `(address, filesz,
memsz, align)`. The program that needs it can read it **from its own
program headers**, which are already mapped -- the first `PT_LOAD` of every
binary here starts at file offset 0, so the ELF header and the program
header table land at `load_base + e_phoff` -- and which the kernel already
locates: `struct elf_info` carries `phdr_vaddr`, `phnum` and `phent`
today, computed for the Linux personality's `AT_PHDR`.

So the native auxiliary vector gains the standard trio, from values the
loader already has:

```c
#define COSMO_AT_PHDR  3   /* the program header table, mapped */
#define COSMO_AT_PHENT 4   /* one entry's size */
#define COSMO_AT_PHNUM 5   /* how many */
```

and libc walks them at startup to find its own `PT_TLS`. That is how every
real libc does it, and it means **the kernel needs no knowledge of thread
local storage at all** -- not a field, not a struct, not a syscall. The
loader does not even need a new branch: it already ignores `PT_TLS`, and
after this unit that is the correct behaviour rather than an omission.

**Two earlier drafts of this section were worse, and review found both.**
The first put four fields on `struct cosmo_procinfo`, which is unsafe:
`sys_procinfo` writes `count * sizeof(struct cosmo_procinfo)` using the
*kernel's* `sizeof`, so growing that structure makes an older binary's
buffer overflow -- and it is the wrong place besides, a table of every
process that libc would have to search by pid to learn its own image's
layout. The second declared the template's sizes as `uint32_t`, which
truncates: ELF's `p_filesz`, `p_memsz` and `p_align` are 64-bit, and a
header claiming more than 4 GiB of TLS would arrive as a small number that
passes every check. Reading the header directly removes both problems
instead of fixing them: the fields keep their own widths, and there is no
second copy of them to disagree.

**What libc must check**, since it is now reading a structure a program
image controls: `phent` equal to `sizeof(struct elf64_phdr)`, `phnum`
bounded, at most one `PT_TLS`, `memsz >= filesz`, the template's bytes
inside a mapped segment, and an alignment that is a power of two and no
larger than a page. A malformed header must exit 127 rather than place a
wrong image, for the same reason the startup path already does: a process
whose thread-local storage is wrong cannot be allowed to run `main`.

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
| `kernel/process/process.c` | three more pairs in the native auxiliary vector, from `elf_info`'s existing `phdr_vaddr`, `phent`, `phnum` |
| `kernel/include/uapi/cosmo/syscall.h` | `COSMO_AT_PHDR`, `-_PHENT`, `-_PHNUM` |
| `libc/src/tcb.c` | allocate image + block; the AArch64 accessor's offset; `cosmo_tcb_install` takes an image |
| `libc/include/cosmo/tcb.h` | the layout, and the reserved-prefix rule that changes |
| `libc/src/stdlib.c` | `__libc_start` places the first thread's image |
| `libc/src/thread.c` | image + block in the thread's mapping |
| `libc/src/errno.c`, `-/unistd.c` | `strerror` and `getcwd(NULL)` become `_Thread_local` |
| `libc/include/cosmo/thread.h` | the raw-thread contract gains the image |
| `userland/tests/thrtest.c` | the new steps |
| `docs/libc/invariants.md` | L8 finished; the reserved-prefix rule |
| `docs/kernel/process/design.md`, `-/testing.md` | §13's named consumer; the native auxiliary vector's three new tags. **The loader itself does not change** -- it already ignores `PT_TLS`, and after this unit that is correct rather than an omission |
| `README.md` | Status entry |

## New APIs

**No new syscall, no new structure and no changed structure**: three
auxiliary-vector tags whose numbers are the standard ones, one changed
libc function (`cosmo_tcb_install`), and a language feature that needs no
API at all. The kernel learns nothing about TLS.

## Migration plan (steps 3 and 4 landed together)

1. **The auxiliary vector's three tags**, with a test that a native
   program can find its own program headers and its own `PT_TLS` through
   them. No libc change beyond the reader; nothing places an image yet.
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
assertion on its address catches); and the header trusted
without checks (a crafted binary with `memsz` smaller than `filesz`, two
`PT_TLS` segments, an alignment that is not a power of two, or a template
outside every mapped segment -- each must exit 127 rather than place a
wrong image, and each needs a deliberately malformed binary in the boot
archive to prove, which is the one piece of test scaffolding this unit
adds).

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
- **`memsz` is attacker-controlled in a boot archive, and the check is
  libc's.** This is the cost of the kernel learning nothing about TLS: the
  validation the loader would otherwise have done moves into the library,
  where a wrong answer is one process's rather than the machine's. An image
  asking for a gigabyte per thread must fail its mapping and exit 127, and
  the bug-proof for that needs a deliberately malformed binary in the boot
  archive -- the one piece of scaffolding this unit adds. A reader who
  expects the loader to bound it will not find that code, so the report
  says where it is instead.

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
