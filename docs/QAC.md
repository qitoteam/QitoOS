# QAC — Qira Application iCon

`.qac` is the icon format used by the Qira OS desktop. It exists because a
kernel that draws its own user interface needs an image format it can
decode without a heap of third-party code, and because icons have
requirements that general-purpose formats handle badly.

- **Magic:** `QACI`
- **Extension:** `.qac`
- **Endianness:** little-endian throughout
- **Decoder:** [`src/kernel/gfx/qac.c`](../src/kernel/gfx/qac.c)
- **Header:** [`src/kernel/include/kernel/qac.h`](../src/kernel/include/kernel/qac.h)
- **Builder:** [`tools/mkqac.py`](../tools/mkqac.py)

---

## Design rationale

PNG would mean vendoring zlib into a freestanding kernel. BMP has no
compression worth the name and no multi-size support. What an icon format
actually needs is narrow:

- **Multiple sizes in one file.** The same icon is drawn at 16 px in a
  window title bar and 32 px in the application menu. Scaling one bitmap
  down looks bad; a format that stores both is simpler *and* prettier.
- **An alpha channel**, because icons are composited over a wallpaper.
- **Compression that is trivial to decode.** Icons are flat-shaded with
  long runs of identical pixels, so run-length encoding gets most of the
  benefit of a real compressor for about forty lines of code.
- **Bounded, checkable structure.** The decoder runs in kernel space.
  Every offset must be validatable before use.

QAC is that and nothing else. The decoder is roughly 200 lines.

---

## File layout

```
offset 0    ┌───────────────────────────────┐
            │ struct qac_header     32 bytes│
            ├───────────────────────────────┤
            │ struct qac_entry[]    16 each │   frame_count entries
            ├───────────────────────────────┤
            │ frame payloads                │   offsets relative to here
            └───────────────────────────────┘
```

The payload area begins immediately after the frame table. Every
`offset` in a frame entry is measured **from the start of the payload
area**, not from the start of the file.

---

## Header — 32 bytes

Python `struct` format: `<4sHHIII12s`

| Offset | Size | Field | Notes |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | `"QACI"` |
| 4 | 2 | `version` | `1` |
| 6 | 2 | `frame_count` | 1–8 |
| 8 | 4 | `payload_size` | total bytes of all frame payloads |
| 12 | 4 | `checksum` | wrapping byte sum over the payload area only |
| 16 | 4 | `flags` | reserved, must be 0 |
| 20 | 12 | `name` | NUL-padded icon name |

The checksum covers the **payload only** — not the header, not the frame
table. It is an integrity check, not a signature.

---

## Frame table — 16 bytes per entry

Python `struct` format: `<HHBBHII`

| Offset | Size | Field | Notes |
| ---: | ---: | --- | --- |
| 0 | 2 | `width` | pixels, ≤ 256 |
| 2 | 2 | `height` | pixels, ≤ 256 |
| 4 | 1 | `encoding` | 0 `RAW`, 1 `RLE`, 2 `INDEX` |
| 5 | 1 | `palette_size` | `INDEX` only; **0 means 256 colours** |
| 6 | 2 | `reserved` | 0 |
| 8 | 4 | `offset` | from the start of the payload area |
| 12 | 4 | `size` | payload bytes for this frame |

**Frames are stored largest-first.** `qac_decode()` takes a preferred
size and walks the table for the smallest frame that is at least as large
as the request, falling back to the largest available. Storing them in
descending order means that search is a single forward scan.

---

## Encodings

### 0 — `QAC_RAW`

`width × height` pixels, 4 bytes each, in **BGRA** order, row-major from
the top-left. No compression. Used when RLE would make the frame larger,
which happens for photographic or heavily dithered content.

### 1 — `QAC_RLE`

A sequence of 5-byte runs:

```
┌───────┬───┬───┬───┬───┐
│ count │ B │ G │ R │ A │
└───────┴───┴───┴───┴───┘
```

`count` is 1–255; a count of 0 is invalid. Runs are emitted in row-major
order and may cross row boundaries. The decoder stops when
`width × height` pixels have been produced and treats an overrun as
corruption.

This is the default for the bundled icons, which are flat-shaded and
compress to roughly a quarter of their raw size.

### 2 — `QAC_INDEX`

A palette of `palette_size` BGRA colours (4 bytes each, and remember that
`palette_size == 0` means 256), followed by one byte per pixel indexing
into it. Chosen automatically by the builder when an image has few enough
distinct colours that 1 byte/pixel plus the palette beats RLE.

---

## Validation

`qac_probe()` and `qac_decode()` reject an image if:

- the buffer is shorter than 32 bytes, or the magic or version is wrong;
- `frame_count` is 0 or exceeds 8;
- the frame table extends past the end of the buffer;
- `payload_size` disagrees with the remaining buffer length;
- any frame's `offset + size` exceeds `payload_size`;
- any dimension is 0 or exceeds 256;
- an `INDEX` frame's palette does not fit within its payload;
- decoding produces more or fewer pixels than `width × height`;
- the payload checksum does not match.

---

## The icon registry

At boot, `qac_init()` scans `/usr/share/icons/*.qac`, decodes each at
32 px, and keys the result on the file's basename. Applications reference
icons by that name:

```c
const struct qac_image *icon = qac_get("terminal");
if (icon) {
    qac_draw_scaled(icon, x, y, 16);
}
```

`qac_draw()` composites using the alpha channel via `fb_blend_pixel()`.
`qac_draw_scaled()` nearest-neighbour scales to an arbitrary size, which
is how one 32 px frame serves the 16 px title bar.

The twelve bundled icons — `browser`, `calculator`, `clock`, `editor`,
`files`, `help`, `monitor`, `network`, `package`, `qira`, `settings` and
`terminal` — total about 20 KB.

---

## Working with QAC files

From a shell inside the OS:

```
ush> qac list                    # every registered icon and its size
ush> qac info /usr/share/icons/terminal.qac
```

From the host:

```sh
# Rebuild the bundled icon set
python3 tools/mkqac.py --all --output rootfs/usr/share/icons

# Inspect a file
python3 tools/mkqac.py --info rootfs/usr/share/icons/terminal.qac
```

The builder defines its icons as ASCII art with a colour key, which keeps
them diffable in version control — a binary blob in a Git history is a
blob nobody can review.

---

## Limitations

- Maximum 256 × 256 pixels and 8 frames per file.
- No animation. The frame table stores *sizes*, not time steps.
- No colour management; values are treated as sRGB.
- RLE never spans a colour change, so noisy images are better stored raw.
  The builder picks the smallest of the three encodings automatically.
