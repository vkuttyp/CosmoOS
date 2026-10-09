# Roadmap M1: sound randomness — 2026-10-09

Milestone M1 of the [1.0 roadmap](../roadmap-1.0.md). Base: main
`8d16491d` (PR #339, the roadmap, merged; its merge run was in progress
when this branch was cut). Design: `docs/kernel/security/design.md` §6;
invariants S16-S19.

## Baseline (main `5e54c319` / `8d16491d`)

- `random_init` mixed `clock_now_ns()` and a stack address, uncredited.
- `random_add_entropy` hashed every input straight into the output state
  and credited `g_entropy_bits`; `random_get_bytes` never consulted it.
- virtio-rng was the only credited source, at 8 bits per byte; the
  `random` self-test credited 8 bits of constant input.
- `lx_getrandom` ignored its flags. cosmofs drew its master key, salts and
  block nonces with no check. Nothing logged seeding.
- Default QEMU CPU models (`qemu64,+nx,+svm,+npt`, `cortex-a72`) have no
  random instruction, so the ordinary boots are device-seeded only.

## What changed

| Area | Change | Files |
|---|---|---|
| Construction | input pool and output key separated (S16); reseed at ≥256 credited bits; first reseed = seeded (sticky); ratchet per request; reseed every ≥60 s with a fresh CPU sample | `kernel/core/random.c` |
| Sources | RDSEED 32 / RDRAND 4 / RNDRRS 32 / RNDR 4 bits per successful 64-bit read, success from CF / NZCV, 10 retries, failures never mixed (S17); virtio-rng 4 bits per byte; self-test input 0 | `kernel/include/arch/rng.h`, `kernel/arch/{x86_64,aarch64}/rng.c`, `drivers/virtio/virtio_rng.c`, `kernel/device/devtest.c` |
| API | `random_ready`, `random_wait_ready` (killable, bounded or not), `random_boot_wait`, `RANDOM_KEYGEN_WAIT_NS`; none exported, module ABI unchanged | `kernel/include/kernel/random.h` |
| Boot | `random_boot_wait()` after `module_load_boot`, before the self-tests and init: 5 s, one WARN (S19) | `kernel/core/main.c` |
| Linux | `getrandom`: unknown bit or `INSECURE|RANDOM` `-EINVAL`; unseeded default/`RANDOM` blocks (killable), `NONBLOCK` `-EAGAIN`, `INSECURE` never waits | `compat/linux/syscalls.c`, `linux_abi.h` |
| cosmofs | `cfs_need_seeded`: encrypted format and key load wait 5 s then `-EAGAIN`, before anything is written; a firmware-key mount then stays locked (S18) | `cosmofs_crypt.c`, `cosmofs_core.c` |
| Tests | `random-seed` self-test; `cosmofs-crypt` unseeded branch; `lxtest` flag checks; harness `QEMU_RNG`/`QEMU_HWRNG` required/forbidden lines; `make test-entropy`; CI step; fuzz shim `random_wait_ready` | `kernel/`, `tests/`, `Makefile`, `scripts/qemu-run.sh`, `.github/workflows/ci.yml` |

Caller classification (must-be-seeded / may-be-early, with reasons) is
the table in design.md §6. `AT_RANDOM` is must-be-seeded by purpose but
exec does not wait (that would change `execve`'s Linux behaviour); the
boot wait makes it seeded in every configuration that has a source.

## Acceptance results (local, macOS host, QEMU 11.1.1, TCG)

| Boot | x86-64 | AArch64 |
|---|---|---|
| ordinary `test` (virtio-rng, no CPU source) | PASS 158.0 s; `pool seeded after 396 ms: 256 bits (cpu 0, devices 256)` | PASS 157.1 s; seeded after 280 ms (cpu 0, devices 256) |
| `test-entropy` no source (`QEMU_RNG=0`) | PASS 160.9 s; `cpu source: none`, WARN, `random-seed: unseeded`, `cosmofs-crypt: unseeded ... -EAGAIN after 5005 ms, device untouched`, `lxtest: getrandom: unseeded` | PASS 156.0 s; same lines, refusal after 5004 ms |
| `test-entropy` CPU only (`qemu64,+rdrand,+rdseed` / `neoverse-v1`) | PASS 158.3 s; `cpu source: rdseed rdrand`, seeded after 1 ms (cpu 512, devices 0), `lxtest: getrandom: seeded` | PASS 147.9 s; `cpu source: rndrrs rndr`, seeded after 0 ms (cpu 512, devices 0) |

The no-entropy configuration was produced on both architectures by
removing the virtio-rng; the default CPU models already lack the
instructions, which the required `random: cpu source: none` line checks.

Probe convention: M1 is a new feature, proved by its acceptance test
(`AGENTS.md` scope rules). The harness lines it requires (`random: cpu
source`, the seeded line or the WARN, `random-seed`, `lxtest: getrandom`)
are strings the old tree does not contain (`git grep` on `8d16491d`), so
no configuration of the old tree can pass `test-entropy`; that was not
run as a boot.

## Validation matrix (local)

| Item | x86-64 | AArch64 |
|---|---|---|
| `host-test` | pass | pass |
| `fuzz` (50000) | pass (after the shim gained `random_wait_ready`) | pass |
| `analyze` | pass | pass |
| debug `test`, `QEMU_SMP=1` | PASS 140.7 s | PASS 133.9 s |
| debug `test`, `QEMU_SMP=2` (`test-smp2`) | PASS 152.1 s | PASS 143.8 s |
| debug `test`, `QEMU_SMP=4` | PASS 158.0 s | PASS 157.1 s |
| `test-chaos` | PASS 149.0 s | PASS 145.7 s |
| `test-harness-retry` | PASS 153.5 s | PASS 160.6 s |
| `BUILD=release test` | PASS 16.7 s | PASS 20.1 s |
| `test-entropy` | PASS (both boots) | PASS (both boots) |

The network harness passes in every debug boot (it is part of the boot
verdict). A further ten local AArch64 `test-smp2` boots: nine passed, one
failed `quiesce-kick-spinner` (flakes.md, 2026-10-09).

## CI

- PR #339's merge run 37981337663 failed AArch64 `test-smp2` with a hang
  in `el2-guest-hvc` (docs-only merge; flakes.md entry); the failed job's
  rerun passed.
- This PR's first run (37984006846): every boot passed on both
  architectures, including both entropy boots, but the AArch64 job
  reached the workflow's 40-minute limit and was cancelled in the
  panic-path step. The entropy boots moved to their own CI job
  (`entropy`), rather than widening the limit.

## Recorded, not fixed (inventory §2.9)

- No SP 800-90B health tests on the sources.
- The TCP SYN-cookie secret is drawn at `tcp_init`, before a device-only
  pool seeds (may-be-early; rekey at seeding not done).
- No timing-jitter source: a machine with neither instruction nor device
  never seeds.
