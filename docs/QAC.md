# QAC — Qito Application iCon (Legacy – now QTI)

> **Legacy – now QTI.** Since QitoOS 0.4a Alpha, QAC replaced by QTI (QiTo Icons): QTI stores real pixels not ASCII art, 5 sizes 16/32/64/128/256 default 64, magic QTI1, encodings RAW/RLE/INDEX. Old generator drew from char grids; QTI stores actual pixel data. See `docs/QTI.md`.

`.qac` was icon format in 0.3.0 and earlier.

- Magic `QACI` → now `QTI1`
- Extension `.qac` → `.qti`
- Builder `tools/mkqac.py` → `tools/mkqti.py` accepts real PNG/RGBA

See `QTI.md` for current.
