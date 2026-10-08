# CosmoOS contributor rules

Read the governing [prompts](prompts/) before acting. Follow the
[development workflow](docs/development.md#workflow-for-a-change), the
[remaining-work plan](docs/plan.md), and the current task prompt.

## Project conventions

- One unit, one branch from current main, one PR. Record scope and baseline;
  update design docs and invariants before the smallest correct change;
  validate; then update the plan, [inventory](docs/audit/2026-09-deferred-work-inventory.md),
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

## Validation matrix

The [device-lifecycle matrix](docs/audit/2026-10-08-vnet-remove-report.md#validation)
requires, on x86-64 and AArch64 (`ARCH=aarch64`):

- `make host-test`, `make fuzz`, `make analyze`.
- Debug `make test` boots at `QEMU_SMP=1`, `2`, and `4`, with the network
  harness passing in every debug boot.
- `make test-smp2`, `make test-chaos`, `make test-harness-retry`.
- Release build and boot (`make BUILD=release test`).
- Branch CI passing on both architectures before completion. After merge,
  check CI on main's merge commit; a red merge run takes priority.

Use GNU make (`gmake` on macOS). Preserve additional required checks from
the [development docs](docs/development.md#workflow-for-a-change) and CI.
