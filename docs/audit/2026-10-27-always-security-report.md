# Roadmap 1.2's first unit, third PR: NET-6 and SEC-4, 2026-10-27

The third PR of the "Always, whichever direction" unit (after #464 and
#465). Two security lines from the backlog.

## Scope and baseline

Branch `always-security` from main at `6df2fce8`. Each defect has a test
commit that fails on main's code on every boot, then the fix.
`tools/always-security-probe.py --old` reverts the two fixes in a
throwaway worktree, keeping the tests.

## NET-6: the SYN-cookie secret

`tcp_init` drew the SYN-cookie secret once, and on a machine whose pool
seeds after it -- no firmware seed, no CPU random instruction, entropy
arriving with the drivers -- that secret came from an unseeded pool,
predictable in principle, and was kept for the whole boot. In this tree's
usual boots the loader's `EFI_RNG_PROTOCOL` seed makes the pool seeded
before `tcp_init`, so the defect needs a device-only machine to show.

- **Test (failing first):** a `CONFIG_SELFTEST` seam marks the current
  secret as drawn unseeded, the state such a machine starts in, and
  `net-syncookie-seeded` makes a cookie on the now-seeded pool and requires
  its secret to have been drawn from it. A pool that never seeded skips
  (invariant S19 already warns).
- **Fix:** `cookie_secret`, which every cookie made or checked goes
  through, draws the secret again the first time it runs after the pool
  has seeded, if `tcp_init` drew it before, and never after: one caller
  wins a compare-and-swap on the state (unseeded, drawing, seeded). A
  cookie made under the first secret fails its check and the peer's
  handshake starts over -- at most the few SYNs a boot answers before
  seeding. The security design's table of random-pool callers says so.

## SEC-4: the signal trampoline

Every Linux process had a read-only, executable page at the fixed
`0x7FFFFFFF1000`, holding the `rt_sigreturn` call: a known code address in
an otherwise randomised layout (roadmap M5).

- **Test (failing first):** lxtest now requires, on x86-64, nothing at
  that address (a store there faults `SEGV_MAPERR`), and on AArch64 that
  the trampoline -- found from the link register of a handler installed
  without `SA_RESTORER` -- is not at that address, holds the sigreturn call
  and is read-only.
- **Fix:** x86-64 maps no trampoline: a frame needs `SA_RESTORER`, as on
  Linux, so the page was never used. AArch64, where a handler without
  `SA_RESTORER` returns through it (Linux's counterpart is the randomised
  vDSO's sigreturn), maps it at a random page of the mmap range, drawn at
  every exec and kept by fork, recorded in the process's Linux state.

## Validation (2026-10-27, macOS host)

| Check | x86_64 | aarch64 |
|---|---|---|
| `tools/always-security-probe.py --old` | PASS: `net-syncookie-seeded` FAIL; lxtest's trampoline check fails (`SEGV_MAPERR` expected, `SEGV_ACCERR` seen) | PASS: the same (`tpage != LX_SIGTRAMP`) |
| `tools/always-security-probe.py` (debug `make test`) | PASS: `net-syncookie-seeded` ok, `LINUXTEST: PASS`; the boot PASS (184.2 s) | PASS (176.0 s) |
| `BUILD=release make test` | PASS (19.0 s) | PASS (21.1 s) |
| `make analyze` | clean: 7 baselined, 0 unexpected | clean, the same |
| `make host-test`, `make fuzz` | PASS, PASS | (host) |

The first `make fuzz` failed to link `fuzz_net_frame`: its shim
(`tests/fuzz/shim_net.c`) had no `random_ready`, which NET-6's test commit
made `tcp.c` call. The shim answers seeded (its source is deterministic),
folded into that commit so every commit builds; the rerun passed.

## Backlog

NET-6 and SEC-4 are done. The unit's last PR: BUILD-1.
