# Writing kernel code in CeeSafe

The kernel is being written in CeeSafe, a memory-safe systems language with
C#/Java/TypeScript-like syntax that compiles to C
(https://github.com/vkuttyp/CeeSafe; documentation at
https://vkuttyp.github.io/CeeSafe/). CeeSafe is the fork of Nova 0.6.0,
the language this tree's first ports were written in; since 2026-10-31
it is the kernel's language, and Nova is no longer developed. This page tells a contributor, human or
AI agent, how to write it, how it becomes the C the build compiles, and
what the kernel asks of it. The build side (the pin, the CI job) is
[`design.md`, "CeeSafe sources"](design.md#ceesafe-sources).

## The model in one paragraph

A CeeSafe source `NAME.cee` is what is committed; the `NAME.c` it becomes
is not (roadmap 2.0's CeeSafe rule, point 7). Every build runs the pinned
compiler, which writes the C into `out/cee/kernel/<area>/NAME.c`, with a
banner, `GENERATED: do not edit`, naming its source, and compiles it from
there; so building CosmoOS needs the pinned ceesafec. You edit the `.cee`
file and build. A source tarball (`make dist`) carries the generated C
beside each source and builds with no ceesafec, which is how a self-hosted
CosmoOS builds its kernel; `make reproducible` checks that it builds the
same bytes.

## Files of a port

| File | Written by | What it is |
|---|---|---|
| `kernel/<area>/NAME.cee` | you | the code |
| `out/cee/kernel/<area>/NAME.c` | the build (`make cee`) | ceesafec's C for it, with the banner; not committed |
| `kernel/include/kernel/NAME.h` | you, or the build (CeeSafe first: `out/cee/kernel/include/kernel/NAME_types.h`) | the C interface (below: hand-written or generated) |
| `out/cee/kernel/include/ceesafe/rt.h`, `out/cee/kernel/ceesafe/ceesafe_rt_core.c` | the build | CeeSafe's runtime, shared by every port |
| `kernel/ceesafe/ceesafe_panic.c`, `tests/host/ceesafe_panic.c` | hand-written | what a failed CeeSafe check does: a kernel panic naming the CeeSafe line; `abort()` in host tests and fuzzers |
| a line in `scripts/cee-gen.py`'s `PORTS` | you | registers the port |
| `kernel/kernel.mk`, `tests/host/host.mk`, `tests/fuzz/fuzz.mk` | you | list `kernel/<area>/NAME.c` like any C file, with `ceesafe_rt_core.c` and a `ceesafe_panic.c`; the build takes it from `out/cee` (`build/cee.mk`) |

The current ports: `kernel/block/gpt.cee` (the GPT parser and writer),
`kernel/process/cputime.cee` (CPU time and the CPU-time timers),
`kernel/process/nice.cee` (the rules of nice values),
`kernel/scheduler/fair.cee` (the fair level's weights and virtual time)
and `kernel/ceesafe/kcore_test.cee` (the self-test `cee-kcore`, below).
Read them before writing a new one.

### CeeSafe's kernel library

CeeSafe ships a library for code built against this kernel's C API
(`lib/kernel` in CeeSafe's repository; ADR 0025 in its `spec/decisions`,
the API in its `spec/api` and the guide's kernel page): `KernelHeap`; `SpinLock<T>`,
`IrqSpinLock<T>` and `Mutex<T>`, whose data is reached only through a
guard, and spin locks in `static` items built at compile time with
`SPINLOCK_INIT`; `PreemptGuard`, `IrqGuard` and `PerCpu<T>`; `KRef<T>`
over `struct kobject`; `Quiesce` and `deferDrop`. A port uses it by
setting the last field of its `PORTS` line (`ceesafec --kernel cosmoos`).
`IrqGuard`, `PreemptGuard` and `PerCpu` call primitives the kernel does
not export, so they are for code built into the kernel, not modules.
A `Mutex` can be a static too (ADR 0027): its
`@constant` constructor expands `MUTEX_INIT_AT(p, name)` (`mutex.h`),
given the mutex's own address, which its self-pointing lists need.
`ObjectCache<T>` is a typed slab cache: a static, set up once at
start-up (`setUp`), whose `alloc(value)` returns a `CacheBox<T>` that
drops the value and frees the object when dropped (in v1, `T` holds no C
state). The self-test `cee-kcore` (`kernel/ceesafe/kcore_test.cee`, named
by `kcore_selftest.c`) exercises each type in every debug boot, and checks
that `MUTEX_INIT_AT` leaves a mutex as `mutex_init` does.

## Setting up

Install the pinned compiler, the version in `scripts/cee-gen.py`
(`python3 scripts/cee-gen.py --pinned`); `make cee` refuses any other:

```sh
curl -fsSL https://vkuttyp.github.io/CeeSafe/install.sh | CEESAFE_VERSION=v0.2.0 sh   # into ~/.ceesafe/bin
export PATH="$HOME/.ceesafe/bin:$PATH"     # or: export CEESAFEC=~/.ceesafe/bin/ceesafec
```

An AI agent with MCP support should also attach the compiler's MCP server.
It checks code, applies the compiler's own fixes, explains diagnostics and
searches the language specification:

```sh
claude mcp add ceesafe -- ceesafec mcp        # Claude Code; other clients: run `ceesafec mcp` over stdio
```

## The workflow

1. **Write or change `NAME.cee`.** New code starts from the closest
   existing port, which has the right imports, exports and safety comments.
2. **Check, in a loop:**
   `ceesafec check --profile freestanding --no-float kernel/<area>/NAME.cee`.
   With MCP, use `ceesafe_check`. Then `ceesafec fix` (`ceesafe_fix`) applies the
   fixes the compiler is sure of, and `ceesafec explain N0294` (`ceesafe_explain`)
   explains any code. Do not guess at a diagnostic; read its explanation.
3. **Format:** `ceesafec fmt kernel/<area>/NAME.cee`.
4. **New port only:** add its line to `PORTS` in `scripts/cee-gen.py`,
   and its sources to the makefiles, like `cputime.c`'s
   (`kernel/kernel.mk`; a host test in `tests/host/host.mk`; a fuzz target
   in `tests/fuzz/fuzz.mk`).
5. **Build:** the build generates `out/cee/kernel/<area>/NAME.c` (and
   the header, if the port has a generated one) before it compiles
   anything; `make cee` does only that, to read the C.
6. **Test** as for any C change: the validation matrix in `AGENTS.md`,
   both architectures, and `make cee-check` (every source compiles, the
   negative sources are refused).
7. **Commit `NAME.cee`**, never its generated C (`out/` is not tracked).

## The header: two models

**Header first (the default; every current port).** The C header is
hand-written, like any kernel header: it declares the structs, the
constants and the prototypes, with comments, and C callers include it.
The CeeSafe source imports the structs it touches, with the header that
defines them:

```ceesafe
@header("kernel/cputime.h")
extern "C" {
    struct cputime_pair {
        user_ns: u64;
        system_ns: u64;
    }
}
```

It then defines each function under the header's name:

```ceesafe
/// Safety: `ct` points to a live cputime whose lock the caller holds.
@export("cputime_tick")
@context(irq)
unsafe fn tickC(ct: *mut cputime, user: bool, tickNs: u64) -> u32 => tick(Ptr.toBorrowMut(ct), user, tickNs);
```

The generated C `#include`s the header, so the C compiler checks every
export against its prototype. ceesafec also emits static assertions that each
imported field has the type and size the CeeSafe declaration says. A header
change that the CeeSafe side does not follow fails the build. Choose this
model when C code owns the structs, as kernel structs usually are
(embedded in `struct process`, guarded by C locks), or when the header
needs macros or documentation.

**CeeSafe first.** CeeSafe defines the structs (`@repr(C) struct ...`) and the
exports, and ceesafec writes the header. Give the port's `PORTS` line a
fourth element, the header's path (`"kernel/include/kernel/NAME.h"`).
The build then writes ceesafec's interface there, under `out/cee`: the `@repr(C)` structs
and a prototype per export, with a banner and an include guard from its
name under `kernel/include` (`kernel/NAME.h`, passed as ceesafec's
`--exports-header`). The generated C includes the header by that name
rather than defining the structs again (ADR 0030), so a kernel
header may include it and the port may include that kernel header: the
handle table's `handle_types.h`, which `handle.h` includes, is one. Choose
it for a component whose types CeeSafe owns. The header cannot carry macros
or comments, so a C header that includes it carries them, or C callers
learn the API from the `.cee` file's doc comments.

Either way only these types cross between C and CeeSafe: integers, `bool`,
raw pointers to them or to C-compatible structs, `@repr(C)` structs and
`@repr(uN)` enums (CeeSafe's `type-system.md` §28.3). A slice crosses as a
pointer and a length; references, optionals and CeeSafe strings do not cross.

## Rules for CeeSafe in the kernel

- **Profile.** Always `--profile freestanding --no-float`: no hosted
  library, and no floating point (the kernel saves no FP state for it).
  `make cee` passes both.
- **`unsafe` only at the boundary.** Raw pointers from C become references
  and slices once, in the exported wrapper (`Ptr.toBorrowMut(p)`,
  `SliceMut.fromRaw(p, n)`). Everything below works on references and
  slices, which are bounds- and lifetime-checked. Every `unsafe fn`
  carries a `/// Safety:` comment saying what the C caller guarantees
  (N0283 without one). Say exactly what the header's contract is: who
  holds which lock, which pointers may be NULL.
- **A failed check is a kernel panic.** CeeSafe checks every index, every
  integer overflow, every shift and every division. In the kernel a failed
  check calls `ceesafe_panic`, which panics and names the CeeSafe line
  (`KERNEL PANIC: ceesafe panic: index out of bounds at
  kernel/ceesafe/crash_test.cee:10:18`). `make test-crash-cee` (CI's stress
  part, both architectures) proves that: a `CRASH_TEST=5` kernel calls
  `kernel/ceesafe/crash_test.cee` with an index past its table, and the
  harness (`run_boot_test.py --expect-panic ceesafe`) requires that report
  with the file and the line the source marks. So either make the failure
  impossible or make it explicit:
  - Code that runs in interrupt context, or under a spin lock, must not
    fail on any input. Use `saturatingAdd`/`wrappingSub`/`checkedMul`
    (each says what happens at the limit) where an ordinary `+` could
    overflow, and compare before subtracting. `cputime.cee` is the model.
  - Code that parses untrusted input, such as a disk or a packet, should
    reject a bad input as an error result, not reach a failing check.
    `gpt.cee` is the model: every refusal has a reason.
- **Context effects.** Mark a function that may run in interrupt context
  `@context(irq)`, and one that runs under a spin lock `@context(atomic)`.
  The compiler then refuses anything that could sleep or allocate there
  (N0295). The C caller's context is not visible to CeeSafe, so this
  annotation is the only check.
- **Locks stay in C.** The exported function's caller holds whatever lock
  the header says; the CeeSafe code takes none, signals nothing and allocates
  nothing (no current port does). A signal to send, an object to free, or
  work to queue comes back to the C caller as a result, which acts on it
  after unlocking (`cputime_tick` returns the due signals).
- **Imported C structs never move.** They are C state: borrow them and
  write their fields in place. `let t = c.virt;` is refused (N0294), and
  so is an `if` expression that would pick one of two.
- **No hidden C calls.** Generated C has no compiler-rt libcalls. CeeSafe has
  no 128-bit integer; exact wide arithmetic is written out
  (`cputime.cee`'s `scale`).

## Testing a port

Hold a port to the same tests as C code, and more, since the CeeSafe side is
cheap to drive from the host:

- **A host test** (`tests/host/test_NAME.c`) linking `NAME.c`,
  `kernel/ceesafe/ceesafe_rt_core.c` and `tests/host/ceesafe_panic.c`. A failed
  check aborts, so ASan and UBSan builds catch it.
- **A fuzz target** (`tests/fuzz/fuzz_NAME.c`), for anything that takes
  untrusted input or carries a state machine. Assert the invariants the
  header promises after every step, and supply seeds (`fuzz_seed`).
- **The in-kernel or guest test** for the behaviour, written to fail on
  main first, as for any change (`AGENTS.md`).
- **A port that replaces C** also gets a differential test against the C
  original. CeeSafe's repository runs one for GPT (`integrations/cosmoos/c9.py`).

## Common mistakes

| Mistake | What happens | Instead |
|---|---|---|
| editing the generated `out/cee/.../NAME.c` | the next build writes it again | edit `NAME.cee` |
| new C in a port's `*_floor.c` beyond the C the port deletes | CI's `cee-share` job fails | keep the floor to what CeeSafe cannot express; delete the C the port replaces in the same change |
| committing generated C | `cee-share` counts it as C to convert, and the tree is two copies of one thing | commit the `.cee` only |
| a ceesafec other than the pinned one | the build refuses it; another version's C differs | install the pinned release; bumping the pin is its own change |
| `+` on a counter in interrupt context | a possible kernel panic on overflow | `saturatingAdd`, or prove the bound |
| a borrow-taking parameter called without `mut` | N0249 | `f(mut x)`: a call that lends mutably says so |
| copying an imported struct out of a field | N0294, N0251 | borrow it; act on each field in place |
| `unsafe fn` without `/// Safety:` | N0283 | state the C caller's obligations |
| a float, or a hosted library call | refused by `--no-float` and the profile | integers; a C function declared in `extern "C"` |

## Upgrading the compiler

Moving to a newer ceesafec is a change of its own. Bump `CEESAFEC_VERSION` in
`scripts/cee-gen.py`, the CI job's `CEESAFEC_SHA256` and `ci/Dockerfile`'s
`CEESAFEC_VERSION` and `CEESAFEC_SHA256` (from the release's `SHA256SUMS`), run
`make cee` before and after, and review the difference of the two
`out/cee` trees. Usually only version lines and locations change;
anything else needs an explanation.
Then run the validation matrix on both architectures. How CeeSafe makes a
release is its `docs/release.md`.

## Porting C to CeeSafe: c2cee

The SA track converts this tree's C file by file. CeeSafe's
`integrations/cosmoos/c2cee.py` (its `spec/tooling.md` §9) starts each one;
it reads this tree and never edits it:

```sh
python3 integrations/cosmoos/c2cee.py analyse kernel/scheduler/semaphore.c \
    --header kernel/include/kernel/semaphore.h --cosmoos ~/dev/CosmoOS   # what the port needs, from clang's AST
python3 integrations/cosmoos/c2cee.py port kernel/ipc/pipe.c \
    --header kernel/include/kernel/pipe.h --out <dir>                    # a draft .cee, its floor and build lines
```

A draft is a start, finished by hand to this page's rules; the `.cee`
files that began as drafts say so in their first lines (`quiesce`, `pipe`
and `fifo`, drafted by c2cee's predecessor, Nova's `c2nova`).

## References (CeeSafe's repository)

- The language: `spec/type-system.md`. §28 is C interoperability (`@export`,
  `@repr(C)`, FFI-safe types), §31 imported C structs (`@header`), §32
  context effects, §33 C state embedded by value, §6 overflow arithmetic.
- `spec/unsafe.md`: what `unsafe` permits and the safety-comment rule.
- The guide's pages: https://vkuttyp.github.io/CeeSafe/guide/kernel (kernel
  code: contexts, locks, C state), .../guide/unsafe-and-c (C interop), and
  .../guide/for-agents (the mistakes agents make most often).
- Port reports from before the fork are Nova's, at its v0.6.0:
  `docs/cosmoos/c9-report.md` (GPT) and `docs/cosmoos/a5-report.md` (CPU time).
