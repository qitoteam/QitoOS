# Fonts

Qira OS ships four bitmap typefaces and lets you choose independently
which one the desktop uses and which one terminals use.

- **Generator:** [`tools/genfont.py`](../tools/genfont.py)
- **Registry:** [`src/kernel/gfx/font.c`](../src/kernel/gfx/font.c)
- **Header:** [`src/kernel/include/kernel/font.h`](../src/kernel/include/kernel/font.h)
- **Generated data:** `src/kernel/gfx/font_data.c` (not in version control)

---

## The bundled faces

| ID | Weight | Intended use | Notes |
| --- | --- | --- | --- |
| `qira-sans` | regular | desktop UI | proportional-looking humanist forms |
| `qira-sans-bold` | bold | UI emphasis | derived from `qira-sans` |
| `qira-mono` | regular | terminals | fixed pitch, **slashed zero** |
| `qira-mono-bold` | bold | terminal emphasis | derived from `qira-mono` |

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
ush> fonts set terminal qira-mono-bold       # change the terminal font
ush> fonts set ui qira-sans-bold             # change the desktop font
```

Changes take effect on the next redraw — you do not need to restart
anything.

### From configuration

Two keys in the system configuration store the preference across reboots:

| Key | Default | Applies to |
| --- | --- | --- |
| `desktop.font` | `qira-sans` | panels, menus, window title bars |
| `terminal.font` | `qira-mono` | the terminal application and the text console |

```
qcsh> config set terminal.font qira-mono-bold
qcsh> config save
```

### From C

```c
#include <kernel/font.h>

const struct font *f = font_terminal();
const uint8_t *glyph = font_glyph(f, 'A');   /* 16 rows, 1 byte each */
int width = font_text_width(f, "hello", 1);

font_set_ui("qira-sans-bold");
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

## Limitations

- **ASCII only.** No Unicode, no combining marks, no bidirectional text.
  `font_glyph()` returns the fallback glyph for anything outside
  U+0020–U+007F.
- **One size.** The cell is fixed at 8 × 16. `fb_draw_text()` accepts an
  integer scale factor for larger text, which produces blocky
  nearest-neighbour output rather than a properly designed larger face.
- **No antialiasing or hinting.** These are 1-bit bitmaps.
- **No kerning.** Advance width is a constant 8 pixels, so `qira-sans` is
  monospaced in practice despite its proportional-looking letterforms.
