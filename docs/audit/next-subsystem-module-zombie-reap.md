# NEXT SUBSYSTEM — a zombie nobody will come back for

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This report is a design, not
an as-built.

**Subsystem: what module teardown leaves behind, and the request that
would collect it but nobody makes.** A module whose objects outlive its
unload becomes a *zombie*: `module_unload` returns `-EBUSY`, the module
is pushed onto `g_zombies`, and — in the words of its own comment —
"the memory stays … a later unload of the name reaps it once the count
reaches zero. The dependencies stay pinned too" (`module.c:518-528`).

That later unload is the only reaper there is. Nothing calls it, and in
two ordinary cases nothing *can*.

## What a zombie holds

Not a `struct module`. The whole thing:

- **Its image** — text, rodata and data stay mapped, because a release
  callback still to run lives in that text.
- **Its dependency pins** — `drop_deps` runs at the free, not at the
  unload, so every module it depends on keeps a reference. **A stuck
  zombie permanently blocks unloading its dependencies**, and that is
  the part that compounds: one module nobody thinks about pins a chain
  of others that cannot then be unloaded either.

## Why the reaper does not run

`module_unload(name)` is the only path that frees a zombie
(`module.c:467-486`), and reaching the zombie requires the name to
resolve to *no live module*:

1. **Nothing calls it.** There is no periodic sweep and no reap at load
   or unload of anything else. Freeing a zombie requires someone to
   unload a name that is not loaded — a request with no reason to be
   made, by anyone who does not already know a zombie is there.
2. **A reused name hides it.** `module_unload` calls `find_locked(name)`
   first. Load a replacement under the same name and the unload targets
   the replacement; the zombie is never looked for.
3. **A second zombie of the same name is unreachable.**
   `find_zombie_locked` returns the **first** match on the list
   (`module.c:457-465`). Two zombies of one name, and the later one can
   never be found by any call — not "unlikely to be reaped", *cannot
   be*.

The inventory records this as "zombie modules are reaped only by a later
`module_unload` of the same name", which is accurate and reads as an
inconvenience. Cases 2 and 3 are not inconveniences; they are
unreachable memory with pins attached.

## Why it survived

**Because the happy path is tested.** `selftest_module_unload_busy`
(`modtest.c:396-455`) builds a zombie, proves the release runs from the
zombie's own text, proves a second unload frees it, and proves a zombie
keeps its dependencies pinned. It is a good test of the mechanism, and
everything it asserts is true.

What it does not ask is whether anyone ever makes that second call. The
mechanism works; the policy is that there isn't one. That is a shape
this tree has hit before — a rule stated in one place, enforced nowhere
— with the twist that here the rule is stated *in a test that passes*.

## The second item, and it is not connected

`MODULE_MAX_LIVE` is a fixed 32 and exhausting it **panics**:

```c
    if (!published)
        panic("module: more than %u modules live", MODULE_MAX_LIVE);
```

The load path has an errno and uses it everywhere else; this one case
kills the machine instead. Thirteen modules load at boot, so 32 is not
close — but a limit whose overflow is a panic is a different kind of
limit from one whose overflow is `-ENOSPC`, and the difference costs
nothing to fix.

**It is *not* a consequence of un-reaped zombies, and this report checked
rather than assumed.** `unpublish(m)` runs at step 1 of the unload
(`module.c:498`), before the zombie is created, so a zombie holds no
`g_live[]` slot and cannot exhaust the array. The two items share a
subsystem and a flavour — resources released only by a request that may
never come — and nothing else. Recording the non-connection because the
first draft of this report assumed one.

## Design

**A zombie is identified by what it is, not by what it is called, and is
collected without being asked for.**

1. **Sweep at every load and unload.** Both already hold `g_lock` and
   both already walk module state; a pass over `g_zombies` freeing every
   entry whose `live_objects` has reached zero costs one list walk on a
   path that is not hot. This closes case 1 without a timer, a thread or
   a policy knob.
2. **Reap by identity.** The name lookup stays for the explicit
   `module_unload("name")` call, because that is a real request and
   should keep working. But the sweep does not use names, so cases 2 and
   3 stop existing: a second zombie of the same name is just another
   list entry.
3. **`-ENOSPC`, not `panic`.** The slot array returns an error the
   loader already knows how to report. Whether 32 should be larger is a
   separate question this report does not answer — the defect is the
   panic, not the number.
4. **Say what a zombie costs, where a zombie is made.** The `kwarn` at
   `module.c:524` names the module and the live count. It should also
   say what is being held: the image, and which dependencies stay
   pinned. A warning that does not say what it is costing is read once
   and forgotten.

## Affected files

| file | change |
| --- | --- |
| `kernel/module/module.c` | the sweep, called from load and unload; reap by identity; `-ENOSPC` in place of the panic; the warning says what is held |
| `kernel/module/modtest.c` | the tests in the table below |
| `kernel/include/kernel/selftest.h`, `kernel/core/selftest.c` | their declarations and registry entries |
| `docs/kernel/modules/` (design and invariants) | when a zombie is collected, and that it is by identity |
| `docs/audit/2026-09-deferred-work-inventory.md` | §4's last two small debts struck |
| `README.md` | the Status entry |

## Tests

| test | asserts |
| --- | --- |
| `module-zombie-swept` | a zombie whose objects die is freed by the **next unrelated load or unload**, with no unload of its own name |
| `module-zombie-name-reused` | a replacement loaded under the zombie's name does not hide it: the sweep still collects it, and the replacement is untouched |
| `module-zombie-two-of-a-name` | two zombies sharing a name are **both** collected — the case `find_zombie_locked` cannot reach today |
| `module-zombie-holds-deps` | unchanged in substance from what `selftest_module_unload_busy` proves, plus: once swept, the dependency pin is **released**, which is the consequence that matters |
| `module-slots-enospc` | exhausting `MODULE_MAX_LIVE` returns `-ENOSPC` and the machine lives |

**The bug-proof.** `module-zombie-two-of-a-name` must fail against the
current tree for the stated reason — the second zombie is unreachable by
name — and not merely because no sweep exists. A test that fails for
"nothing collected it" would pass the moment a sweep is added even if
the sweep still keyed on names, which is the half-fix this design is
most likely to receive.

**Watch the existing test.** `selftest_module_unload_busy` asserts a
second unload frees the zombie. A sweep at every load and unload may
free it *first*, so that test's second unload could start returning
`-ENOENT` instead of `0`. Whether that is a break or the new truth is a
decision the build must make deliberately, and record — this report's
position is that the explicit call should keep succeeding when there is
something to free and that the test should say which reaper ran.

## Risks

- **Sweeping at load/unload adds a list walk under `g_lock`.** Bounded
  by the zombie count, which is normally zero and pathologically small;
  neither path is hot.
- **Freeing earlier changes when release code runs.** It does not: the
  sweep frees only zombies whose `live_objects` is already zero, which
  is exactly the condition the name-based reap uses today.
- **The existing zombie test may need to change**, as above. A unit that
  quietly rewrites a passing test's expectation is worse than one that
  says it is doing so.

## Alternatives considered

- **A periodic reaper thread.** Collects case 1 and needs a thread, a
  period and a reason to prefer them over a walk on a cold path.
- **Reap only at load.** Cheaper and misses the case where the last
  module load has already happened and unloads continue.
- **Leave it and document the limitation.** The documentation would have
  to say "a module can pin its dependencies forever if nobody issues a
  request they have no reason to issue", which reads as a bug report
  rather than a design note.
