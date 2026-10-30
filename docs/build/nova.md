# Writing kernel code in Nova

Some of this tree is written in Nova, a memory-safe systems language with
C#/TypeScript-like syntax that compiles to C
(https://github.com/vkuttyp/Nova; documentation at
https://vkuttyp.github.io/Nova/). This page tells a contributor, human or
AI agent, how to write it, how it becomes the C the build compiles, and
what the kernel asks of it. The build side (the pin, the CI job) is
[`design.md`, "Nova sources"](design.md#nova-sources).

## The model in one paragraph

A Nova source `NAME.nova` sits beside the `NAME.c` it generates. Both are
committed. The C is what every build compiles, so building CosmoOS never
needs the Nova compiler; **changing the code does**. You edit only the
`.nova` file, run `make nova` with the pinned compiler, and commit both.
The generated C carries a banner, `GENERATED: do not edit`, naming its
source. CI's "Nova sources" job regenerates everything and fails if any
checked-in file differs. A hand edit to generated C, or a `.nova` change
committed without its C, cannot merge.

## Files of a port

| File | Written by | What it is |
|---|---|---|
| `kernel/<area>/NAME.nova` | you | the code |
| `kernel/<area>/NAME.c` | `make nova` | novac's C for it, with the banner; never edit |
| `kernel/include/kernel/NAME.h` | you, or `make nova` | the C interface (below: hand-written or generated) |
| `kernel/include/nova/rt.h`, `kernel/nova/nova_rt_core.c` | `make nova` | Nova's runtime, shared by every port |
| `kernel/nova/nova_panic.c`, `tests/host/nova_panic.c` | hand-written | what a failed Nova check does: a kernel panic naming the Nova line; `abort()` in host tests and fuzzers |
| a line in `scripts/nova-gen.py`'s `PORTS` | you | registers the port |
| `kernel/kernel.mk`, `tests/host/host.mk`, `tests/fuzz/fuzz.mk` | you | build `NAME.c` like any C file, with `nova_rt_core.c` and a `nova_panic.c` |

The current ports: `kernel/block/gpt.nova` (the GPT parser and writer),
`kernel/process/cputime.nova` (CPU time and the CPU-time timers),
`kernel/process/nice.nova` (the rules of nice values),
`kernel/scheduler/fair.nova` (the fair level's weights and virtual time)
and `kernel/nova/kcore_test.nova` (the self-test `nova-kcore`, below).
Read them before writing a new one.

### Nova's kernel library

Nova ships a library for code built against this kernel's C API
(`lib/kernel` in Nova's repository; Nova ADR 0025, API in Nova's
`docs/api` and the guide's kernel page): `KernelHeap`; `SpinLock<T>`,
`IrqSpinLock<T>` and `Mutex<T>`, whose data is reached only through a
guard, and spin locks in `static` items built at compile time with
`SPINLOCK_INIT`; `PreemptGuard`, `IrqGuard` and `PerCpu<T>`; `KRef<T>`
over `struct kobject`; `Quiesce` and `deferDrop`. A port uses it by
setting the last field of its `PORTS` line (`novac --kernel cosmoos`).
`IrqGuard`, `PreemptGuard` and `PerCpu` call primitives the kernel does
not export, so they are for code built into the kernel, not modules.
Since novac 0.3.0 (Nova ADR 0027) a `Mutex` can be a static too: its
`@constant` constructor expands `MUTEX_INIT_AT(p, name)` (`mutex.h`),
given the mutex's own address, which its self-pointing lists need.
`ObjectCache<T>` is a typed slab cache: a static, set up once at
start-up (`setUp`), whose `alloc(value)` returns a `CacheBox<T>` that
drops the value and frees the object when dropped (in v1, `T` holds no C
state). The self-test `nova-kcore` (`kernel/nova/kcore_test.nova`, named
by `kcore_selftest.c`) exercises each type in every debug boot, and checks
that `MUTEX_INIT_AT` leaves a mutex as `mutex_init` does.

## Setting up

Install the pinned compiler, the version in `scripts/nova-gen.py`
(`python3 scripts/nova-gen.py --pinned`); `make nova` refuses any other:

```sh
curl -fsSL https://vkuttyp.github.io/Nova/install.sh | NOVA_VERSION=v0.6.0 sh   # into ~/.nova/bin
export PATH="$HOME/.nova/bin:$PATH"     # or: export NOVAC=~/.nova/bin/novac
```

An AI agent with MCP support should also attach the compiler's MCP server.
It checks code, applies the compiler's own fixes, explains diagnostics and
searches the language specification:

```sh
claude mcp add nova -- novac mcp        # Claude Code; other clients: run `novac mcp` over stdio
```

## The workflow

1. **Write or change `NAME.nova`.** New code starts from the closest
   existing port, which has the right imports, exports and safety comments.
2. **Check, in a loop:**
   `novac check --profile freestanding --no-float kernel/<area>/NAME.nova`.
   With MCP, use `nova_check`. Then `novac fix` (`nova_fix`) applies the
   fixes the compiler is sure of, and `novac explain N0294` (`nova_explain`)
   explains any code. Do not guess at a diagnostic; read its explanation.
3. **Format:** `novac fmt kernel/<area>/NAME.nova`.
4. **New port only:** add its line to `PORTS` in `scripts/nova-gen.py`,
   and its sources to the makefiles, like `cputime.c`'s
   (`kernel/kernel.mk`; a host test in `tests/host/host.mk`; a fuzz target
   in `tests/fuzz/fuzz.mk`).
5. **Generate:** `make nova`, which writes `NAME.c` (and the header, if
   the port has a generated one).
6. **Build and test** as for any C change: the validation matrix in
   `AGENTS.md`, both architectures.
7. **Commit `NAME.nova` and everything `make nova` changed together.**
   `make nova-check` must say the files match.

## The header: two models

**Header first (the default; every current port).** The C header is
hand-written, like any kernel header: it declares the structs, the
constants and the prototypes, with comments, and C callers include it.
The Nova source imports the structs it touches, with the header that
defines them:

```nova
@header("kernel/cputime.h")
extern "C" {
    struct cputime_pair {
        user_ns: u64;
        system_ns: u64;
    }
}
```

It then defines each function under the header's name:

```nova
/// Safety: `ct` points to a live cputime whose lock the caller holds.
@export("cputime_tick")
@context(irq)
unsafe fn tickC(ct: *mut cputime, user: bool, tickNs: u64) -> u32 => tick(Ptr.toBorrowMut(ct), user, tickNs);
```

The generated C `#include`s the header, so the C compiler checks every
export against its prototype. novac also emits static assertions that each
imported field has the type and size the Nova declaration says. A header
change that the Nova side does not follow fails the build. Choose this
model when C code owns the structs, as kernel structs usually are
(embedded in `struct process`, guarded by C locks), or when the header
needs macros or documentation.

**Nova first.** Nova defines the structs (`@repr(C) struct ...`) and the
exports, and novac writes the header. Give the port's `PORTS` line a
fourth element, the header's path (`"kernel/include/kernel/NAME.h"`).
`make nova` then writes novac's interface there: the `@repr(C)` structs
and a prototype per export, with a banner and an include guard from its
name under `kernel/include` (`kernel/NAME.h`, passed as novac's
`--exports-header`). The generated C includes the header by that name
rather than defining the structs again (Nova ADR 0030), so a kernel
header may include it and the port may include that kernel header: the
handle table's `handle_types.h`, which `handle.h` includes, is one. Choose
it for a component whose types Nova owns. The header cannot carry macros
or comments, so a C header that includes it carries them, or C callers
learn the API from the `.nova` file's doc comments.

Either way only these types cross between C and Nova: integers, `bool`,
raw pointers to them or to C-compatible structs, `@repr(C)` structs and
`@repr(uN)` enums (Nova's `type-system.md` §28.3). A slice crosses as a
pointer and a length; references, optionals and Nova strings do not cross.

## Rules for Nova in the kernel

- **Profile.** Always `--profile freestanding --no-float`: no hosted
  library, and no floating point (the kernel saves no FP state for it).
  `make nova` passes both.
- **`unsafe` only at the boundary.** Raw pointers from C become references
  and slices once, in the exported wrapper (`Ptr.toBorrowMut(p)`,
  `SliceMut.fromRaw(p, n)`). Everything below works on references and
  slices, which are bounds- and lifetime-checked. Every `unsafe fn`
  carries a `/// Safety:` comment saying what the C caller guarantees
  (N0283 without one). Say exactly what the header's contract is: who
  holds which lock, which pointers may be NULL.
- **A failed check is a kernel panic.** Nova checks every index, every
  integer overflow, every shift and every division. In the kernel a failed
  check calls `nova_panic`, which panics and names the Nova line. So
  either make the failure impossible or make it explicit:
  - Code that runs in interrupt context, or under a spin lock, must not
    fail on any input. Use `saturatingAdd`/`wrappingSub`/`checkedMul`
    (each says what happens at the limit) where an ordinary `+` could
    overflow, and compare before subtracting. `cputime.nova` is the model.
  - Code that parses untrusted input, such as a disk or a packet, should
    reject a bad input as an error result, not reach a failing check.
    `gpt.nova` is the model: every refusal has a reason.
- **Context effects.** Mark a function that may run in interrupt context
  `@context(irq)`, and one that runs under a spin lock `@context(atomic)`.
  The compiler then refuses anything that could sleep or allocate there
  (N0295). The C caller's context is not visible to Nova, so this
  annotation is the only check.
- **Locks stay in C.** The exported function's caller holds whatever lock
  the header says; the Nova code takes none, signals nothing and allocates
  nothing (no current port does). A signal to send, an object to free, or
  work to queue comes back to the C caller as a result, which acts on it
  after unlocking (`cputime_tick` returns the due signals).
- **Imported C structs never move.** They are C state: borrow them and
  write their fields in place. `let t = c.virt;` is refused (N0294), and
  so is an `if` expression that would pick one of two.
- **No hidden C calls.** Generated C has no compiler-rt libcalls. Nova has
  no 128-bit integer; exact wide arithmetic is written out
  (`cputime.nova`'s `scale`).

## Testing a port

Hold a port to the same tests as C code, and more, since the Nova side is
cheap to drive from the host:

- **A host test** (`tests/host/test_NAME.c`) linking `NAME.c`,
  `kernel/nova/nova_rt_core.c` and `tests/host/nova_panic.c`. A failed
  check aborts, so ASan and UBSan builds catch it.
- **A fuzz target** (`tests/fuzz/fuzz_NAME.c`), for anything that takes
  untrusted input or carries a state machine. Assert the invariants the
  header promises after every step, and supply seeds (`fuzz_seed`).
- **The in-kernel or guest test** for the behaviour, written to fail on
  main first, as for any change (`AGENTS.md`).
- **A port that replaces C** also gets a differential test against the C
  original. Nova's repository runs it for GPT (`integrations/cosmoos/c9.py`).

## Common mistakes

| Mistake | What happens | Instead |
|---|---|---|
| editing the generated `NAME.c` | CI's "Nova sources" job fails | edit `NAME.nova`, `make nova` |
| committing `NAME.nova` without regenerating | the same | `make nova`, commit both |
| a novac other than the pinned one | `make nova` refuses; another version's C differs | install the pinned release; bumping the pin is its own change |
| `+` on a counter in interrupt context | a possible kernel panic on overflow | `saturatingAdd`, or prove the bound |
| a borrow-taking parameter called without `mut` | N0249 | `f(mut x)`: a call that lends mutably says so |
| copying an imported struct out of a field | N0294, N0251 | borrow it; act on each field in place |
| `unsafe fn` without `/// Safety:` | N0283 | state the C caller's obligations |
| a float, or a hosted library call | refused by `--no-float` and the profile | integers; a C function declared in `extern "C"` |

## Upgrading the compiler

Moving to a newer novac is a change of its own. Bump `NOVAC_VERSION` in
`scripts/nova-gen.py` and the CI job's `NOVAC_SHA256` (from the release's
`SHA256SUMS`), run `make nova`, and review the regenerated C. Usually only
version lines and locations change; anything else needs an explanation.
Then run the validation matrix on both architectures. Nova's side of the
checklist (its CI, nightly fuzz campaign and NativeAOT check green for the
release) is in Nova's `docs/release.md`, "Moving CosmoOS to a new release".

## References (Nova's repository)

- The language: `docs/type-system.md`. §28 is C interoperability (`@export`,
  `@repr(C)`, FFI-safe types), §31 imported C structs (`@header`), §32
  context effects, §33 C state embedded by value, §6 overflow arithmetic.
- `docs/unsafe.md`: what `unsafe` permits and the safety-comment rule.
- The guide's pages: https://vkuttyp.github.io/Nova/guide/kernel (kernel
  code: contexts, locks, C state), .../guide/unsafe-and-c (C interop), and
  .../guide/for-agents (the mistakes agents make most often).
- Port reports: `docs/cosmoos/c9-report.md` (GPT) and
  `docs/cosmoos/a5-report.md` (CPU time).
