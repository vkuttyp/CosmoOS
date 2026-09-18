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
3. **N zombies of one name need N of those calls.**
   `find_zombie_locked` returns the **first** match
   (`module.c:457-465`), and the reap `list_remove`s it
   (`module.c:482`), so a later call does reach the next one.

   **An earlier draft of this report said the second one was
   unreachable by any call. That was wrong** — it read the first-match
   lookup and did not check that the reap removes what it found. The
   true statement is weaker and still bad: each zombie needs its own
   call, and case 1 is that nobody makes even the first.

The inventory records this as "zombie modules are reaped only by a later
`module_unload` of the same name", which is accurate and reads as an
inconvenience. It is worse than that — a reused name hides a zombie
entirely, and the pins it holds are permanent for as long as it is
unreaped — but the memory is reachable, and this report says so because
its first draft did not.

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
| `docs/kernel/modules/` (design and invariants) | when a zombie is collected, and that it is by identity |
| `docs/audit/2026-09-deferred-work-inventory.md` | §4's last two small debts struck |
| `README.md` | the Status entry |

## Tests

| test | asserts |
| --- | --- |
| `module-zombie-swept` | a zombie whose objects die is freed by the **next unrelated load or unload**, with no unload of its own name |
| `module-zombie-name-reused` | a replacement loaded under the zombie's name does not hide it: the sweep still collects it, and the replacement is untouched |
| `module-zombie-two-of-a-name` | two zombies sharing a name are **both** collected by one sweep — today each needs its own `module_unload` call, and nothing makes any of them |
| `module-zombie-holds-deps` | unchanged in substance from what `selftest_module_unload_busy` proves, plus: once swept, the dependency pin is **released**, which is the consequence that matters |
| `module-slots-enospc` | exhausting `MODULE_MAX_LIVE` returns `-ENOSPC` and the machine lives |

**The bug-proof.** `module-zombie-two-of-a-name` must fail against the
current tree because **nothing collects either of them** — one
unrelated load or unload leaves both in place. That is the whole claim,
and it is weaker than what this report first wrote: the earlier version
said the second zombie was unreachable by name and built the bug-proof
on it, which was false. The test asserts that a single sweep collects
**both**, which is what distinguishes a sweep from the one-at-a-time
name lookup that exists today.

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
