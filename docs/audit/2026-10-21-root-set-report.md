# The root's record of its base system is checked, not trusted

Date: 2026-10-21. Base: `main` at `2acce0c3` (#429's merge; v1.0.8
published). A blocker for roadmap 1.2: any operator who follows v1.0.8's
release notes -- upgrade from v1.0.7, `--rollback`, roll forward again --
can never upgrade again. The Parallels VM "CosmoOS 1.0.7" is in that state.

## Scope and baseline

B4 (#426) made `/var/lib/cosmo-upgrade/root-set` the root's record of
which set's base system it holds, written by the base pass, and made the
upgrade refuse when it names another set than the current one (`the root
holds another set's base system than the current set's (a rollback half
done): cosmo-upgrade --repair first; nothing written`).

The sequence: v1.0.7 upgrades to v1.0.8 with v1.0.7's tool (no record, no
environment). On v1.0.8, `--rollback` returns the set and the base
system; its base pass writes the record naming v1.0.7's set. On v1.0.7,
v1.0.7's own `cosmo-upgrade --rollback` rolls forward to v1.0.8; its base
pass brings the root to v1.0.8's base system and, predating the record,
leaves it. On v1.0.8 the next upgrade is refused on the record alone, and
`--repair` cannot help: there is no `\cosmo\be` intent, so it says `no
boot environments recorded: nothing more to repair`.

## Decisions (the owner's)

1. The record is not trusted; it is checked against what is derived from
   the root. `--list` says when they disagree and names both; `--repair`
   corrects the record from the derived value, with no intent needed; the
   upgrade refuses only when the derived value says the root holds
   another set's base system, never on the record alone.
2. The record carries the set that wrote it, so the roll-forward case is
   detected and named.
3. The guide's upgrading page documents `--repair` for this state, and
   v1.0.8's published notes gain a known-issue paragraph pointing at it.

## The derived-truth rule, as built

`root_is(S, O)` (`cosmo-upgrade.c`): the root holds set S's base system
when every regular file and link S's `boot.tar` puts outside `/etc`
(`/bin`, `/sbin`, `/usr`, `/lib`, `/var/lib/pkg/repo`) is on the root
byte for byte (`holds`, the base pass's own test), and none of those
outside `/etc` that only the other kept set O has is on the root at all
(the base pass removes them). `/etc` is not looked at: a file there the
operator changed stays theirs. `root_base_read` asks it of the current
set first, then of `\cosmo\prev`'s; the first that holds is what the
root's files are. When neither does (a base file changed since, a
package that replaced one), nothing is decided by the files, and nothing
by the record alone either.

- **Upgrade** (with environments): refused only when the files are
  `\cosmo\prev`'s and not the current set's: `the root's base system is
  the previous set's (vX, ID), not the current set's (a rollback half
  done): cosmo-upgrade --repair first; nothing written`. When they are the
  current set's and the record says otherwise (or nothing), the record is
  corrected before the environment is taken, and said.
- **`--rollback`** to an environment: the same correction before the
  safety snapshot, so the tree it keeps carries the right record.
- **`--repair`**: corrects the record first, whether or not `\cosmo\be`
  has an intent (`the root's record named the previous set's (v1.0.7, ID)
  base system, but its files are the current set's (v1.0.8, ID): record
  corrected`); then the pairing, with what the root holds taken from its
  files (their record's only when they do not say).
- **`--list`** names what the files are (`the root's base system is the
  current set's (v1.0.9, ID)`) and, when the record disagrees, what it
  says and how to correct it; **`--status`** says the record is wrong.
- **The record** (`upgrade_be.h`): its first line stays the set's
  SHA-512 alone, which is all v1.0.8 reads, so a rollback to v1.0.8 still
  works; then `cosmo-root-set 2`, `release` (that set's kernel's build id,
  `release_of`) and `from` (the set the base pass brought the root from).
  A record naming another set than the files', written on the way from the
  set the files are, is a move an older cosmo-upgrade reversed; `--list`
  and the correction say so. `cosmo-install` writes it too (the set on the
  ESP it copied, the running kernel's release).

## Change

- `userland/system/upgrade_be.[ch]`: `struct root_set`,
  `rootset_parse`/`rootset_format` (version 1, v1.0.8's, and 2),
  `release_of`.
- `userland/system/cosmo-upgrade.c`: `root_set_mark(to, from)`,
  `root_set_read`, `root_is`, `root_base_read`/`root_base_load`,
  `root_set_correct`; the upgrade gate, `--rollback`, `--repair`,
  `--list` and `--status` as above.
- `userland/system/cosmo-install.c`: `write_root_set` (links
  `upgrade_fat.c`, `blkdevs.c`, `upgrade_be.c`).
- `tests/host/test_upgrade_be.c`: the record's format and parse (v1.0.8's
  form included), `release_of`.
- `tests/boot/be_test.py`: set B's distinguishing file moves out of
  `/etc` (`usr/share/cosmo-set-b`): the files now tell which set's base
  system the root holds, and `/etc` is not among them. The `forward`
  stage, on a disk of its own from the published v1.0.7 (`--older`);
  `--only forward`. `Makefile`: `test-be` is a target of its own
  (`BE_OLDER_TAG`, `BE_ONLY`), run by `test-upgrade` as before.
- `tools/root-set-probe.py`.
- `docs/userland/design.md`, `docs/guide/upgrading.md`,
  `docs/releases/v1.0.8.md` (and the published notes).

## Validation

Probe (`tools/root-set-probe.py`, each in a throwaway worktree from the
branch's head; `--old` reverse-applies d4781f61's change to the C sources
and `userland.mk`):

| arch | `--old` | fixed |
|---|---|---|
| x86_64 | PASS: the upgrade to B refused at the gate (exit 3) on the record v1.0.7 left | PASS: the forward stage passed |
| aarch64 | PASS: the same refusal (exit 3) | PASS: the forward stage passed |

`make BUILD=release test-be` (`--older` the published v1.0.7): x86_64
every stage PASS in 427.9 s, the forward stage 61.1 s; aarch64 the forward
stage alone (`BE_ONLY=forward`) PASS in 86 s, and every stage within
`test-upgrade` below.

Matrix (on the branch's head, `gmake`, macOS host):

| step | x86_64 | aarch64 |
|---|---|---|
| host-test | PASS 70 s | PASS 69 s |
| fuzz (host, `FUZZ_RUNS=50000`) | PASS 47 s | (host; once) |
| analyze | PASS 220 s | PASS 223 s |
| debug test, `QEMU_SMP=1` | PASS 232 s | PASS 253 s |
| debug test, `QEMU_SMP=2` | PASS 163 s | PASS 155 s |
| debug test, `QEMU_SMP=4` | PASS 156 s | PASS 174 s |
| test-smp2 | PASS 154 s | PASS 161 s |
| test-chaos | PASS 196 s | PASS 203 s |
| test-harness-retry | PASS 183 s | PASS 197 s |
| release test | PASS 21 s | PASS 23 s |
| release test-upgrade (test-be included) | PASS 647 s | PASS 805 s |

Branch CI: see the PR.
