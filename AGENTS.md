# CosmoOS contributor rules

Read the governing [prompts](prompts/) before acting. Follow the
[development workflow](docs/development.md#workflow-for-a-change), the
[backlog](docs/backlog.md), and the current task prompt.

## Scope rules

The [1.3 roadmap](docs/roadmap-1.3.md) (a release people can install,
adopted 2026-11-11) sets the work. The [1.0](docs/roadmap-1.0.md) and
[1.1](docs/roadmap-1.1.md) roadmaps are complete, as is 1.2's direction B;
[1.2](docs/roadmap-1.2.md)'s direction A and the [2.0 roadmap](docs/roadmap-2.0.md)
are the record of longer work, not scheduled, except that 2.0's CeeSafe
rule stands for new kernel code. These rules take precedence over the
backlog when choosing what to do.

- Work only on a 1.3 milestone, in the roadmap's order: one in the
  release stream (R) and one in the kernel stream (K) at a time, never
  the same subsystem in the same week; T's two units go first. With the
  next milestone unclear, propose; do not start one. A defect
  found along the way is fixed only if it blocks that unit's acceptance
  test or turns main red. Otherwise add one line to
  [docs/backlog.md](docs/backlog.md) (under its subsystem, the next free
  id, a severity and a size) and move on. Do not propose loose ends as
  the next unit. [`docs/plan.md`](docs/plan.md) and the
  [deferred-work inventory](docs/audit/2026-09-deferred-work-inventory.md)
  are the historical record; nothing new is added to them.
- New features are proven by their acceptance test. The probe/`--old`
  convention applies to defect fixes, not to new features.
- Run the full validation matrix once per PR before opening it, and not
  again after documentation-only commits. Exempt from the matrix and from
  the dated report: a documentation-only PR, and a timeout-only PR (a
  boot or self-test budget under `tests/boot/` or a `*_TIMEOUT` variable)
  that changes no other code; it cites the CI data that sets the budget
  instead (as #418 did).
- Exempt from the matrix, not from the dated report: a test-only PR,
  one that changes only an acceptance test's scripts or harness that no
  matrix step runs (such as `tests/busybox/`, copied into the image but
  run only by `make test-busybox`) and host-side `tools/`, and no
  compiled code. Its proof is the probe on both architectures and branch
  CI, which runs the acceptance test it changes (as #479 did). A change
  to the self-tests, anything compiled, or a harness a matrix step runs
  (such as `tests/boot/run_boot_test.py`) is not test-only.
- Every change reaches main through a PR with CI green, conversions to
  CeeSafe included; a conversion PR may hold several ports and changes no
  behaviour.
- At most 5 PRs per unit, except K2, which is bounded by its three weeks
  (to 2026-12-02). If more seem needed, stop and report why.
- A unit is done when its acceptance test is green on main; tick its
  box in [docs/roadmap-1.3.md](docs/roadmap-1.3.md) in the same PR.
- Release from main at least every two weeks, by the rule under
  [Releases](#releases).

## Project conventions

- One unit, one branch from current main, one PR. Record scope and baseline;
  update design docs and invariants before the smallest correct change;
  validate; then update the [backlog](docs/backlog.md),
  [history](docs/history/subsystem-units.md), and a dated audit report using
  the actual work date.
- Prove each defect with a deterministic test failing on main and passing
  with the fix. Add a `tools/<name>-probe.py` whose `--old` mode restores
  the old behavior in a throwaway worktree and demonstrates the failing
  check on both architectures.
- Never weaken assertions or widen budgets to obtain a pass. Budget changes
  require evidence and the keep / restate / widen-and-label rules in
  [flakes.md](docs/testing/flakes.md#the-rule-for-joining-the-list).
- Treat unexplained failures as regressions; record log facts in flakes.md,
  read the harness verdict first, and establish a mechanism before calling
  a failure intermittent or environmental.
- Put instrumentation and test seams under `CONFIG_DEBUG` or
  `CONFIG_SELFTEST`; check release symbols with `llvm-nm` when touching
  shared paths. Unarmed hot-path hooks must take no lock and perform no
  atomic read-modify-write.
- Compare benchmarks with matching lockdep configurations and alternated
  before/after boots; see [Benchmark runs](docs/development.md#benchmark-runs).
  Run QEMU at default priority; avoid zsh background `&` boot loops, and
  inspect QEMU's priority and nice value with `ps`.
- State checkable facts: actual paths, functions, measurements and run IDs.
- The kernel's CeeSafe sources (`NAME.cee`) are committed and the C made from
  them is not: the build runs the pinned ceesafec (install it first) into
  `out/cee/` and compiles from there. Edit the `.cee` file, never the
  generated C; `make cee-check` also runs the negative sources, and `make
  dist` writes a source tarball that builds with no ceesafec. Before writing or changing CeeSafe, read
  [Writing kernel code in CeeSafe](docs/build/ceesafe.md): setup (the pinned
  ceesafec, its MCP server), the workflow, hand-written or generated headers,
  and the kernel's rules (`unsafe` only at the boundary, no failing check
  in interrupt context, `@context(irq)`, locks stay in C).

## Validation matrix

The [device-lifecycle matrix](docs/audit/2026-10-08-vnet-remove-report.md#validation)
requires, on x86-64 and AArch64 (`ARCH=aarch64`):

- `make host-test`, `make fuzz`, `make analyze`.
- Debug `make test` boots at `QEMU_SMP=1`, `2`, and `4`, with the network
  harness passing in every debug boot.
- `make test-smp2`, `make test-chaos`, `make test-harness-retry`.
- Release build and boot (`make BUILD=release test`).
- Branch CI passing on both architectures before completion. After merge,
  check CI on main's merge commit; a red merge run takes priority. A pull
  request skips the two long acceptance tests unless it changes a path they
  exercise; label it `full-ci` when a change could reach the file system,
  storage or upgrade path indirectly
  ([which jobs a pull request runs](docs/build/testing.md#which-jobs-a-pull-request-runs)).

Use GNU make (`gmake` on macOS). Preserve additional required checks from
the [development docs](docs/development.md#workflow-for-a-change) and CI.

## Releases

A release is proven on the owner's Parallels VM before it is tagged
(standing rule since v1.0.10):

1. Merge the release's notes (`docs/releases/vX.Y.Z.md`) and check CI on
   main's merge commit.
2. Push `vX.Y.Z-rc1` on that commit. The release workflow builds and signs
   it as a GitHub pre-release; the release key is a CI secret, so a
   locally built set is refused by an installed system and cannot stand in.
3. Upgrade the Parallels VM to the rc with its own `cosmo-upgrade` (the
   rc's set fetched or served from the Mac), reboot, and check it: the
   boot's lines, `cosmo-upgrade --status`, `pkg update && pkg upgrade`, the
   console's `dmesg`, and what the release adds.
4. Only then push `vX.Y.Z` on the same commit, and upgrade the VM from the
   rc to the release (`cosmo-upgrade --fetch vX.Y.Z`).

A defect found on the rc is fixed through the ordinary workflow and a new
rc (`-rc2`) on the fixed merge commit; the release is never tagged on a
commit the VM did not run.
