#!/usr/bin/env python3
"""Convert Daniel Hart's SVG fonts into compact dot-grid C++ glyph tables."""

import html
import re
import sys
from pathlib import Path


def glyphs(path: Path):
    source = path.read_text()
    result = []
    for codepoint in range(32, 91):
        character = chr(codepoint)
        encoded = html.escape(character, quote=True)
        match = re.search(
            rf'<glyph unicode="{re.escape(encoded)}"(?: horiz-adv-x="(\d+)")? d="([^"]*)"',
            source,
        )
        if character == " ":
            result.append((2, [0] * 9))
            continue
        if not match:
            result.append((2, [0] * 9))
            continue
        advance = int(match.group(1) or 20) // 10
        # The regular cuts occupy seven rows above the baseline. Bold Tall
        # deliberately continues for two rows below it; those descender rows
        # are part of every numeral, not optional glyph decoration. Keep one
        # nine-row cell for all cuts so their shared baseline stays exact.
        rows = [0] * 9
        for x, y in re.findall(r'(?:^|z)M(-?[\d.]+) (-?[\d.]+)', match.group(2)):
            column = round((float(x) - 8.5355339059) / 10)
            row_from_bottom = round((float(y) - 1.4644660941) / 10)
            row = 6 - row_from_bottom
            if 0 <= column < 8 and 0 <= row < 9:
                rows[row] |= 1 << column
        result.append((advance, rows))
    return result


def emit(name: str, table):
    print(f"constexpr DotGlyph {name}[] = {{")
    for width, rows in table:
        values = ", ".join(f"0x{row:02x}" for row in rows)
        print(f"    {{{width}, {{{values}}}}},")
    print("};")


if len(sys.argv) != 4:
    raise SystemExit("usage: generate_dot_matrix_font.py REGULAR.svg BOLD.svg BOLD_TALL.svg")

print("""#pragma once
#include <stdint.h>

// Generated from Daniel Hart's Dot Matrix Typeface, pinned at
// 9d6d28877b368023ead04139be9e4e2e9172dfab and licensed under the OFL-1.1.
struct DotGlyph { uint8_t advance; uint8_t rows[9]; };
""")
emit("kDotRegular", glyphs(Path(sys.argv[1])))
emit("kDotBold", glyphs(Path(sys.argv[2])))
emit("kDotTall", glyphs(Path(sys.argv[3])))
