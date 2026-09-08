#!/usr/bin/env python3
"""
mkfont.py - Generate the kernel console font from the glyph art below.

The face is drawn here, in the tree, rather than copied from an existing
font: "a new operating system from scratch" excludes borrowed artifacts,
and ninety-six glyphs is an afternoon.

Each glyph is 5 pixels wide and 7 tall (row 7 carries descenders) inside
an 8x8 cell, so a byte per row holds a whole line of a glyph with the
leftmost pixel in bit 7. The table is 96 glyphs of 8 bytes: 0x20..0x7e,
plus a box at the end for anything the console cannot draw.

    tools/mkfont.py --out kernel/core/font8x8.c
    tools/mkfont.py --preview font.ppm      # look at the face

The output is checked in; regenerate it when the art changes.
"""

import argparse
import sys

WIDTH = 8
HEIGHT = 8
ART_WIDTH = 5
FIRST = 0x20
LAST = 0x7E

# Glyph art. Each entry is a name line (the character itself, "SP" for the
# space and "BOX" for the replacement glyph) followed by exactly 8 rows of
# exactly 5 cells, '#' for ink.
ART = r"""
SP
.....
.....
.....
.....
.....
.....
.....
.....
!
..#..
..#..
..#..
..#..
..#..
.....
..#..
.....
"
.#.#.
.#.#.
.....
.....
.....
.....
.....
.....
#
.#.#.
.#.#.
#####
.#.#.
#####
.#.#.
.#.#.
.....
$
..#..
.####
#.#..
.###.
..#.#
####.
..#..
.....
%
##..#
##.#.
...#.
..#..
.#...
#.##.
#..##
.....
&
.##..
#..#.
#.#..
.#...
#.#.#
#..#.
.##.#
.....
'
..#..
..#..
.....
.....
.....
.....
.....
.....
(
...#.
..#..
.#...
.#...
.#...
..#..
...#.
.....
)
.#...
..#..
...#.
...#.
...#.
..#..
.#...
.....
*
.....
..#..
#.#.#
.###.
#.#.#
..#..
.....
.....
+
.....
..#..
..#..
#####
..#..
..#..
.....
.....
,
.....
.....
.....
.....
.....
..##.
..#..
.#...
-
.....
.....
.....
#####
.....
.....
.....
.....
.
.....
.....
.....
.....
.....
.##..
.##..
.....
/
....#
...#.
..#..
..#..
.#...
#....
.....
.....
0
.###.
#...#
#..##
#.#.#
##..#
#...#
.###.
.....
1
..#..
.##..
..#..
..#..
..#..
..#..
.###.
.....
2
.###.
#...#
....#
..##.
.#...
#....
#####
.....
3
.###.
#...#
....#
..##.
....#
#...#
.###.
.....
4
...#.
..##.
.#.#.
#..#.
#####
...#.
...#.
.....
5
#####
#....
####.
....#
....#
#...#
.###.
.....
6
..##.
.#...
#....
####.
#...#
#...#
.###.
.....
7
#####
....#
...#.
..#..
.#...
.#...
.#...
.....
8
.###.
#...#
#...#
.###.
#...#
#...#
.###.
.....
9
.###.
#...#
#...#
.####
....#
...#.
.##..
.....
:
.....
.##..
.##..
.....
.##..
.##..
.....
.....
;
.....
.##..
.##..
.....
.##..
..#..
.#...
.....
<
...#.
..#..
.#...
#....
.#...
..#..
...#.
.....
=
.....
.....
#####
.....
#####
.....
.....
.....
>
.#...
..#..
...#.
....#
...#.
..#..
.#...
.....
?
.###.
#...#
....#
...#.
..#..
.....
..#..
.....
@
.###.
#...#
#.###
#.#.#
#.###
#....
.###.
.....
A
..#..
.#.#.
#...#
#...#
#####
#...#
#...#
.....
B
####.
#...#
#...#
####.
#...#
#...#
####.
.....
C
.###.
#...#
#....
#....
#....
#...#
.###.
.....
D
###..
#..#.
#...#
#...#
#...#
#..#.
###..
.....
E
#####
#....
#....
####.
#....
#....
#####
.....
F
#####
#....
#....
####.
#....
#....
#....
.....
G
.###.
#...#
#....
#.###
#...#
#...#
.###.
.....
H
#...#
#...#
#...#
#####
#...#
#...#
#...#
.....
I
.###.
..#..
..#..
..#..
..#..
..#..
.###.
.....
J
....#
....#
....#
....#
#...#
#...#
.###.
.....
K
#...#
#..#.
#.#..
##...
#.#..
#..#.
#...#
.....
L
#....
#....
#....
#....
#....
#....
#####
.....
M
#...#
##.##
#.#.#
#.#.#
#...#
#...#
#...#
.....
N
#...#
##..#
#.#.#
#..##
#...#
#...#
#...#
.....
O
.###.
#...#
#...#
#...#
#...#
#...#
.###.
.....
P
####.
#...#
#...#
####.
#....
#....
#....
.....
Q
.###.
#...#
#...#
#...#
#.#.#
#..#.
.##.#
.....
R
####.
#...#
#...#
####.
#.#..
#..#.
#...#
.....
S
.###.
#...#
#....
.###.
....#
#...#
.###.
.....
T
#####
..#..
..#..
..#..
..#..
..#..
..#..
.....
U
#...#
#...#
#...#
#...#
#...#
#...#
.###.
.....
V
#...#
#...#
#...#
#...#
#...#
.#.#.
..#..
.....
W
#...#
#...#
#...#
#.#.#
#.#.#
##.##
#...#
.....
X
#...#
#...#
.#.#.
..#..
.#.#.
#...#
#...#
.....
Y
#...#
#...#
.#.#.
..#..
..#..
..#..
..#..
.....
Z
#####
....#
...#.
..#..
.#...
#....
#####
.....
[
.###.
.#...
.#...
.#...
.#...
.#...
.###.
.....
\
#....
.#...
..#..
..#..
...#.
....#
.....
.....
]
.###.
...#.
...#.
...#.
...#.
...#.
.###.
.....
^
..#..
.#.#.
#...#
.....
.....
.....
.....
.....
_
.....
.....
.....
.....
.....
.....
#####
.....
`
.#...
..#..
.....
.....
.....
.....
.....
.....
a
.....
.....
.###.
....#
.####
#...#
.####
.....
b
#....
#....
####.
#...#
#...#
#...#
####.
.....
c
.....
.....
.###.
#....
#....
#...#
.###.
.....
d
....#
....#
.####
#...#
#...#
#...#
.####
.....
e
.....
.....
.###.
#...#
#####
#....
.###.
.....
f
..##.
.#..#
.#...
###..
.#...
.#...
.#...
.....
g
.....
.....
.####
#...#
#...#
.####
....#
.###.
h
#....
#....
####.
#...#
#...#
#...#
#...#
.....
i
..#..
.....
.##..
..#..
..#..
..#..
.###.
.....
j
...#.
.....
..##.
...#.
...#.
#..#.
#..#.
.##..
k
#....
#....
#..#.
#.#..
##...
#.#..
#..#.
.....
l
.##..
..#..
..#..
..#..
..#..
..#..
.###.
.....
m
.....
.....
##.#.
#.#.#
#.#.#
#.#.#
#.#.#
.....
n
.....
.....
####.
#...#
#...#
#...#
#...#
.....
o
.....
.....
.###.
#...#
#...#
#...#
.###.
.....
p
.....
.....
####.
#...#
#...#
####.
#....
#....
q
.....
.....
.####
#...#
#...#
.####
....#
....#
r
.....
.....
#.##.
##..#
#....
#....
#....
.....
s
.....
.....
.###.
#....
.###.
....#
####.
.....
t
.#...
.#...
###..
.#...
.#...
.#..#
..##.
.....
u
.....
.....
#...#
#...#
#...#
#..##
.##.#
.....
v
.....
.....
#...#
#...#
#...#
.#.#.
..#..
.....
w
.....
.....
#...#
#.#.#
#.#.#
#.#.#
.#.#.
.....
x
.....
.....
#...#
.#.#.
..#..
.#.#.
#...#
.....
y
.....
.....
#...#
#...#
#...#
.####
....#
.###.
z
.....
.....
#####
...#.
..#..
.#...
#####
.....
{
...#.
..#..
..#..
.#...
..#..
..#..
...#.
.....
|
..#..
..#..
..#..
..#..
..#..
..#..
..#..
.....
}
.#...
..#..
..#..
...#.
..#..
..#..
.#...
.....
~
.....
.#..#
#.#.#
#..#.
.....
.....
.....
.....
BOX
#####
#...#
#...#
#...#
#...#
#...#
#####
.....
"""


