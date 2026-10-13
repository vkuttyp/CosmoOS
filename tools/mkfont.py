#!/usr/bin/env python3
"""
mkfont.py - Generate the kernel console face from the glyph art below.

The face is drawn here, in the tree, rather than copied from an existing
font: "a new operating system from scratch" excludes borrowed artifacts.

The cell is 8 pixels wide and 16 tall, the proportions of a PC or Linux
text console. A glyph is drawn in the left 7 columns, so column 7 is the
space between letters, and in rows 2 to 14: rows 0, 1 and 15 stay blank,
the space between lines. Within the art (rows 0 to 12 below = cell rows
2 to 14):

    0       ascenders and the top of brackets
    1       capitals and digits start
    4       lowercase starts (the x-height: 7 rows)
    10      the baseline: the last row of every letter without a descender
    11-12   descenders

A byte per row holds a whole line with the leftmost pixel in bit 7. The
table is 96 glyphs of 16 bytes: 0x20..0x7e, plus a box at the end for
anything the console cannot draw.

Each glyph is a name line (the character, "SP" for the space, "BOX" for
the replacement) followed by rows written as `R pattern` or `R-S pattern`
(the same pattern for art rows R to S); a row not given is blank. A
pattern is 7 cells, '#' for ink.

    tools/mkfont.py --out kernel/core/font8x16.c
    tools/mkfont.py --preview font.ppm      # look at the face

The output is checked in; regenerate it when the art changes.
"""

import argparse
import sys

WIDTH = 8
HEIGHT = 16
ART_WIDTH = 7
ART_ROWS = 13
ART_TOP = 2          # art row 0 is cell row 2
FIRST = 0x20
LAST = 0x7E

