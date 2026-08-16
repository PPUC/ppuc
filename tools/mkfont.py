#!/usr/bin/env python3
"""Generates src/dmd/DmdFont5x7.h from the glyph art below.

The font is defined here as ASCII art rather than as hex tables so that a wrong
pixel is visible in review. Run this after editing GLYPHS and commit the
generated header; the header is a source file, not a build artifact, so there is
no generation step in the build and nothing for the "rebuild must be a no-op" CI
guard to trip over.

    python3 tools/mkfont.py > src/dmd/DmdFont5x7.h

Each glyph is 5 columns by 7 rows. '#' is a lit pixel, anything else is unlit.
Rows are packed one byte per row, bit 4 (0x10) leftmost, so a row reads the same
way in the art and in the data.
"""

import sys

CELL_W = 5
CELL_H = 7

# Only the characters a pinball display actually needs: digits, upper-case
# letters, and the punctuation used by scores and the built-in screens. An
# unmapped character renders as a space rather than as garbage.
GLYPHS = {
    ' ': ("     ", "     ", "     ", "     ", "     ", "     ", "     "),
    '0': (" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "),
    '1': ("  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "),
    '2': (" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"),
    '3': ("#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### "),
    '4': ("   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "),
    '5': ("#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "),
    '6': ("  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### "),
    '7': ("#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "),
    '8': (" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "),
    '9': (" ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  "),
    'A': (" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"),
    'B': ("#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "),
    'C': (" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "),
    'D': ("###  ", "#  # ", "#   #", "#   #", "#   #", "#  # ", "###  "),
    'E': ("#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"),
    'F': ("#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "),
    'G': (" ### ", "#   #", "#    ", "#  ##", "#   #", "#   #", " ####"),
    'H': ("#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"),
    'I': (" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "),
    'J': ("  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  "),
    'K': ("#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"),
    'L': ("#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"),
    'M': ("#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"),
    'N': ("#   #", "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #"),
    'O': (" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "),
    'P': ("#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "),
    'Q': (" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"),
    'R': ("#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"),
    'S': (" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "),
    'T': ("#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "),
    'U': ("#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "),
    'V': ("#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "),
    'W': ("#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"),
    'X': ("#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"),
    'Y': ("#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "),
    'Z': ("#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"),
    '-': ("     ", "     ", "     ", "#####", "     ", "     ", "     "),
    '.': ("     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "),
    ',': ("     ", "     ", "     ", "     ", " ##  ", " ##  ", "#    "),
    ':': ("     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     "),
    '/': ("    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    "),
    '>': ("#    ", " #   ", "  #  ", "   # ", "  #  ", " #   ", "#    "),
    '<': ("    #", "   # ", "  #  ", " #   ", "  #  ", "   # ", "    #"),
    '*': ("     ", "# # #", " ### ", "#####", " ### ", "# # #", "     "),
    '!': ("  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  "),
    '?': (" ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "),
    "'": ("  #  ", "  #  ", "     ", "     ", "     ", "     ", "     "),
    '+': ("     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "),
    '=': ("     ", "     ", "#####", "     ", "#####", "     ", "     "),
    '%': ("##   ", "##  #", "   # ", "  #  ", " #   ", "#  ##", "   ##"),
    '(': ("   # ", "  #  ", " #   ", " #   ", " #   ", "  #  ", "   # "),
    ')': (" #   ", "  #  ", "   # ", "   # ", "   # ", "  #  ", " #   "),
}

FIRST = 0x20
LAST = 0x7E


def row_bits(row: str) -> int:
    bits = 0
    for column in range(CELL_W):
        if column < len(row) and row[column] == '#':
            bits |= 1 << (CELL_W - 1 - column)
    return bits


def main() -> int:
    for character, art in GLYPHS.items():
        if len(art) != CELL_H:
            raise SystemExit(f"glyph {character!r} has {len(art)} rows, expected {CELL_H}")
        for row in art:
            if len(row) > CELL_W:
                raise SystemExit(f"glyph {character!r} has a row wider than {CELL_W}")

    out = sys.stdout
    out.write("// GENERATED by tools/mkfont.py -- do not edit by hand.\n")
    out.write("//\n")
    out.write("// Edit the glyph art in tools/mkfont.py and regenerate:\n")
    out.write("//     python3 tools/mkfont.py > src/dmd/DmdFont5x7.h\n")
    out.write("//\n")
    out.write("// 5x7 cell, one byte per row, bit 4 (0x10) leftmost. Characters outside the\n")
    out.write("// table render as a space.\n")
    out.write("\n#pragma once\n\n#include <cstdint>\n\n")
    out.write(f"inline constexpr uint8_t kDmdFont5x7First = 0x{FIRST:02X};\n")
    out.write(f"inline constexpr uint8_t kDmdFont5x7Last = 0x{LAST:02X};\n")
    out.write(f"inline constexpr uint8_t kDmdFont5x7Width = {CELL_W};\n")
    out.write(f"inline constexpr uint8_t kDmdFont5x7Height = {CELL_H};\n\n")
    out.write("inline constexpr uint8_t kDmdFont5x7[][%d] = {\n" % CELL_H)

    blank = ("     ",) * CELL_H
    for code in range(FIRST, LAST + 1):
        character = chr(code)
        art = GLYPHS.get(character, blank)
        packed = ", ".join(f"0x{row_bits(row):02X}" for row in art)
        label = "space" if character == ' ' else character
        out.write(f"    {{{packed}}},  // 0x{code:02X} {label}\n")

    out.write("};\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
