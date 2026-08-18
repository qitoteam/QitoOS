#!/usr/bin/env python3
"""
capture_screenshots.py - produce the screenshots used in the documentation.

Boots Qira OS repeatedly, each time with a kernel command line that opens a
different application and captures the frame, then decodes the captures into
PNG files under docs/screenshots/.

Because the screenshots come from a real boot of the real ISO, they cannot
drift away from what the system actually looks like.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))

import grabframe  # noqa: E402
from run_boot_tests import run_bochs, run_qemu, find_bochs, strip_ansi  # noqa: E402
import shutil  # noqa: E402

# Each entry drives one boot: a name, the autorun script, and when to capture.
SCENES = [
    (
        "desktop",
        "Desktop with the application menu open",
        "",
        6000,
    ),
    (
        "terminal-ultrashell",
        "UltraShell running in the terminal",
        "autorun=version;ls_-l_/etc;calc_(7+3)*4;cat_/etc/motd",
        9000,
    ),
    (
        "terminal-qcsh",
        "QCSH reporting system information",
        "autorun=qcsh;sysinfo",
        10000,
    ),
    (
        "selftest",
        "The in-kernel self-test suite",
        "autorun=qcsh;selftest",
        11000,
    ),
    (
        "diagnostics",
        "System diagnostics and hardware inventory",
        "autorun=qcsh;diag;hwinfo",
        11000,
    ),
    (
        "pipeline",
        "Pipelines and redirection in UltraShell",
        "autorun=cat_/proc/meminfo_|_grep_Heap;echo_hello_>_/tmp/a.txt;cat_/tmp/a.txt;ls_/etc_|_wc_-l;seq_5_|_tail_-2",
        10000,
    ),
    (
        "fonts",
        "The selectable font registry",
        "autorun=fonts;fonts_set_terminal_qira-mono-bold;version",
        10000,
    ),
    (
        "formats",
        "QAC icons and LQX executables",
        "autorun=qac_list;lqx_info_/bin/hello.lqx",
        10000,
    ),
]


def build_iso(cmdline: str, output: str) -> None:
    """Build an ISO with a specific kernel command line."""
    build = os.path.join(ROOT, "build")
    subprocess.run(
        [
            sys.executable,
            os.path.join(ROOT, "tools", "mkiso.py"),
            "--boot", os.path.join(build, "boot.bin"),
            "--kernel", os.path.join(build, "qira-kernel.bin"),
            "--ramdisk", os.path.join(build, "qirafs.img"),
            "--output", output,
            "--cmdline", cmdline,
        ],
        check=True,
        capture_output=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture Qira OS screenshots")
    parser.add_argument("--output", default=os.path.join(ROOT, "docs", "screenshots"))
    parser.add_argument("--timeout", type=int, default=150)
    parser.add_argument("--only", default=None, help="capture just one scene")
    parser.add_argument("--iso", default=None, help="unused, kept for the Makefile")
    args = parser.parse_args()

    os.makedirs(args.output, exist_ok=True)

    runner = run_qemu if shutil.which("qemu-system-x86_64") else run_bochs
    if runner is run_bochs and not find_bochs():
        print("error: no emulator available", file=sys.stderr)
        return 2

    build = os.path.join(ROOT, "build")
    os.makedirs(build, exist_ok=True)

    written = 0
    for name, description, autorun, capture_ms in SCENES:
        if args.only and args.only != name:
            continue

        print(f"\n=== {name}: {description}")

        cmdline = f"root=qirafs console=fb {autorun} capture={capture_ms}".strip()
        iso = os.path.join(build, f"shot-{name}.iso")
        build_iso(cmdline, iso)

        log = strip_ansi(runner(iso, args.timeout, 512))
        frames = grabframe.extract(log)

        if not frames:
            print(f"  warning: no frame captured for {name}", file=sys.stderr)
            continue

        frame = frames[-1]
        path = os.path.join(args.output, f"{name}.png")
        grabframe.write_png(path, frame["width"], frame["height"], frame["data"])
        print(f"  wrote {path} ({frame['width']}x{frame['height']})")
        written += 1

        os.remove(iso)

    print(f"\n{written} screenshot(s) captured in {args.output}")
    return 0 if written else 1


if __name__ == "__main__":
    raise SystemExit(main())
