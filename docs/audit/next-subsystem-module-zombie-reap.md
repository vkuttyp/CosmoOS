# NEXT SUBSYSTEM — a zombie nobody will come back for

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. **This report is as built**
(PR #186), and the banner below records where the build differed from
it — including **one** test it named and did not produce
(`module-zombie-holds-deps`, item 4) and **one** it argued against and
then built (`module-slots-enospc`, item 2). An earlier version of this
line said two were not produced, which stopped being true when the
argument in item 2 was overturned.

**What the build changed, each found by building rather than reading:**

1. **In `module_unload` the sweep runs after the name is resolved, not
   before, and that is what keeps the existing test true.** The report
   flagged that a sweep might free a zombie before
   `selftest_module_unload_busy`'s second unload reached it, turning
   that test's `0` into `-ENOENT`, and said the build must decide
   deliberately. The decision is **ordering, not a single placement**:
   the named lookup goes first on every path, so an explicit
   `module_unload("name")` still finds its zombie and returns 0. That
   test is unchanged and still passes. Item 3 below is the same rule
   applied to the exits the first implementation missed.
2. **`module-slots-enospc` IS built, after an argument against it that
   did not hold.** This banner first said it could not be: reaching
   `MODULE_MAX_LIVE` needs thirty-two distinct modules, the archive has
   three fixtures, and "a hook that changes the bound is not obviously
   testing the same code". Review pushed back, and it was right — a cap
   on the publish *search* exercises the same search, the same
   `-ENOSPC`, and the same pre-commit ordering, with only the number
   different. `module_set_max_live_for_test` (debug only) caps it to
   one, and the test asserts the errno, that nothing was published, and
   — the part that matters — that **a subsequent load still succeeds**,
   which a botched unwind would break. The `KASSERT` alone covered only
   the success path, which was the weakness in the original argument.
3. **The sweep had to run on every exit from `module_unload`, not just
   the successful one.** The first implementation swept at the end of
   the success path only, and review found the hole: a zombie that pins
   a dependency makes `module_unload("that dependency")` return
   `-EBUSY` from an early return that never reached the sweep — so the
   one action a user would take on discovering a pinned dependency did
   nothing about it, which is the exact scenario this unit exists to
   fix. The sweep now runs once `m` is known LIVE (where it cannot
   touch it), on the `-ENOENT` path, and — found while checking the
   claim rather than the anchor — on the two named-zombie exits as
   well: the `-EBUSY` for a named zombie that is still busy (it is not
   collectable, so the sweep cannot steal it) and the `0` after a named
   zombie is freed (it is already off the list). "Every exit" now means
   every exit.
4. **`module-zombie-holds-deps` was not built**, because
   `selftest_module_unload_busy` already proves a zombie keeps its
   dependency pinned. The report wanted the *converse* — that a swept
   zombie releases the pin — which `module-zombie-swept` establishes
   implicitly: the sweep calls `drop_deps` on the same path as the named
   reap, and a leaked pin would fail the later clean unload in that
   test. A dedicated test would assert the same code twice.
5. **The forward declaration.** `module_load` is defined above the
   zombie helpers, so the sweep needed one — trivial, and the kind of
   thing a design does not know.
6. **The tests' short unload timeout is now scoped to one call.** These
   tests shorten a *global* to 50 ms so an unload gives up quickly.
   `CHECK` returns immediately, so a test that set the global, checked
   and restored afterwards left 50 ms behind on the failing path, and
   the next test to unload anything then failed for a reason that was
   not its own. Review found one site (`module-zombie-two-of-a-name`)
   running four checks inside that window. Rather than add a cleanup
   path per test, `unload_with_timeout()` sets, unloads and restores in
   one call, so the global is short for the duration of an unload and
   never across an assertion — and `MODULE_UNLOAD_TIMEOUT_MS_DEFAULT`
   names the value being restored, which was a literal `5000` copied
   into five places.

**Subsystem: what module teardown left behind, and the request that
would have collected it but nobody made.** A module whose objects
outlive its unload becomes a *zombie*: `module_unload` returns
`-EBUSY` and the module is pushed onto `g_zombies`. In the words the
code carried before this unit: "the memory stays … a later unload of
the name reaps it once the count reaches zero. The dependencies stay
pinned too" (`module.c:518-528`, pre-build).

That later unload **was the only reaper there was**. Nothing called it,
and in two ordinary cases nothing *could*. Everything from here to the
Design section describes the tree as it stood before PR #186; the
sweep described in the Design section is what it does now.

## What a zombie holds

Not a `struct module`. The whole thing:

- **Its image** — text, rodata and data stay mapped, because a release
  callback still to run lives in that text.
- **Its dependency pins** — `drop_deps` runs at the free, not at the
  unload, so every module it depends on keeps a reference. **A zombie
  that was never collected therefore blocked unloading its dependencies
  for good**, and that was the part that compounded: one module nobody
  thought about pinned a chain of others that could not then be
  unloaded either. A zombie still holds all of this — what changed is
  that it no longer holds it indefinitely.

## Why the reaper did not run

`module_unload(name)` was the only path that freed a zombie
(`module.c:467-486`), and reaching the zombie required the name to
resolve to *no live module*:

1. **Nothing called it.** There was no periodic sweep and no reap at
   load or unload of anything else. Freeing a zombie required someone
   to unload a name that was not loaded — a request with no reason to
   be made, by anyone who did not already know a zombie was there.
2. **A reused name hid it.** `module_unload` calls `find_locked(name)`
   first. Load a replacement under the same name and the unload
   targeted the replacement; the zombie was never looked for.
3. **N zombies of one name needed N of those calls.**
   `find_zombie_locked` returns the **first** match
   (`module.c:457-465`), and the reap `list_remove`s it
   (`module.c:482`), so a later call did reach the next one.

   **An earlier draft of this report said the second one was
   unreachable by any call. That was wrong** — it read the first-match
   lookup and did not check that the reap removes what it found. The
   true statement is weaker and still bad: each zombie needs its own
   call, and case 1 is that nobody makes even the first.

The inventory recorded this as "zombie modules are reaped only by a
later `module_unload` of the same name", which was accurate and read as
an inconvenience. It was worse than that — a reused name hid a zombie
entirely, and the pins it held lasted as long as it went unreaped — but
the memory was reachable, and this report says so because its first
draft did not.

## Why it survived

**Because the happy path is tested.** `selftest_module_unload_busy`
(`modtest.c:396-455`) builds a zombie, proves the release runs from the
zombie's own text, proves a second unload frees it, and proves a zombie
keeps its dependencies pinned. It is a good test of the mechanism, and
everything it asserts is true.

What it did not ask is whether anyone ever makes that second call. The
mechanism worked; the policy was that there wasn't one. That is a shape
this tree has hit before — a rule stated in one place, enforced nowhere
— with the twist that here the rule was stated *in a test that passes*.

## The second item, and it is not connected

`MODULE_MAX_LIVE` is a fixed 32, and exhausting it used to **panic**:

```c
    if (!published)
        panic("module: more than %u modules live", MODULE_MAX_LIVE);
```

The load path has an errno and used it everywhere else; this one case
killed the machine instead. Thirteen modules load at boot, so 32 was
not close — but a limit whose overflow is a panic is a different kind
of limit from one whose overflow is `-ENOSPC`, and the difference cost
nothing to fix.

**It was *not* a consequence of un-reaped zombies, and this report
checked rather than assumed.** `unpublish(m)` runs at step 1 of the
unload (`module.c:498`), before the zombie is created, so a zombie
holds no `g_live[]` slot and cannot exhaust the array. The two items
share a
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

   **Two constraints the sweep must honour, stated here because a sweep
   is easy to write wrongly.** It frees list entries as it walks, so it
   needs **removal-safe iteration** — a plain `list_for_each_entry`
   advances through the node it has just freed. And it must load
   `live_objects` with **acquire** ordering, as the existing wait does
   (`module.c:517`): a relaxed zero could unmap module text without
   synchronising against the final object release, which is a
   use-after-free in the release code's own text.
2. **Reap by identity.** The name lookup stays for the explicit
   `module_unload("name")` call, because that is a real request and
   should keep working. But the sweep does not use names, so cases 2 and
   3 stop existing: a second zombie of the same name is just another
   list entry.
3. **`-ENOSPC`, not `panic` — and the slot is reserved *before* the
   module is committed.** Returning an error at the current panic site
   would not be enough, and this report's first draft said only
   "return an error". By the time that loop runs, `m->info->init()` has
   **already executed**, `m->state` is `MODULE_LIVE`, the module is
   linked into `g_modules` and `g_count` is incremented
   (`module.c:384-402`). Failing there without unwinding would leave an
   initialised, linked, counted module with no slot — a worse state
   than the panic it replaces.

   So the slot is claimed **before** `init()` runs: find a free index,
   fail with `-ENOSPC` while failing is still free, and publish into the
   reserved index afterwards. That keeps the existing failure path
   correct, because nothing has been committed when it is taken.
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
| `docs/kernel/module/design.md`, `docs/kernel/module/invariants.md` | when a zombie is collected and that it is by identity; **M24** (as built — the report wrote `docs/kernel/modules/`, which does not exist) |
| `docs/audit/2026-09-deferred-work-inventory.md` | §4's last two small debts struck |
| `README.md` | the Status entry |

## Tests

| test | asserts |
| --- | --- |
| `module-zombie-swept` | a zombie whose objects die is freed by the **next unrelated load**, with no unload of its own name |
| `module-zombie-name-reused` | a replacement loaded under the zombie's name does not hide it: the sweep still collects it, and the replacement is untouched |
| `module-zombie-two-of-a-name` | two zombies sharing a name are **both** collected by one sweep — today each needs its own `module_unload` call, and nothing makes any of them |
| ~~`module-zombie-holds-deps`~~ | **NOT BUILT** (item 4): unchanged in substance from what `selftest_module_unload_busy` proves, plus: once swept, the dependency pin is **released**, which is the consequence that matters |
| `module-slots-enospc` | exhausting the publish-slot search returns `-ENOSPC`, publishes nothing, and leaves the loader usable. As built it uses `module_set_max_live_for_test` rather than thirty-two fixtures — the same search and the same error, with a smaller bound |
| `module-zombie-swept-on-every-exit` | **not in the design; added in review.** The two exits where the NAME resolves to a zombie — the `-EBUSY` of one still busy, and the `0` after one is freed — each collect the zombies nobody named. Asserted on the zombie-list length across the one call, since a later unload's return value is satisfied by a sweep on any exit and so cannot tell them apart |

**The bug-proof.** `module-zombie-two-of-a-name` had to fail against
the **pre-build** tree because nothing there collected either of them —
one unrelated load or unload left both in place. That was the whole
claim, and it is weaker than what this report first wrote: the earlier
version said the second zombie was unreachable by name and built the
bug-proof on it, which was false. The test asserts that a single sweep
collects **both**, which is what distinguishes a sweep from the
one-at-a-time name lookup it sits beside.

`module-zombie-swept-on-every-exit` was bug-proofed the same way and
**once per exit**, because removing both sweeps at once proves only the
first: the failing test leaves its zombies behind and the later module
tests then fail on the contamination rather than on the mechanism.
Removing only the first sweep fails it at `module_zombie_count() == 1`;
removing only the second fails it at `module_zombie_count() == 0`, one
failure in 352.

**Watch the existing test — and the build did.**
`selftest_module_unload_busy` asserts a second unload frees the zombie.
A sweep at every load and unload could have freed it *first*, turning
that test's `0` into `-ENOENT`. The decision went the way this report
argued: the explicit call keeps succeeding when there is something to
free, because in `module_unload` the sweep runs **after** the name is
resolved, never before it (banner item 1). The test is unchanged and
still passes.

## Risks

- **Sweeping at load/unload adds a list walk under `g_lock`.** Bounded
  by the zombie count, which is normally zero and pathologically small;
  neither path is hot.
- **Freeing earlier changes when release code runs.** It does not: the
  sweep frees only zombies whose `live_objects` is already zero, which
  is exactly the condition the name-based reap already used.
- ~~**The existing zombie test may need to change**~~ — **it did not**,
  and that was the point of resolving the name before sweeping. The
  risk is recorded rather than deleted because a unit that quietly
  rewrites a passing test's expectation is worse than one that says it
  is doing so, and this one did not have to.

## Alternatives considered

- **A periodic reaper thread.** Collects case 1 and needs a thread, a
  period and a reason to prefer them over a walk on a cold path.
- **Reap only at load.** Cheaper and misses the case where the last
  module load has already happened and unloads continue.
- **Leave it and document the limitation.** The documentation would have
  to say "a module can pin its dependencies forever if nobody issues a
  request they have no reason to issue", which reads as a bug report
  rather than a design note.
