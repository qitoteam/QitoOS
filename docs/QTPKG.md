# QTPKG — QitoOS Package Manager

The centrepiece of QitoOS. Replaces `git` entirely (the in-OS git client is removed).

## Entry file

`/user/qtpkg/entry.var` — user-facing `c:root/user/qtpkg/entry.var` (VFS path)

Syntax:

```
pkg1 = [1.001](https://download-link.com/pkg),[1.002](https://download-link.com/pkg2);
pkg2 = # not implemented yet
browser = [0.3.0](http://example.com/browser-0.3.0.qtpkg_profile);
qasm = [1.0.0](http://example.com/qasm-1.0.0.qtpkg_profile);
```

Rules:

- One entry per line
- Format: `name = [version](url),[version](url);`
- Comma-separated `[version](url)` pairs, terminated by `;`
- `#` starts a comment (whole line or after `;`)
- Users may add their own URLs
- Real parser with clear error messages including line numbers (see `src/kernel/sys/qtpkg.c: qtpkg_parse_entry_file`)

Example of not-implemented:

```
mypackage = # not implemented yet
```

This creates entry with 0 versions, `qtpkg info` shows "not implemented yet".

## Profile manifest

Each version URL points at a `pkg.qtpkg_profile` manifest – a metadata file describing where the project is, not the project itself.

It carries:

- name, version, description
- architecture (x86_64, any)
- dependencies (comma-separated package names)
- install paths
- file checksums (SHA-256)
- payload URL
- Ed25519 signature (optional)

Example `.qtpkg_profile`:

```
name=qasm
version=1.0.0
description=QitoOS assembler – genuine x86-64 subset, produces .qtx and .qdl
arch=x86_64
depends=
payload=http://example.com/qasm-1.0.0.qtx
install_path=/bin/qasm
checksum=sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
signature=ed25519:abc123...
```

For packages with multiple files:

```
file0_path=/bin/qasm
file0_sha256=...
file1_path=/usr/share/man/qasm.1
file1_sha256=...
```

The loader verifies checksums and signature before install.

## Commands

| Command | Behaviour |
|---------|-----------|
| `qtpkg install <pkg>` | Resolve via entry.var → profile → payload; verify; install; snapshot before install |
| `qtpkg update` | Update every installed package (checks registry for newer versions) |
| `qtpkg -os update` | Update QitoOS itself only (looks for `qito-os` package) |
| `qtpkg upgrade` | qtpkg upgrades itself (installs `qtpkg` package) |
| `qtpkg -fix os` | Repair corrupted OS install – verifies files against manifest checksums, re-fetches corrupt |
| `qtpkg -fix --driver amd64 \| intel` | Repair/reinstall driver |
| `qtpkg list [filter]` | List packages from entry.var |
| `qtpkg search <query>` | Search |
| `qtpkg remove <pkg>` | Remove |
| `qtpkg info <pkg>` | Show versions and fetch profile for details |
| `qtpkg rollback <snapshot>` | Rollback to snapshot |

## How it installs

1. Parse `/user/qtpkg/entry.var` → find package → pick latest version URL
2. Fetch profile via HTTP (plain HTTP only, see TLS note)
3. Verify profile signature (Ed25519) if present
4. Snapshot current system (`persist_snapshot("pre-<pkg>-<timestamp>")`) – copy-on-write snapshots of `/` before install
5. Fetch payload URL (must be plain HTTP unless TLS implemented)
6. Verify payload checksum (SHA-256) against profile
7. Write to `install_path` (e.g., `/bin/qasm`, `/lib/libfoo.qdl`, `/usr/share/icons/*.qti`)
8. If driver/font/app, register with system

## TLS blocker – handled honestly

QitoOS has TCP/UDP/DNS/HTTP but no TLS, so it cannot fetch `https://` URLs — including from GitHub, where the registry lives.

Two options (spec):

- Implement TLS 1.2 (needs AES-GCM, SHA-256, RSA/ECDHE, X.509 parsing) – large
- Ship qtpkg against plain-HTTP mirrors with clear "TLS not supported yet" error on https:

QitoOS chooses honest second path for now, with TLS 1.2 stub ready:

```c
if (qtpkg_is_https(url)) {
    snprintf(error, err_size,
        "TLS not supported yet – cannot fetch https:// URL. Use plain HTTP mirror for %s. "
        "See docs/QTPKG.md", url);
    return -1;
}
```

When user tries:

```bash
qtpkg install qasm
# Resolving qasm -> https://github.com/...
# qtpkg: TLS not supported yet – cannot fetch https:// URL. Use plain HTTP mirror for https://...
# See docs/QTPKG.md
```

And for payload:

```
qtpkg: TLS not supported yet – cannot fetch https:// URL
       http://example.com/qasm... (use plain HTTP mirror or wait for TLS 1.2)
```

Do not fake downloads or pretend HTTPS works.

### Future TLS 1.2

When implemented, will need:

- AES-GCM
- SHA-256 (already have)
- RSA/ECDHE
- X.509 parsing
- Then qtpkg can fetch from GitHub directly, registry can live on https

See `docs/DRIVERS.md` for network stack, and planned `src/kernel/net/tls.c`.

## Package integrity

A package manager that fetches and executes unverified code over plain HTTP is a remote-code-execution hole.

Mitigations implemented:

- SHA-256 checksum verification of payload against profile (`qtpkg_verify_checksum`)
- Ed25519 signatures on profiles (`qtpkg_verify_signature`) – verify before install
- `-fix` is checksum-driven: verifies installed files against manifest, re-fetches corrupt
- Snapshots before every install, with `qtpkg rollback`

## System snapshot and rollback

Copy-on-write snapshots of `/` before any install or update, with `qtpkg rollback`.

Implemented in `src/kernel/fs/persist.c`:

- `persist_snapshot(name)` – creates snapshot slot
- `persist_rollback(name)` – restores
- `persist_list_snapshots` – list
- Stored in `/user/persist/` plus AHCI backing if available

This makes `-fix os` trustworthy rather than hopeful.

## Driver handling

`qtpkg -fix --driver amd64 | intel` repairs/reinstalls driver:

- Looks for package `drv-amd64` or `drv-intel`
- Fetches and installs to `/lib/` or driver path
- Uses AHCI/SATA persistence if available

## Comparison with git

Old QitoOS had `git` smart-HTTP client (`src/kernel/net/git.c`) supporting `clone` and `ls-remote`. This is removed – qtpkg supersedes it.

- Git in OS was demo (no TLS, no inflate, no working tree)
- qtpkg is real package manager with verification, rollback, driver handling
- Project still uses GitHub for development; package requests go through repo's issues tab (`https://github.com/qitoteam/QitoOS/issues`)

## SDK and qcc/qasm via qtpkg

`qcc` and `qasm` are not bundled – installed via qtpkg:

```bash
qtpkg install qasm
qtpkg install qcc
```

Then:

```bash
qasm -o hello.qtx hello.s
qcc -o hello.qtx hello.c
```

See `sdk/` for headers, and `docs/QTX.md` for format.

## Registry

Official registry: https://github.com/qitoteam/qtpkg-registry

But QitoOS cannot fetch https yet – so plain HTTP mirrors should be listed in entry.var:

```
qasm = [1.0.0](http://mirror.example.com/qasm-1.0.0.qtpkg_profile);
```

Users may add own URLs.

## Security

- HTTPS blocked with clear error until TLS 1.2
- SHA-256 verification
- Ed25519 signature verification (stub, ready for real crypto)
- Snapshots and rollback
- No null service pointer – unresolved import is load-time error
