# qtpkg-registry – QitoOS Package Registry

Official registry for QitoOS packages: https://github.com/qitoteam/qtpkg-registry

This repo contains package manifests (`.qtpkg_profile`) and payloads (`.qtx`, `.qdl`, `.qti`).

QitoOS itself cannot fetch `https://` URLs (no TLS yet) – it reports clear error "TLS not supported yet – use plain HTTP mirror". For now, use plain HTTP mirrors or download manually. This registry is hosted on GitHub (https) but qtpkg inside QitoOS will give honest TLS error; future TLS 1.2 will unblock HTTPS.

## Structure

```
packages/
  <pkg-name>/
    <version>/
      <pkg>.qtpkg_profile  – manifest with name, version, description, arch, depends, payload, install_path, checksum, signature
      <payload>            – .qtx, .qdl, .qti, etc
```

Example entry.var line in QitoOS:

```
qasm = [1.0.0](https://raw.githubusercontent.com/qitoteam/qtpkg-registry/main/packages/qasm/1.0.0/qasm.qtpkg_profile);
```

Each profile points at payload URL – also in this repo, e.g.:

```
payload=https://raw.githubusercontent.com/qitoteam/qtpkg-registry/main/packages/qasm/1.0.0/qasm.qtx
```

Until TLS, use plain HTTP mirror that serves same files, or install manually.

## Packages included

- **qasm** – QitoOS assembler, genuine x86-64 subset, produces .qtx/.qdl (host version in sdk/bin/qasm, QitoOS version via qtpkg)
- **qcc** – QitoOS C compiler, C subset thin driver, produces .qtx/.qdl
- **hello** – example QTX program
- **libdemo** – example QDL library
- **browser, calculator, clock, editor, files, terminal** – desktop apps as QTX (placeholders, real apps built into kernel but QTX versions for registry)
- **fonts** – qito-sans, qito-mono as QTI 5 sizes 16/32/64/128/256
- **drivers/ahci** – AHCI driver package as QDL
- **about, notes, paint, settings, sysmon** – more apps

All payloads are valid QTX/QDL with QX header 88B, format X/D, checksum, W^X enforced, built via `tools/qasm.py` and `tools/qcc.py`.

## Adding a package

1. Create folder `packages/<name>/<version>/`
2. Add payload binary (built via `sdk/bin/qcc` or `sdk/bin/qasm`)
3. Create `.qtpkg_profile` manifest:

```
name=qasm
version=1.0.0
description=QitoOS assembler – genuine x86-64 subset
arch=x86_64
depends=
payload=https://raw.githubusercontent.com/qitoteam/qtpkg-registry/main/packages/qasm/1.0.0/qasm.qtx
install_path=/bin/qasm
checksum=sha256:...
signature=ed25519:...
```

4. Update `/user/qtpkg/entry.var` in QitoOS to point at new profile URL.

## Package requests

Open an issue at https://github.com/qitoteam/QitoOS/issues – registry tracks via issues tab.

## Security

- SHA-256 checksums verified before install (`qtpkg_verify_checksum` in `src/kernel/lib/sha256.c` real implementation)
- Ed25519 signatures on profiles (`ed25519.c` API)
- `qtpkg -fix os` verifies files against manifest, re-fetches corrupt
- Snapshots before install, `qtpkg rollback`

## Folder named packages

As requested: this repo contains a folder named `packages` that has all the apps and things. Each app has its own subfolder with versioned payloads and profiles. See `entry.var` for aggregated index.

## License

Apache 2.0, same as QitoOS.
