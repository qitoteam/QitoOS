# QTI — QiTo Icons

`.qti` replaces the old `.qac` (Qira Application iCon) format.

## Why QTI?

The old generator drew icons from character grids – ASCII art with a colour key. It was reviewable in diffs but not capable of real images.

QTI stores actual pixel data with hex colour values – real binary images, not ASCII art.

## Sizes

Five sizes, stored largest-first for single forward scan selection:

- 16, 32, 64, 128, 256

Default is 64 (the third). Built-in apps can switch default size; external apps may ship all five or any subset.

## Header

Suggested header 32 B, little-endian:

| Off | Size | Field |
|-----|------|-------|
| 0 | 4 | magic "QTI1" |
| 4 | 2 | version |
| 6 | 2 | frame_count (≤5) |
| 8 | 4 | payload_size |
| 12 | 4 | checksum (wrapping sum of payload) |
| 16 | 4 | flags |
| 20 | 12 | name[12] |

Then one 16-byte entry per frame:

| Off | Size | Field |
|-----|------|-------|
| 0 | 2 | width |
| 2 | 2 | height |
| 4 | 1 | encoding |
| 5 | 1 | palette_size |
| 6 | 2 | reserved |
| 8 | 4 | offset (from start of payload) |
| 12 | 4 | size |

Encodings:

- 0 RAW (BGRA, 4 bytes per pixel, stored BGRA, presented as ARGB)
- 1 RLE (5-byte runs: count 1-255, B, G, R, A)
- 2 INDEX (palette + 1 byte/pixel, palette is BGRA array)

Store frames largest-first so size selection is a single forward scan.

Validate every offset and checksum before decoding.

Python structs:

```
HEADER_FMT = "<4sHHIII12s"
ENTRY_FMT  = "<HHBBHII"
```

## Encoding selection

`tools/mkqti.py` tries all three encodings per frame and keeps smallest.

Typical icons are flat-shaded, so INDEX (palette) is usually smallest, RLE second, RAW fallback.

## Tools

`tools/mkqti.py` accepts real image input (PNG or raw RGBA) and emits multi-size QTI.

Usage:

```bash
# From PNG (requires Pillow)
mkqti.py --input logo.png --name logo --output logo.qti --sizes 256,128,64,32,16

# From raw RGBA
mkqti.py --input icon.rgba --width 64 --height 64 --raw --name icon --output icon.qti

# Built-in icons (fallback ASCII art converted to pixels)
mkqti.py --builtin --output-dir rootfs/usr/share/icons
```

This builds all 5 sizes from a single high-res source, picking smallest encoding per frame.

## Kernel decoder

`src/kernel/gfx/qti.c` implements full validation:

- magic "QTI1", version 1
- frame_count ≤5
- payload_size matches actual
- checksum verified
- each entry offset+size inside payload
- width/height ≤256, >0
- encoding 0-2

Decoding produces `struct qti_image { width, height, uint32_t *pixels }` with pixels in 0xAARRGGBB, top-left origin, heap-owned, must `qti_free`.

API:

```c
int qti_probe(const void *data, size_t len, struct qti_header *out);
int qti_decode(const void *data, size_t len, int preferred, struct qti_image *out);
int qti_load(const char *path, int preferred, struct qti_image *out);
void qti_free(struct qti_image *image);
void qti_draw(const struct qti_image *image, int x, int y); // with alpha blending
void qti_draw_scaled(const struct qti_image *image, int x, int y, int size);
```

## Registry

Icons in `/usr/share/icons/*.qti` are loaded at boot:

```c
qti_init(); // scans /usr/share/icons, loads each with preferred 64
const struct qti_image *icon = qti_get("terminal");
qti_draw_scaled(icon, x, y, 16);
```

Default is 64 px frame, scaled as needed (nearest-neighbour for bitmap crispness).

## Shell commands

```bash
qti list                # loaded icons, default 64
qti info /usr/share/icons/terminal.qti
```

Old `qac list/info` are kept as hidden aliases for backward compat.

## Example integration

Desktop window title bar:

```c
const struct qti_image *icon = qti_get(app->icon_name);
if (icon) qti_draw_scaled(icon, title_x, y, 16);
```

Image viewer can inspect `.qti` files on disk.

## Migration from QAC

- Magic changed QACI → QTI1
- Max frames 8 → 5
- Sizes fixed to 16,32,64,128,256
- Tools: mkqac.py → mkqti.py (real PNG/RGBA input)
- Kernel: qac.c → qti.c, but wrapper header keeps `qac_*` as aliases to `qti_*`

See `docs/FONTS.md` for typeface handling – icons are independent of fonts but both use the same registry pattern.
