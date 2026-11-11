# K1, second PR: ceesafec 0.37.0 and the native call table in CeeSafe, 2026-11-12

After K1's first PR (#512) `kernel/syscall/native.c` held only what CeeSafe could not write as C declares it: the
system-call table, the list of calls a filter may never deny, and the personality record. ceesafec 0.33.0 (pinned by
#511) writes the table and the record as C's `const` objects. A draft with it showed the list, a plain `[u16; 4]`,
emitted writable, where C's is `static const`; CeeSafe's ADR 0098, released as ceesafec 0.37.0 on 2026-11-11, emits
a plain static of scalar data `const`. This PR moves the pin to 0.37.0 and replaces `native.c` with `native.cee`.

## Scope and baseline

Branch `k1-native-table` from main at `9cfe6183` (#512). Two commits: the pin, then the port.

## Audit (main at `9cfe6183`, read, not copied)

| Item | Where | What it means |
|---|---|---|
| `native_table`: `static const syscall_fn [SYS_COUNT]`, 113 entries by designated index | `kernel/syscall/native.c` | an array of `extern "C" fn` values by index (CeeSafe ADR 0091 §2.3) |
| `native_always_allowed`: `static const uint16_t [4]` | the same | read-only in C; 0.33.0 emitted a `[u16; 4]` static writable |
| `personality_native`: `const struct personality`, eight members set | the same, `kernel/process.h` | an exported static of the imported struct |
| The `SYS_*` numbers are macros | `uapi/cosmo/syscall.h` | each is declared `const SYS_x: u16 = n;` in the port, and the C compiler refuses a value the header does not have |

## What changed

- **The pin, ceesafec 0.37.0** (`scripts/cee-gen.py`, `ci/Dockerfile`, `ci.yml`, the install lines, the roadmap's
  line). No source needed a change for it; `make cee-check` reports the generated files matching their sources.
- **`kernel/syscall/native.cee`** (409 lines) replaces `native.c` (224): the 113 numbers, the 113 calls and the
  three functions the record names, declared from `kernel/syscall.h`, `kernel/process.h` and
  `uapi/cosmo/syscall.h`; `TABLE`, by index; `ALWAYS_ALLOWED`, with the C file's reasons as its comment; and
  `personality_native`. `kernel/kernel.mk` is unchanged: the generated C has the old file's name.
- In the release objects of both architectures `llvm-nm` shows the table and the list as `r` and
  `personality_native` as `R`: read-only data, as before.

**One difference from the C.** The record's name, `"native"`, is emitted as `static char ceeSC_0[]`, in writable
data (`d`), where C's string literal was read-only. The record itself, which holds the pointer, is read-only.
Backlog SEC-15.

## Validation

Local, macOS host, at `da65326b`. No step needed a rerun. The matrix ran with a ceesafec 0.37.0 built on this
machine from the release commit's tree (CeeSafe `e1ed24a`), because the release's binaries were not yet published
when it started; branch CI uses the published Linux binary.

| Step | x86-64 | AArch64 |
|---|---|---|
| debug and release build, `make cee-check` | exit 0, no warning | exit 0, no warning |
| `make host-test`, `make fuzz` | pass | pass |
| `make analyze` | clean (14 baselined, 0 unexpected, 0 stale) | clean (14 baselined, 0 unexpected, 0 stale) |
| debug `make test`, `QEMU_SMP=1` | PASS in 175.4 s | PASS in 179.8 s |
| debug `make test`, `QEMU_SMP=2` | PASS in 186.6 s | PASS in 201.4 s |
| debug `make test`, `QEMU_SMP=4` | PASS in 183.9 s | PASS in 195.8 s |
| `make test-smp2` | PASS in 182.0 s | PASS in 207.0 s |
| `make test-chaos` | PASS in 190.3 s | PASS in 205.4 s |
| `make test-harness-retry` | PASS in 201.8 s | PASS in 202.1 s |
| `make BUILD=release test` | PASS in 19.3 s | PASS in 21.3 s |

## What this does not do

- K1's acceptance box stays open: `compat/linux/syscalls.c` (4,223 lines) and the Linux table and record.
- Documents that name `kernel/syscall/native.c` for calls that now live in this directory's CeeSafe files are not
  swept here: backlog DOC-4.
