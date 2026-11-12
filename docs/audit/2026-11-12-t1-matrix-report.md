# T1: `make matrix`, 2026-11-12

Roadmap 1.3's T1: the validation matrix as one target, in an order that fails soonest, with one verdict per step.
This PR adds a script, a target and documents; it changes no compiled code.

## Scope and baseline

Branch `t1-matrix` from main at `6588f973` (#515).

## Why the order

- 2026-11-11: `make analyze` on both architectures raised the Mac's load average to about 100, and boots started
  straight after it failed their timing bounds (`process-user` at 22.9 s against 15 s).
- 2026-11-12, K1's third PR: `make analyze` failed after the boots had run, and the boots were run again.
- 2026-11-12, T2's report: neither KVM nor HVF boots the kernel today, so the boots stay on TCG. A second machine
  running one architecture is the saving that needs no kernel change.

## What changed

- **`scripts/matrix.sh`**, run by **`make matrix`**. Per architecture: the debug and release builds, `make
  cee-check`, `make host-test`, `make fuzz`, `make analyze`. If one fails, the boots are not run. Otherwise the
  debug boots at one, two and four CPUs, `test-smp2`, `test-chaos`, `test-harness-retry` and the release boot,
  each started when the one-minute load average is under the CPU count (`MATRIX_LOAD`), after at most half an
  hour's wait. Each step prints `PASS` or `FAIL` with the harness's verdict line; the last line is `matrix: PASS`
  or `matrix: FAIL`, and the exit status follows it. Output is kept under `out/matrix/`.
- **`MATRIX_REMOTE`**: an ssh destination that runs the x86-64 half while this machine runs AArch64. The tree is
  copied there, the run is started detached through that machine's `run.sh` (a container of the CI image with
  that machine's limits), and its output is fetched every half minute once the local half is done.
- **`docs/development.md`**, "Running the matrix".

## Defects found in the script while proving it

| Run | What happened | Fix |
|---|---|---|
| first | The load wait's counter was the boots' loop variable: the three debug boots ran with `QEMU_SMP` 5, 4 and 0 on the Mac and 0 three times on the other machine | the counter has its own name |
| first | The ssh connection that carried the remote run timed out; the Mac waited 112 minutes for a run that had ended | the remote run is detached and polled |
| second | The connection dropped again ("Can't assign requested address"); the remote verdicts were lost | the same |

## Validation

The other machine: Xeon E3-1245 v2, 8 threads, Debian 13, the CI image built there from `ci/Dockerfile`, each run in
one container limited to 4 CPUs and 6 GiB. Third run, 2026-11-12, 15:33:24 to 16:10:24: 37 minutes, `matrix: PASS`.
The Mac alone takes about 90 minutes for both architectures.

| Step | AArch64 (Mac) | x86-64 (other machine) |
|---|---|---|
| debug and release build, `make cee-check` | PASS | PASS |
| `make host-test`, `make fuzz` | PASS | PASS |
| `make analyze` | clean (14 baselined, 0 unexpected, 0 stale) | clean (15 baselined, 0 unexpected, 0 stale) |
| debug `make test`, `QEMU_SMP=1` | PASS in 178.5 s | PASS in 254.7 s |
| debug `make test`, `QEMU_SMP=2` | PASS in 199.0 s | PASS in 267.7 s |
| debug `make test`, `QEMU_SMP=4` | PASS in 204.7 s | PASS in 267.5 s |
| `make test-smp2` | PASS in 183.8 s | PASS in 259.9 s |
| `make test-chaos` | PASS in 203.6 s | PASS in 262.1 s |
| `make test-harness-retry` | PASS in 208.0 s | PASS in 269.7 s |
| `make BUILD=release test` | PASS in 21.3 s | PASS in 19.5 s |

`MATRIX_BOOTS=0 MATRIX_REMOTE=…` also ran to `matrix: PASS`.

## Limits

- The other machine is slow: its debug boots take 255 s to 270 s against the 300 s budget, and `process-user`
  took 11.0 s there against its 15 s bound. A timing failure there is to be rerun on the Mac before it is believed;
  no budget is widened for it.
- The x86-64 analysis baseline counts 15 diagnostics under the CI image's clang and 14 under the Mac's.
- The script does not run the acceptance tests (`make test-busybox` and the like).
- The stop-before-boots path was exercised only with a make that always fails (`MAKE=false MATRIX_ARCHES=aarch64
  sh scripts/matrix.sh`): six `FAIL` lines, "the boots were not run", `matrix: FAIL`, exit status 1.