ART = r"""
SP
!
1-7 ...#...
9-10 ...#...
"
1-3 .#...#.
#
2-3 ..#.#..
4 #######
5-6 ..#.#..
7 #######
8-9 ..#.#..
$
0 ...#...
1 .#####.
2 #..#..#
3-4 #..#...
5 .#####.
6-7 ...#..#
8 #..#..#
9 .#####.
10 ...#...
%
1 .##...#
2 #..#.#.
3 .##..#.
4 ....#..
5-6 ...#...
7 ..#....
8 .#..##.
9 .#.#..#
10 #...##.
&
1 ..##...
2-3 .#..#..
4 ..##...
5 .##....
6 #..#..#
7 #...##.
8 #....#.
9 #...#.#
10 .###..#
'
1-3 ...#...
(
0 ....#..
1 ...#...
2-9 ..#....
10 ...#...
11 ....#..
)
0 ..#....
1 ...#...
2-9 ....#..
10 ...#...
11 ..#....
*
4 .#.#.#.
5 ..###..
6 #######
7 ..###..
8 .#.#.#.
+
4-5 ...#...
6 #######
7-8 ...#...
,
9-10 ...##..
11 ...#...
12 ..#....
-
6 .#####.
.
9-10 ...##..
/
1 ......#
2-3 .....#.
4 ....#..
5-6 ...#...
7 ..#....
8-9 .#.....
10 #......
0
1 .#####.
2 #.....#
3 #....##
4 #...#.#
5 #..#..#
6 #.#...#
7 ##....#
8-9 #.....#
10 .#####.
1
1 ...#...
2 ..##...
3 .#.#...
4-9 ...#...
10 .#####.
2
1 .#####.
2 #.....#
3-4 ......#
5 .....#.
6 ....#..
7 ...#...
8 ..#....
9 .#.....
10 #######
3
1 .#####.
2 #.....#
3-4 ......#
5 ..####.
6-8 ......#
9 #.....#
10 .#####.
4
1 ....##.
2 ...#.#.
3 ..#..#.
4 .#...#.
5 #....#.
6 #######
7-10 .....#.
5
1 #######
2-4 #......
5 ######.
6-8 ......#
9 #.....#
10 .#####.
6
1 ..####.
2 .#.....
3-4 #......
5 ######.
6-9 #.....#
10 .#####.
7
1 #######
2 ......#
3 .....#.
4 ....#..
5-10 ...#...
8
1 .#####.
2-4 #.....#
5 .#####.
6-9 #.....#
10 .#####.
9
1 .#####.
2-5 #.....#
6 .######
7-8 ......#
9 .....#.
10 .####..
:
4-5 ...##..
9-10 ...##..
;
4-5 ...##..
9-10 ...##..
11 ...#...
12 ..#....
<
2 .....#.
3 ....#..
4 ...#...
5 ..#....
6 .#.....
7 ..#....
8 ...#...
9 ....#..
10 .....#.
=
5 #######
8 #######
>
2 .#.....
3 ..#....
4 ...#...
5 ....#..
6 .....#.
7 ....#..
8 ...#...
9 ..#....
10 .#.....
?
1 .#####.
2 #.....#
3 ......#
4 .....#.
5 ....#..
6-7 ...#...
9-10 ...#...
@
1 .#####.
2 #.....#
3 #..####
4-6 #.#...#
7 #..####
8-9 #......
10 .######
A
1 ..###..
2 .#...#.
3-5 #.....#
6 #######
7-10 #.....#
B
1 ######.
2-4 #.....#
5 ######.
6-9 #.....#
10 ######.
C
1 .#####.
2 #.....#
3-8 #......
9 #.....#
10 .#####.
D
1 #####..
2 #....#.
3-8 #.....#
9 #....#.
10 #####..
E
1 #######
2-4 #......
5 #####..
6-9 #......
10 #######
F
1 #######
2-4 #......
5 #####..
6-10 #......
G
1 .#####.
2 #.....#
3-5 #......
6 #..####
7-9 #.....#
10 .#####.
H
1-4 #.....#
5 #######
6-10 #.....#
I
1 .#####.
2-9 ...#...
10 .#####.
J
1 ...####
2-7 .....#.
8-9 #....#.
10 .####..
K
1 #.....#
2 #....#.
3 #...#..
4 #..#...
5 ###....
6 #..#...
7 #...#..
8 #....#.
9-10 #.....#
L
1-9 #......
10 #######
M
1 #.....#
2 ##...##
3 #.#.#.#
4 #..#..#
5-10 #.....#
N
1 #.....#
2-3 ##....#
4 #.#...#
5-6 #..#..#
7 #...#.#
8-9 #....##
10 #.....#
O
1 .#####.
2-9 #.....#
10 .#####.
P
1 ######.
2-4 #.....#
5 ######.
6-10 #......
Q
1 .#####.
2-7 #.....#
8 #...#.#
9 #....#.
10 .####.#
R
1 ######.
2-4 #.....#
5 ######.
6 #..#...
7 #...#..
8 #....#.
9-10 #.....#
S
1 .#####.
2 #.....#
3-4 #......
5 .#####.
6-8 ......#
9 #.....#
10 .#####.
T
1 #######
2-10 ...#...
U
1-9 #.....#
10 .#####.
V
1-4 #.....#
5-7 .#...#.
8-9 ..#.#..
10 ...#...
W
1-5 #.....#
6-7 #..#..#
8 #.#.#.#
9 ##...##
10 #.....#
X
1 #.....#
2-3 .#...#.
4 ..#.#..
5-6 ...#...
7 ..#.#..
8-9 .#...#.
10 #.....#
Y
1-2 #.....#
3 .#...#.
4 ..#.#..
5-10 ...#...
Z
1 #######
2 ......#
3 .....#.
4 ....#..
5 ...#...
6 ..#....
7 .#.....
8-9 #......
10 #######
[
0 ..####.
1-10 ..#....
11 ..####.
\
1 #......
2-3 .#.....
4 ..#....
5-6 ...#...
7 ....#..
8-9 .....#.
10 ......#
]
0 .####..
1-10 ....#..
11 .####..
^
1 ...#...
2 ..#.#..
3 .#...#.
4 #.....#
_
12 #######
`
1 ..#....
2 ...#...
3 ....#..
a
4 .#####.
5-6 ......#
7 .######
8-9 #.....#
10 .######
b
1-3 #......
4 ######.
5-9 #.....#
10 ######.
c
4 .#####.
5 #.....#
6-8 #......
9 #.....#
10 .#####.
d
1-3 ......#
4 .######
5-9 #.....#
10 .######
e
4 .#####.
5-6 #.....#
7 #######
8-9 #......
10 .#####.
f
1 ...###.
2-3 ..#....
4 ######.
5-10 ..#....
g
4 .######
5-9 #.....#
10 .######
11 ......#
12 .#####.
h
1-3 #......
4 #.####.
5 ##....#
6-10 #.....#
i
1 ...#...
4 ..##...
5-9 ...#...
10 ..###..
j
1 .....#.
4 ....##.
5-10 .....#.
11 #....#.
12 .####..
k
1-3 #......
4 #....#.
5 #...#..
6 #..#...
7 ###....
8 #..#...
9 #...#..
10 #....#.
l
1 ..##...
2-9 ...#...
10 ..###..
m
4 ###.##.
5-10 #..#..#
n
4 #.####.
5 ##....#
6-10 #.....#
o
4 .#####.
5-9 #.....#
10 .#####.
p
4 ######.
5-9 #.....#
10 ######.
11-12 #......
q
4 .######
5-9 #.....#
10 .######
11-12 ......#
r
4 #.####.
5 ##....#
6-10 #......
s
4 .######
5-6 #......
7 .#####.
8-9 ......#
10 ######.
t
1-3 ..#....
4 ######.
5-9 ..#....
10 ...###.
u
4-9 #.....#
10 .######
v
4-5 #.....#
6-7 .#...#.
8-9 ..#.#..
10 ...#...
w
4-5 #.....#
6-8 #..#..#
9 #.#.#.#
10 .#...#.
x
4 #.....#
5 .#...#.
6 ..#.#..
7 ...#...
8 ..#.#..
9 .#...#.
10 #.....#
y
4-9 #.....#
10 .######
11 ......#
12 .#####.
z
4 #######
5 .....#.
6 ....#..
7 ...#...
8 ..#....
9 .#.....
10 #######
{
0 ....##.
1-4 ...#...
5 .##....
6-10 ...#...
11 ....##.
|
0-12 ...#...
}
0 .##....
1-4 ...#...
5 ....##.
6-10 ...#...
11 .##....
~
5 .##....
6 #..#..#
7 ....##.
BOX
1 #######
2-9 #.....#
10 #######
"""


