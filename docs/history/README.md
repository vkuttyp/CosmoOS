# Project history

What has been built, in the order it merged. Each entry records what a
phase, milestone or unit delivered, how it was tested, and what it left
open at the time. Entries are history: they describe the tree when they
merged, and later work may have changed what they say. For what is open
now, read [`docs/plan.md`](../plan.md) and the
[deferred-work inventory](../audit/2026-09-deferred-work-inventory.md).

| File | Era |
|---|---|
| [`roadmap.md`](roadmap.md) | The constitution's numbered roadmap, Phases 0-13: boot, memory, SMP, objects, modules, devices, VFS and storage, networking, userland, packages, Linux stage 1, virtualization stage 1, AArch64 |
| [`post-roadmap-audit.md`](post-roadmap-audit.md) | The post-roadmap audit's critical-fix pass and ten milestones, then the storage, container, hardware and guest-virtualization units that followed |
| [`subsystem-units.md`](subsystem-units.md) | Units chosen from the deferred-work inventory through section 68 reports (`docs/audit/next-subsystem-*.md`), and the lockdep milestone (PRs #302-#308) |

## Where things stand

The roadmap's numbered phases and the post-roadmap audit's own list are
complete, apart from pid renumbering, which the process domain
deliberately does without (`docs/kernel/security/design.md`, "This is not
a pid namespace"). Every section 68 report under
`docs/audit/next-subsystem-*.md` has been built and has its entry in
`subsystem-units.md`. What remains is grouped in
[`docs/plan.md`](../plan.md); the next report is chosen from it and the
inventory, and names the entry it closes. Section 68 is not a list of
deferrals: it is the instruction to stop after the audit, name one
subsystem in a fixed shape and wait. Design documents first, one
subsystem at a time.

## Adding an entry

Every merged unit adds one entry at the end of `subsystem-units.md` in the
same commit as its documentation: a bold title, what was built and why,
how it was proved (tests, bug-proofs, measurements), what it deliberately
left open, and the PR number in parentheses. Describe the merged diff, not
the plan.
