# The guide's fetch by hostname checked a site the project does not run (TEST-19)

Date: 2026-10-22. Base: `main` at `d45235c3` (#437's merge). PR #438.

## Scope and baseline

Backlog TEST-19 (correctness, S). The guide test (`tests/guide/run_guide.py`,
CI jobs `guide` and `guide-macos`, and the release workflow's guide run)
types every command of `docs/guide/` and requires the output shown. The
networking page fetched `http://example.com/` and `https://example.com/`.
`docs/testing/flakes.md` has two sightings of that site answering
`HTTP/1.1 503 Service Unavailable`, both on 2026-10-22: CI run 39790028445
(`user guide commands (linux, x86_64)` on `d0778ec0`) and release run
39813732868 (`guide (macos, aarch64)`, `v1.0.10`). In both the guest's
`nslookup` passed and the request reached a server that answered it, so
the resolver, TCP and HTTP worked. No CosmoOS defect: the run depended on
a third party's front end.

## What changed

- The must-pass step resolves `vkuttyp.github.io` and fetches
  `https://vkuttyp.github.io/CosmoOS/releases/LATEST`, which the release
  workflow publishes (on 2026-10-22 it read `tag: v1.0.10`); the page
  expects `tag: v...`. HTTPS on its own, because the site redirects HTTP
  to it.
- The `example.com` fetches stay on the page, in a console block marked
  `optional`: a command whose expected line is missing is printed as a
  warning and the block's next command is still typed. A prompt timeout
  or a session that exits still fails the run.
- Any console command whose output says `server returned error: HTTP/1.x
  5NN` is typed again, up to three times, after 5, 10 and 20 s.

Codex's review of the first push found that an optional block abandoned
its remaining commands at the first warning (the HTTPS fetch was never
typed); the check is now per command, and the host test covers it.

## Validation

- `tests/host/test_run_guide.py` (`make host-test`): a 503 then a 200
  passes after one retry; four 503s fail after four tries; a 404 is not
  retried; in an optional block a missing line is noted and the next
  command still runs and is checked; outside one it fails; `optional` is
  refused on a host block.
- `tools/guide-fetch-probe.py --old` runs that test against `d45235c3`'s
  runner and fails at `a 503 is typed again and the retry passes`;
  without `--old` it passes.
- The whole guide on macOS x86-64 against a fresh `gmake release` set:
  `run-guide: PASS (macos, x86_64): 14 host blocks, 7 sessions, 24
  console blocks run, 15 for another host or architecture skipped`, the
  `LATEST` fetch printing `tag: v1.0.10`.
- The validation matrix on x86-64 and AArch64 (host-test, fuzz, analyze,
  debug `make test` at `QEMU_SMP=1/2/4`, `test-smp2`, `test-chaos`,
  `test-harness-retry`, `BUILD=release test`): every step exit 0 on the
  first push. The second push changes the runner and adds a host test;
  `make host-test` was run again for it.

## What is not done

The guide still shows `example.com` as the reader's example and a CI run
can still print a warning for it; a run passes with the warning.