def parse():
    """Return {name: [8 row strings]} in the order the art declares."""
    lines = [ln.rstrip("\n") for ln in ART.strip("\n").split("\n")]
    glyphs = {}
    order = []
    i = 0
    while i < len(lines):
        name = lines[i]
        rows = lines[i + 1:i + 1 + HEIGHT]
        if len(rows) != HEIGHT:
            sys.exit(f"mkfont: glyph {name!r} has {len(rows)} rows, expected {HEIGHT}")
        for r in rows:
            if len(r) != ART_WIDTH or set(r) - {".", "#"}:
                sys.exit(f"mkfont: glyph {name!r} has a bad row {r!r}")
        if name in glyphs:
            sys.exit(f"mkfont: glyph {name!r} defined twice")
        glyphs[name] = rows
        order.append(name)
        i += 1 + HEIGHT
    return glyphs, order


def name_of(code):
    return "SP" if code == 0x20 else chr(code)


def table(glyphs):
    """[(code, label, [8 byte values])] for 0x20..0x7e then the box."""
    out = []
    for code in range(FIRST, LAST + 1):
        name = name_of(code)
        if name not in glyphs:
            sys.exit(f"mkfont: no glyph for 0x{code:02x} ({name!r})")
        out.append((code, name, rows_to_bytes(glyphs[name])))
    out.append((None, "BOX", rows_to_bytes(glyphs["BOX"])))
    return out


