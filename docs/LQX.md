# LQX — Linked Qito Executables (Legacy – now QTX)

> **Legacy – now QTX.** Since QitoOS 0.4a Alpha, LQX has been renamed to QTX (Qito eXecutable): on-disk magic stays `QX`, format byte `L` → `X`, extension `.qtx`, `.qtx` only produced by `qcc`/`qasm`, `tools/mklqx.py` deleted. See `docs/QTX.md` and `docs/QDL.md`.

`.lqx` was native executable format of QitoOS 0.3.0 and earlier.

- Signature `QX` + format byte `L` → now `X` for QTX, `D` for QDL
- Extension `.lqx` → `.qtx` / `.qdl`
- Header 88B `<2sBBHHIIQQIIIIIIII24s`, section 36B, import/symbol 32B, W^X, checksum
- Builder `tools/mklqx.py` → deleted, replaced by `tools/qasm.py` and `tools/qcc.py`

See `QTX.md` for current.
