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
  moved to CeeSafe, `kernel/` has 39 fewer lines of C than main (91,575 to 91,536, `scripts/cee-share.py --ref`).
- **Documents**: `docs/kernel/diagnostics/` (design "Colours and escapes", API, I-DIAG-22, testing),
  `docs/boot/design.md` (the command line's source).

## Found before the PR

| Finding | Fix |
|---|---|
| The first version added 45 lines of C to `kernel/` and CI's `cee-share` would have refused it | the cursor placement and the replay's tail moved to CeeSafe, and `cmdline.c` became CeeSafe |

## Found in review

Greptile and Codex on #520; each fix is held by a new `fb-escapes` case.

| Finding | Fix |
|---|---|
| `ESC [ 66 ; 1 H` did not move: a row of 65 to 68 was read as the direction 'A' to 'D' of a relative move, and 1920x1080 has 67 rows | the parser records which of the two it returned (`fbcon_esc.kind`) |
| A CSI with a private or intermediate byte ran its final byte: `ESC [ > 4 ; 1 m` made bold, `ESC [ ? 2 J` cleared | such a sequence is read to its final byte and not obeyed |
| `ESC ( B` drew `B`; `ESC ] 0 ; title BEL` drew `0;title` | an ESC with intermediate bytes is read through the byte after them; a control string through its BEL or ST |
| An erase, a clear and a scroll's new rows were black whatever the background (`ESC [ 44 m ESC [ K`) | they blank in the colours being written, as the Linux console does |
| `ESC [ K` at a pending wrap (the cursor past the last column) erased nothing | it blanks the last cell and cancels the wrap |
| A surface switch kept the rows through the cursor's only, so a program that moved the cursor up lost the rows below it | `fbcon_rows_used`: through the cursor's row and the last row with text, the cursor's row kept on the new screen |
| The design and API said the early console holds 65536 cells; since the colours it holds 32768 | both say 65536 bytes, 32768 cells |

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
  drew 'RED' in (255, 85, 85) and ESC [ 0 m the next word in (208, 208, 208)`. Main's `putc` drops a control
  byte and draws the rest, so on main that row would read `colour-check [1;31mRED[0m grey` and the check would
  fail on the missing `colour-check RED grey` (from the code; the check was not run against main).
- The debug boots' self-tests: `fb-escapes`, `fb-console`, `console-log-floor`, `fb-bench` and `fb-switch` ok on both
  architectures.
- `python3 scripts/cee-share.py --gate origin/main`: no tree has more C to convert than origin/main.
- After the review's fixes (commit `001ffcf7`), on the developer's Mac: debug `make test` x86-64 PASS in 217.9 s,
  AArch64 PASS in 204.1 s, with `fb-escapes` (its eight new cases), `fb-console`, `console-log-floor`, `fb-bench`
  and `fb-switch` ok on both; `make BUILD=release test-display` x86-64 PASS in 34.0 s, AArch64 PASS in 33.8 s, the
  colour check reading the same pixels; `make host-test` and `make cee-check` pass; the `cee-share` gate as above.
  The matrix above ran on the tree before these fixes and was not run again.
- The AArch64 chaos boot took 305.6 s, over the 300 s the plain boots are given and inside its own budget: that
  machine emulates AArch64 slowly (the quiet screen's report, "Limits").

## Limits

- The serial port receives a program's escapes as written; a terminal there obeys them, a log file keeps them.
- No 256-colour or true-colour SGR, no underline, no reverse video, no scroll regions, no `ESC [ J` other than 2 and
  3: a program that writes them gets the colours it asked for where they are among these, and nothing drawn for the
  rest.
- The colour check reads one word in one colour; the parser's other colours are checked by `fb-escapes` alone.