def rows_to_bytes(rows):
    """Leftmost pixel in bit 7, so a row reads left to right as written."""
    vals = []
    for r in rows:
        b = 0
        for x, cell in enumerate(r):
            if cell == "#":
                b |= 1 << (WIDTH - 1 - x)
        vals.append(b)
    return vals


def emit_c(entries, path):
    lines = [
        "/*",
        " * font8x8.c - The kernel console face. GENERATED by tools/mkfont.py;",
        " * edit the glyph art there and regenerate, never this file.",
        " *",
        f" * {len(entries)} glyphs of {HEIGHT} rows: 0x{FIRST:02x}..0x{LAST:02x} and a box for",
        " * everything else. The leftmost pixel of a row is bit 7.",
        " */",
        "",
        "#include <kernel/font.h>",
        "",
        f"const uint8_t font8x8[FONT_GLYPHS][FONT_HEIGHT] = {{",
    ]
    for code, label, vals in entries:
        where = f"0x{code:02x} '{label}'" if code is not None else "replacement"
        body = ", ".join(f"0x{v:02x}" for v in vals)
        lines.append(f"    {{ {body} }},   /* {where} */")
    lines += ["};", ""]
    with open(path, "w") as f:
        f.write("\n".join(lines))


def emit_preview(entries, path, columns=16):
    """A PPM of the whole face, so it can be looked at rather than trusted."""
    scale = 4
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

    glyphs, _ = parse()
    entries = table(glyphs)
    if args.out:
        emit_c(entries, args.out)
    if args.preview:
        emit_preview(entries, args.preview)
    if not args.out and not args.preview:
        ap.error("nothing to do: pass --out or --preview")


if __name__ == "__main__":
    main()
