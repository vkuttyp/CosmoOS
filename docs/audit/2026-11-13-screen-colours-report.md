# The landing terminal, first PR: the screen's colours and escapes, 2026-11-13

The owner, on v1.1.0-rc2's screen in Parallels: "it does not contain a natural landing terminal of a Linux distro",
next to a screenshot of an Ubuntu terminal with a coloured system-facts banner. The owner chose a welcome screen in
colour with the prompt unchanged, and a login screen with it, after v1.1.0 (2026-11-13). The framebuffer console
interpreted no escape sequence: no colour, no clear, and an `ESC [ K` drew `[K`. This PR is the kernel's half: the
screen obeys the escapes a terminal program writes. The welcome and the login are the second PR.

## Scope and baseline

Branch `landing` from main at `fb2c9a61` (#519). In scope: `kernel/core/fbcon.c` and `kernel/core/fbrules.cee`
(the escape parser, the palette, cursor placement), the self-test `fb-escapes`, `make test-display`'s colour check;
and, to keep the kernel's C from growing, `kernel/core/cmdline.c` in CeeSafe.

## What changed

- **Each cell has colours.** An attribute byte per cell (foreground and background, sixteen colours each), in the
  text buffer's allocation; scroll, repaint, erase and a surface switch carry it. The palette is the PC's, but light
  grey stays the 0xd0 grey the console has always drawn, so the default screen is unchanged pixel for pixel.
- **Escapes** (`fbcon_esc_feed`, CeeSafe): SGR colours (0, 1, 22, 30-37, 39, 40-47, 49, 90-97, 100-107), `ESC [ 2 J`
  and `3 J` and `ESC c` (clear), `ESC [ r ; c H` and `f` (place), `ESC [ n A`-`D` (move), `ESC [ K` (erase to the
  end of the row). Any other sequence is read to its end and draws nothing; a byte that cannot be in one ends it.
- **The cursor stays on the screen** whatever a sequence asks (`fbcon_esc_place`, I-DIAG-22).
- **`cmdline.c` is CeeSafe** (`cmdline.cee`): the same parser, held to `tests/host/test_gpt.c`'s cases, which link
  it with a stand-in for `bootinfo_cmdline`. With the replay's tail (`fbcon_tail_start`) and the cursor placement
  moved to CeeSafe, `kernel/` has 47 fewer lines of C than main (91,575 to 91,528).
- **Documents**: `docs/kernel/diagnostics/` (design "Colours and escapes", API, I-DIAG-22, testing),
  `docs/boot/design.md` (the command line's source).

## Found before the PR

| Finding | Fix |
|---|---|
| The first version added 45 lines of C to `kernel/` and CI's `cee-share` would have refused it | the cursor placement and the replay's tail moved to CeeSafe, a clear reuses `fbcon_start`, and `cmdline.c` became CeeSafe |

## Validation

On the other machine (`docs/development.md`, "Running the matrix": the CI image, one container of 4 CPUs and 6
GiB), 2026-11-13, both architectures one after the other on the tree of this PR's code; `matrix: PASS` both.

| Step | x86-64 | AArch64 |
|---|---|---|
| debug, release, chaos and harness-retry builds, `make cee-check` | PASS | PASS |
| `make host-test` (`test_gpt`'s command-line cases on `cmdline.cee`), `make fuzz` | PASS | PASS |
| `make analyze` | clean (15 baselined, 0 unexpected, 0 stale) | clean (15 baselined, 0 unexpected, 0 stale) |
| debug `make test`, `QEMU_SMP=1` | PASS in 253.2 s | PASS in 272.1 s |
| debug `make test`, `QEMU_SMP=2` | PASS in 242.0 s | PASS in 282.2 s |
| debug `make test`, `QEMU_SMP=4` | PASS in 241.3 s | PASS in 297.4 s |
| `make test-smp2` | PASS in 231.9 s | PASS in 285.7 s |
| `make test-chaos` | PASS in 241.0 s | PASS in 305.6 s |
| `make test-harness-retry` | PASS in 242.2 s | PASS in 298.6 s |
| `make BUILD=release test` | PASS in 19.1 s | PASS in 22.1 s |

Also, on the same machine:

- `make BUILD=release test-display`: x86-64 PASS in 34.9 s, AArch64 PASS in 22.5 s; on both, `colour: ESC [ 1 ; 31 m
  drew 'RED' in (255, 85, 85) and ESC [ 0 m the next word in (208, 208, 208)`. On main the same row reads
  `colour-check [1;31mRED[0m grey`: the console dropped the ESC and drew the rest, so the check fails there.
- The debug boots' self-tests: `fb-escapes`, `fb-console`, `console-log-floor`, `fb-bench` and `fb-switch` ok on both
  architectures.
- `python3 scripts/cee-share.py --gate origin/main`: no tree has more C to convert than origin/main.
- The AArch64 chaos boot took 305.6 s, over the 300 s the plain boots are given and inside its own budget: that
  machine emulates AArch64 slowly (the quiet screen's report, "Limits").

## Limits

- The serial port receives a program's escapes as written; a terminal there obeys them, a log file keeps them.
- No 256-colour or true-colour SGR, no underline, no reverse video, no scroll regions, no `ESC [ J` other than 2 and
  3: a program that writes them gets the colours it asked for where they are among these, and nothing drawn for the
  rest.
- The colour check reads one word in one colour; the parser's other colours are checked by `fb-escapes` alone.
