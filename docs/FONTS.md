# Fonts

QitoOS ships four bitmap typefaces and lets you choose independently
which one the desktop uses and which one terminals use.

- **Generator:** [`tools/genfont.py`](../tools/genfont.py)
- **Registry:** [`src/kernel/gfx/font.c`](../src/kernel/gfx/font.c)
- **Header:** [`src/kernel/include/kernel/font.h`](../src/kernel/include/kernel/font.h)
- **Generated data:** `src/kernel/gfx/font_data.c` (not in version control)

---

## The bundled faces

| ID | Weight | Intended use | Notes |
| --- | --- | --- | --- |
| `qito-sans` | regular | desktop UI | proportional-looking humanist forms |
| `qito-sans-bold` | bold | UI emphasis | derived from `qito-sans` |
| `qito-mono` | regular | terminals | fixed pitch, **slashed zero** |
| `qito-mono-bold` | bold | terminal emphasis | derived from `qito-mono` |

All four cover the 96 printable ASCII characters, U+0020 to U+007F, in an
**8 × 16 pixel cell**.

The `mono` faces disambiguate the characters that matter when you are
reading a hex dump or a path: zero is slashed, `1`/`l`/`I` and `0`/`O` are
visually distinct.

---

## Using them

### From a shell

```
ush> fonts                                   # list every registered face
ush> fonts set terminal qito-mono-bold       # change the terminal font
ush> fonts set ui qito-sans-bold             # change the desktop font
```

Changes take effect on the next redraw — you do not need to restart
anything.

### From configuration

Two keys in the system configuration store the preference across reboots:

| Key | Default | Applies to |
| --- | --- | --- |
| `desktop.font` | `qito-sans` | panels, menus, window title bars |
| `terminal.font` | `qito-mono` | the terminal application and the text console |

```
qcsh> config set terminal.font qito-mono-bold
qcsh> config save
```

### From C

```c
#include <kernel/font.h>

const struct font *f = font_terminal();
const uint8_t *glyph = font_glyph(f, 'A');   /* 16 rows, 1 byte each */
int width = font_text_width(f, "hello", 1);

font_set_ui("qito-sans-bold");
```

`font_find()` looks a face up by ID and returns `NULL` if it is unknown;
`font_at()` and `font_count()` enumerate the registry.

---

## Glyph geometry

Understanding the metrics matters if you draw or edit glyphs.

```
row  0  ┐
row  1  ├─ TOP_PAD — always blank, gives line spacing
row  2  ┐
  ...   ├─ capital height: uppercase and digits live in rows 2–11
row 11  ┘
row 12  ── baseline: the bottom of most lowercase letters
row 13  ┐
row 14  ├─ descender zone: g j p q y and the comma reach down here
row 15  ┘  blank, separates one line from the next
```

Each glyph is 16 bytes, one per row, most-significant bit leftmost. The
art grids in `genfont.py` are 14 rows and get one blank row of padding at
the top and one at the bottom.

> **A bug worth remembering.** The unit tests assert that descenders
> actually descend. That check caught a real defect during development:
> lowercase `g` had no descender at all, and `y`, `p`, `q`, `j` and `,`
> were sitting *on* the baseline at row 12 instead of below it. The text
> looked subtly wrong in a way that is hard to spot by eye but obvious to
> an assertion. If you edit glyph art, run `make test-unit`.

---

## Bold derivation

The bold faces are not drawn by hand. Each row is smeared one pixel to
the right and OR-ed with itself:

```c
bold_row = row | (row >> 1);
```

This is the classic bitmap-emboldening trick. It is cheap, it keeps the
bold face perfectly aligned with the regular one, and at 8 × 16 it looks
better than a separately drawn bold would at this size — there simply is
not enough room for genuinely different letterforms.

---

## Adding a face

1. Open [`tools/genfont.py`](../tools/genfont.py) and copy an existing
   face definition. Glyphs are ASCII art — `#` for a set pixel, space or
   `.` for clear — which keeps them reviewable in a diff.
2. Respect the geometry above: caps in rows 2–11, baseline at row 12,
   descenders in rows 13–14.
3. Register the face in the table at the bottom of the script.
4. Regenerate and rebuild:

   ```sh
   python3 tools/genfont.py --output src/kernel/gfx/font_data.c
   make
   ```

5. Run `make test-unit`. The font tests check glyph coverage, cell
   dimensions, baseline placement and descender depth for every
   registered face.

`font_data.c` is generated and is listed in `.gitignore`; the build
regenerates it automatically when `genfont.py` changes.

---

## Unicode – new in 0.4.0 Nova

- **UTF-8 decoding:** `src/kernel/gfx/unicode.c` provides `utf8_decode`, `utf8_encode`, `utf8_strlen`, `utf8_is_valid`. Fonts still 8×16 cell, but now decode UTF-8 to codepoints.
- **Glyph cache:** 512 entries LRU, `glyph_cache_init`, `glyph_cache_get`, `glyph_cache_put`.
- **Coverage:** ASCII 0x20-0x7F plus Latin-1 0xA0-0xFF, Greek 0x0370-0x03FF, Cyrillic 0x0400-0x04FF, box-drawing 0x2500-0x257F (─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼).
- **Box drawing:** `box_drawing_glyph()` returns 8×16 bitmap for common box chars.
- **Limitations still:** One size 8×16, no combining marks, no bidi, no antialiasing/hinting, no kerning (advance 8px, so qito-sans monospaced in practice). But now `font_glyph` can fallback to cache or box-drawing for Unicode beyond ASCII.

## Limitations (updated)

- **One size.** Fixed 8×16 cell, scale factor gives blocky nearest-neighbour.
- **No antialiasing or hinting.** 1-bit bitmaps.
- **No kerning.** Advance width constant 8.
- **Combining marks and bidi not yet:** UTF-8 decoded but combining marks not composed, bidi not implemented.
- **Coverage:** ASCII + Latin-1/Greek/Cyrillic/box-drawing via cache, not full Unicode yet – but infrastructure ready for Minecraft which needs box-drawing for UI.