def parse():
    """Return {name: [ART_ROWS row strings]}."""
    lines = [ln.rstrip() for ln in ART.strip("\n").split("\n")]
    glyphs = {}
    name = None
    for ln in lines:
        if not ln:
            continue
        head, _, pat = ln.partition(" ")
        rows = head.split("-")
        if name is not None and pat and all(r.isdigit() for r in rows) and len(rows) <= 2:
            lo, hi = int(rows[0]), int(rows[-1])
            if not (0 <= lo <= hi < ART_ROWS):
                sys.exit(f"mkfont: glyph {name!r}: rows {head} outside 0..{ART_ROWS - 1}")
            if len(pat) != ART_WIDTH or set(pat) - {".", "#"}:
                sys.exit(f"mkfont: glyph {name!r}: bad pattern {pat!r}")
            for r in range(lo, hi + 1):
                if glyphs[name][r] is not None:
                    sys.exit(f"mkfont: glyph {name!r}: row {r} given twice")
                glyphs[name][r] = pat
            continue
        name = ln
        if name in glyphs:
            sys.exit(f"mkfont: glyph {name!r} defined twice")
        glyphs[name] = [None] * ART_ROWS
    return {n: [r if r is not None else "." * ART_WIDTH for r in rows] for n, rows in glyphs.items()}


def name_of(code):
    return "SP" if code == 0x20 else chr(code)


def rows_to_bytes(art):
    """The 16 cell rows, leftmost pixel in bit 7; blank outside the art."""
    vals = [0] * HEIGHT
    for i, r in enumerate(art):
        b = 0
        for x, cell in enumerate(r):
            if cell == "#":
                b |= 1 << (WIDTH - 1 - x)
        vals[ART_TOP + i] = b
    return vals


def table(glyphs):
    out = []
    for code in range(FIRST, LAST + 1):
        name = name_of(code)
        if name not in glyphs:
            sys.exit(f"mkfont: no glyph for 0x{code:02x} ({name!r})")
        out.append((code, name, rows_to_bytes(glyphs[name])))
    out.append((None, "BOX", rows_to_bytes(glyphs["BOX"])))
    extra = set(glyphs) - {name_of(c) for c in range(FIRST, LAST + 1)} - {"BOX"}
    if extra:
        sys.exit(f"mkfont: glyphs for no character: {sorted(extra)}")
    return out


def emit_c(entries, path):
    lines = [
        "/*",
        " * font8x16.c - The kernel console face. GENERATED by tools/mkfont.py;",
        " * edit the glyph art there and regenerate, never this file.",
        " *",
        f" * {len(entries)} glyphs of {HEIGHT} rows: 0x{FIRST:02x}..0x{LAST:02x} and a box for",
        " * everything else. The leftmost pixel of a row is bit 7.",
        " */",
        "",
        "#include <kernel/font.h>",
        "",
        "const uint8_t font8x16[FONT_GLYPHS][FONT_HEIGHT] = {",
    ]
    for code, label, vals in entries:
        where = f"0x{code:02x} '{label}'" if code is not None else "replacement"
        body = ", ".join(f"0x{v:02x}" for v in vals)
        lines.append(f"    {{ {body} }},   /* {where} */")
    lines += ["};", ""]
    with open(path, "w") as f:
        f.write("\n".join(lines))


def emit_preview(entries, path, columns=16, scale=3):
    """A PPM of the whole face, so it can be looked at rather than trusted."""
    rows = (len(entries) + columns - 1) // columns
    w = columns * WIDTH * scale
    h = rows * HEIGHT * scale
    px = [[0] * w for _ in range(h)]
    for i, (_, _, vals) in enumerate(entries):
        cx = (i % columns) * WIDTH * scale
        cy = (i // columns) * HEIGHT * scale
        for y, v in enumerate(vals):
            for x in range(WIDTH):
                if v & (1 << (WIDTH - 1 - x)):
                    for dy in range(scale):
                        for dx in range(scale):
                            px[cy + y * scale + dy][cx + x * scale + dx] = 255
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(bytes(v for row in px for v in row for _ in range(3)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="write the C table here")
    ap.add_argument("--preview", help="write a PPM of the face here")
    args = ap.parse_args()
    entries = table(parse())
    if args.out:
        emit_c(entries, args.out)
    if args.preview:
        emit_preview(entries, args.preview)
    if not args.out and not args.preview:
        ap.error("nothing to do: pass --out or --preview")


if __name__ == "__main__":
    main()
