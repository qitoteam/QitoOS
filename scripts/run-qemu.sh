#!/usr/bin/env bash
#
# run-qemu.sh - boot QitoOS in QEMU with sensible defaults.
#
# Usage:
#   scripts/run-qemu.sh [options]
#
# Options:
#   -i, --iso PATH      image to boot (default: build/qito-os.iso)
#   -m, --memory MB     guest memory in MiB (default: 512)
#   -n, --nographic     run headless with the serial console on the terminal
#   -d, --debug         wait for a gdb connection on port 1234
#   -s, --serial FILE   write the serial log to FILE instead of stdout
#       --network       attach an NE2000 network card
#       --kvm           enable hardware acceleration if it is available
#   -h, --help          show this message

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ISO="$ROOT/build/qito-os.iso"
MEMORY=512
NOGRAPHIC=0
DEBUG=0
SERIAL=""
NETWORK=0
KVM=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -i|--iso)       ISO="$2"; shift 2 ;;
        -m|--memory)    MEMORY="$2"; shift 2 ;;
        -n|--nographic) NOGRAPHIC=1; shift ;;
        -d|--debug)     DEBUG=1; shift ;;
        -s|--serial)    SERIAL="$2"; shift 2 ;;
        --network)      NETWORK=1; shift ;;
        --kvm)          KVM=1; shift ;;
        -h|--help)      sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)              echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "error: qemu-system-x86_64 is not installed" >&2
    echo "  Debian/Ubuntu: sudo apt install qemu-system-x86" >&2
    echo "  Fedora:        sudo dnf install qemu-system-x86" >&2
    echo "  macOS:         brew install qemu" >&2
    exit 1
fi

if [[ ! -f "$ISO" ]]; then
    echo "error: $ISO does not exist" >&2
    echo "  run 'make iso' first" >&2
    exit 1
fi

ARGS=(
    -cdrom "$ISO"
    -m "$MEMORY"
    -boot d
    -no-reboot
    -rtc base=utc
)

if [[ $NOGRAPHIC -eq 1 ]]; then
    ARGS+=(-nographic)
elif [[ -n "$SERIAL" ]]; then
    ARGS+=(-serial "file:$SERIAL")
else
    ARGS+=(-serial stdio)
fi

if [[ $NETWORK -eq 1 ]]; then
    # Qito drives the NE2000; user-mode networking needs no host privileges.
    ARGS+=(-netdev user,id=net0 -device ne2k_pci,netdev=net0)
fi

if [[ $DEBUG -eq 1 ]]; then
    ARGS+=(-s -S)
    echo "Waiting for a debugger on tcp:1234."
    echo "Connect with:"
    echo "  gdb build/qito-kernel.elf -ex 'target remote :1234'"
fi

if [[ $KVM -eq 1 ]] && [[ -w /dev/kvm ]]; then
    ARGS+=(-enable-kvm -cpu host)
    echo "Hardware acceleration enabled."
fi

echo "Booting $ISO with ${MEMORY} MiB..."
exec qemu-system-x86_64 "${ARGS[@]}"
