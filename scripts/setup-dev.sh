#!/usr/bin/env bash
#
# setup-dev.sh - install everything needed to build and run Qira OS.
#
# Qira OS deliberately keeps its dependencies small: a hosted GCC/binutils
# toolchain and Python 3 are enough to build the ISO, because the project
# ships its own ISO 9660 writer and filesystem packer rather than relying on
# xorriso, grub-mkrescue or mtools.
#
# An emulator is only needed to run the result.

set -euo pipefail

echo "Qira OS development environment setup"
echo

detect_platform() {
    if [[ "$(uname -s)" == "Darwin" ]]; then
        echo "macos"
    elif command -v apt-get >/dev/null 2>&1; then
        echo "debian"
    elif command -v dnf >/dev/null 2>&1; then
        echo "fedora"
    elif command -v pacman >/dev/null 2>&1; then
        echo "arch"
    elif command -v zypper >/dev/null 2>&1; then
        echo "suse"
    else
        echo "unknown"
    fi
}

PLATFORM="$(detect_platform)"
echo "Detected platform: $PLATFORM"
echo

case "$PLATFORM" in
    debian)
        echo "Installing packages with apt..."
        sudo apt-get update
        sudo apt-get install -y \
            build-essential binutils gcc make python3 qemu-system-x86 gdb
        ;;
    fedora)
        echo "Installing packages with dnf..."
        sudo dnf install -y \
            gcc binutils make python3 qemu-system-x86 gdb
        ;;
    arch)
        echo "Installing packages with pacman..."
        sudo pacman -S --needed --noconfirm \
            base-devel binutils gcc make python qemu-system-x86 gdb
        ;;
    suse)
        echo "Installing packages with zypper..."
        sudo zypper install -y \
            gcc binutils make python3 qemu-x86 gdb
        ;;
    macos)
        echo "Installing packages with Homebrew..."
        if ! command -v brew >/dev/null 2>&1; then
            echo "error: Homebrew is required; see https://brew.sh" >&2
            exit 1
        fi
        brew install x86_64-elf-gcc x86_64-elf-binutils qemu python3
        echo
        echo "Note: on macOS the build uses the x86_64-elf-* cross toolchain,"
        echo "which the Makefile picks up automatically."
        ;;
    *)
        echo "Unrecognised platform. Install these manually:" >&2
        echo "  - GCC and GNU binutils targeting x86_64" >&2
        echo "  - GNU make" >&2
        echo "  - Python 3.8 or newer" >&2
        echo "  - QEMU (qemu-system-x86_64) to run the result" >&2
        exit 1
        ;;
esac

echo
echo "Verifying the toolchain..."

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

make check-tools
echo

if command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "emulator: $(qemu-system-x86_64 --version | head -1)"
else
    echo "warning: QEMU was not found; you can build but not run the ISO"
fi

echo
echo "Setup complete. Next steps:"
echo "  make            build the bootable ISO"
echo "  make run        boot it in QEMU"
echo "  make test       run the unit tests and the boot tests"
