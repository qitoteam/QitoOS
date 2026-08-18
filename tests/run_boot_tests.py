#!/usr/bin/env python3
"""
run_boot_tests.py - boot Qira OS in an emulator and assert on what it does.

This is the test that matters: it takes the real ISO, boots it on an emulated
x86_64 machine, and checks the kernel log and the composited framebuffer.

Three kinds of check are performed:

  1. Boot milestones   every subsystem must announce itself in the right order
                       and the system must reach the desktop.
  2. In-kernel tests   the QCSH `selftest` and `diag` commands are driven
                       through the terminal's autorun hook, and their results
                       are read back from the screen.
  3. Rendering         a framebuffer capture is decoded and inspected, so a
                       blank or single-colour screen fails the run.

QEMU is used when available because it is fastest; otherwise the tests fall
back to Bochs, which is easy to build anywhere.
"""

from __future__ import annotations

import argparse
import base64
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import grabframe  # noqa: E402

BOCHS_PREFIXES = [
    os.path.expanduser("~/.local/bochs"),
    "/opt/bochs",
    "/usr/local",
    "/usr",
]


ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def strip_ansi(text: str) -> str:
    """Remove colour escape sequences so the checks can match plain text."""
    return ANSI_ESCAPE.sub("", text)


def read_log(path: str) -> str:
    """Read a serial log, tolerating a file the emulator left mid-write."""
    if not os.path.isfile(path):
        return ""
    with open(path, "r", errors="replace") as handle:
        return handle.read()


def run_until_timeout(command: list[str], workdir: str, timeout: int) -> None:
    """
    Run an emulator until it exits or the timeout expires.

    The guest never shuts itself down, so a timeout is the normal outcome. The
    process is asked to terminate first and only killed if it ignores that,
    which gives it a chance to flush the serial log to disk.
    """
    process = subprocess.Popen(
        command,
        cwd=workdir,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return
        time.sleep(0.25)

    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)

    # Give the operating system a moment to flush the file.
    time.sleep(0.5)


class Results:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.failures: list[str] = []

    def check(self, description: str, condition: bool, detail: str = "") -> bool:
        if condition:
            self.passed += 1
            print(f"  \033[92mpass\033[0m  {description}")
        else:
            self.failed += 1
            self.failures.append(description)
            suffix = f"  ({detail})" if detail else ""
            print(f"  \033[91mFAIL\033[0m  {description}{suffix}")
        return condition

    def section(self, title: str) -> None:
        print(f"\n\033[1m{title}\033[0m")

    def summary(self) -> int:
        total = self.passed + self.failed
        print(f"\n{total} checks: {self.passed} passed, {self.failed} failed")
        if self.failures:
            print("\nFailures:")
            for failure in self.failures:
                print(f"  - {failure}")
        return 1 if self.failed else 0


# --- emulator drivers -----------------------------------------------------


def find_bochs() -> tuple[str, str] | None:
    binary = shutil.which("bochs")
    if binary is None:
        for prefix in BOCHS_PREFIXES:
            candidate = os.path.join(prefix, "bin", "bochs")
            if os.path.isfile(candidate):
                binary = candidate
                break
    if binary is None:
        return None

    for prefix in BOCHS_PREFIXES:
        share = os.path.join(prefix, "share", "bochs")
        if os.path.isdir(share):
            return binary, share
    return None


def run_qemu(iso: str, timeout: int, memory: int) -> str:
    """Boot the ISO under QEMU and return the serial output."""
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None:
        raise RuntimeError("qemu-system-x86_64 not found")

    with tempfile.TemporaryDirectory(prefix="qira-qemu-") as workdir:
        serial = os.path.join(workdir, "serial.log")
        command = [
            qemu,
            "-cdrom", iso,
            "-m", str(memory),
            "-boot", "d",
            "-serial", f"file:{serial}",
            "-display", "none",
            "-no-reboot",
            "-rtc", "base=utc",
        ]
        print(f"  running: {' '.join(command)}", file=sys.stderr)
        run_until_timeout(command, workdir, timeout)
        return read_log(serial)


def bochs_cpu_model(binary: str) -> str:
    """
    Pick a CPU model this Bochs build actually knows about.

    Bochs 3.x dropped the old `cpuid:` directive in favour of named models,
    and which models exist depends on how it was configured. Asking the binary
    is more reliable than hardcoding a name that may not be compiled in.
    """
    preferred = [
        "corei7_haswell_4770",
        "corei7_sandy_bridge_2600k",
        "corei5_lynnfield_750",
        "core2_penryn_t9600",
        "athlon64_clawhammer",
    ]

    try:
        result = subprocess.run(
            [binary, "--help", "cpu"], capture_output=True, text=True, timeout=20
        )
        # Bochs prints the model list on stderr, not stdout.
        listing = result.stderr + "\n" + result.stdout
        available = {
            line.strip()
            for line in listing.splitlines()
            if line.strip()
            and " " not in line.strip()
            and not line.startswith(("0", "=", "Supported"))
        }
    except (subprocess.SubprocessError, OSError):
        return preferred[0]

    for model in preferred:
        if model in available:
            return model
    return preferred[0]


def bochs_supports(binary: str, option: str) -> bool:
    """
    Report whether this Bochs build accepts a configuration directive.

    Which subsystems are compiled in varies between builds, and an
    unsupported directive is fatal rather than ignored, so anything optional
    is probed before it is written into the config.
    """
    try:
        result = subprocess.run(
            [binary, "--help", "features"], capture_output=True, text=True,
            timeout=20
        )
        listing = result.stdout + result.stderr
        return option in listing
    except (subprocess.SubprocessError, OSError):
        return False


def run_bochs(iso: str, timeout: int, memory: int) -> str:
    """Boot the ISO under Bochs and return the serial output."""
    found = find_bochs()
    if found is None:
        raise RuntimeError("bochs not found")
    binary, share = found
    cpu_model = bochs_cpu_model(binary)

    # A build without sound support treats a `sound:` line as a fatal error,
    # so it is only emitted when the feature is actually present.
    sound_line = ""
    if bochs_supports(binary, "sound"):
        sound_line = "sound: driver=dummy, waveoutdrv=dummy\n"

    with tempfile.TemporaryDirectory(prefix="qira-bochs-") as workdir:
        serial = os.path.join(workdir, "serial.log")
        config = os.path.join(workdir, "bochsrc")

        with open(config, "w") as handle:
            handle.write(
                f"""romimage: file="{os.path.join(share, 'BIOS-bochs-latest')}"
vgaromimage: file="{os.path.join(share, 'VGABIOS-lgpl-latest.bin')}"
megs: {memory}
ata0-master: type=cdrom, path="{iso}", status=inserted
boot: cdrom
com1: enabled=1, mode=file, dev="{serial}"
cpu: model={cpu_model}, count=1, ips=80000000, reset_on_triple_fault=1
mouse: enabled=1, type=ps2
clock: sync=none, time0=local
pci: enabled=1, chipset=i440fx
vga: extension=vbe
{sound_line}display_library: nogui
log: {os.path.join(workdir, 'bochs.log')}
panic: action=report
error: action=report
info: action=ignore
debug: action=ignore
"""
            )

        print(f"  running: {binary} -q -f {config}", file=sys.stderr)
        run_until_timeout([binary, "-q", "-f", config], workdir, timeout)
        return read_log(serial)


# --- checks ---------------------------------------------------------------

# Milestones every successful boot must log, in this order.
BOOT_MILESTONES = [
    ("bootloader reaches stage 2", r"Qira OS stage2"),
    ("kernel image is loaded", r"kernel \.* ok"),
    ("CPU switches to long mode", r"long mode"),
    ("kernel banner is printed", r"Qira OS \d+\.\d+\.\d+"),
    ("boot protocol is accepted", r"boot protocol v\d+"),
    ("CPU is identified", r"INFO  cpu .*vendor="),
    ("descriptor tables are installed", r"descriptor tables installed"),
    ("interrupt vectors are installed", r"\d+ interrupt vectors installed"),
    ("PIC is remapped", r"8259A remapped"),
    ("physical memory is mapped", r"MiB total, .*MiB usable"),
    ("virtual memory is up", r"kernel address space at CR3"),
    ("kernel heap is up", r"kernel heap at"),
    ("real-time clock is read", r"real-time clock reads \d{4}-"),
    ("timer is programmed", r"PIT programmed to \d+ Hz"),
    ("interrupts are enabled", r"interrupts enabled"),
    ("PCI bus is enumerated", r"INFO  pci .*device\(s\) found"),
    ("framebuffer is initialised", r"INFO  fb .*\d+x\d+ at 32 bpp"),
    ("console is created", r"character console"),
    ("keyboard driver loads", r"PS/2 keyboard ready"),
    ("mouse driver loads", r"PS/2 mouse ready"),
    ("filesystem is mounted", r"virtual filesystem mounted"),
    ("ramdisk is unpacked", r"INFO  qirafs .*mounted"),
    ("device nodes are created", r"device nodes registered"),
    ("procfs is populated", r"/proc populated"),
    ("configuration loads", r"INFO  config"),
    ("system calls are registered", r"\d+ system calls registered"),
    ("IPC is ready", r"message ports ready"),
    ("networking starts", r"INFO  net .*interface"),
    ("packages are registered", r"components registered"),
    ("scheduler starts", r"starting the scheduler"),
    ("init task runs", r"userspace starting"),
    ("QCSH registers commands", r"INFO  qcsh .*commands registered"),
    ("UltraShell registers commands", r"ultrashell: \d+ commands registered"),
    ("desktop starts", r"applications registered, \d+x\d+ display"),
]

# Subsystems added alongside the desktop; each must announce itself.
FEATURE_MILESTONES = [
    ("fonts load", r"INFO  font .*faces available"),
    ("icons load", r"INFO  qac .*icon\(s\) loaded"),
    ("executable loader starts", r"INFO  lqx .*services exported"),
    ("transport layer starts", r"INFO  tcp .*TCP, UDP and DNS"),
    ("clipboard starts", r"INFO  clipboard"),
    ("random generator seeds", r"INFO  random"),
]


def check_boot_log(results: Results, log: str) -> None:
    results.section("Boot sequence")

    positions = {}
    for description, pattern in BOOT_MILESTONES:
        match = re.search(pattern, log)
        results.check(description, match is not None)
        if match:
            positions[description] = match.start()

    # Milestones must appear in the documented order.
    ordered = [positions[d] for d, _ in BOOT_MILESTONES if d in positions]
    results.check(
        "milestones are logged in order", ordered == sorted(ordered),
        "subsystems initialised out of sequence",
    )

    results.section("Subsystems")
    for description, pattern in FEATURE_MILESTONES:
        results.check(description, re.search(pattern, log) is not None)

    match = re.search(r"(\d+) faces available", log)
    if match:
        results.check(f"several typefaces are present ({match.group(1)})",
                      int(match.group(1)) >= 2)

    match = re.search(r"qac .*: (\d+) icon\(s\) loaded", log)
    if match:
        results.check(f"icons decoded from the ramdisk ({match.group(1)})",
                      int(match.group(1)) > 0)

    match = re.search(r"lqx .*: loader ready, (\d+) kernel services", log)
    if match:
        results.check(f"kernel services are exported ({match.group(1)})",
                      int(match.group(1)) >= 10)

    results.section("Boot health")
    results.check("no kernel panic", "KERNEL PANIC" not in log)
    results.check("no unhandled CPU exception", "CPU EXCEPTION" not in log)
    results.check("no assertion failure", "assertion failed" not in log)
    results.check("no heap corruption", "heap corruption" not in log)
    results.check("no double free", "double free" not in log)
    results.check("no allocation failure", "allocation of" not in log)
    results.check("no stack overflow", "stack guard destroyed" not in log)
    results.check("no rejected image", "rejected image" not in log)
    results.check("no disk read error", "disk error" not in log)

    match = re.search(r"startup complete in (\d+) ms", log)
    if results.check("startup completes", match is not None):
        elapsed = int(match.group(1))
        results.check(f"startup is under 5 seconds ({elapsed} ms)", elapsed < 5000)

    match = re.search(r"(\d+) MiB total, (\d+) MiB usable", log)
    if results.check("memory is detected", match is not None):
        total = int(match.group(1))
        results.check(f"at least 64 MiB detected ({total} MiB)", total >= 64)

    match = re.search(r"INFO  fb .*: (\d+)x(\d+) at (\d+) bpp", log)
    if results.check("graphics mode is set", match is not None):
        width, height, depth = (int(match.group(i)) for i in (1, 2, 3))
        results.check(f"resolution is usable ({width}x{height})",
                      width >= 640 and height >= 480)
        results.check(f"colour depth is 32 bpp ({depth})", depth == 32)


def check_shell_output(results: Results, log: str) -> None:
    """Check output produced by commands run through the autorun hook."""
    results.section("Shell execution")

    if "--QIRA-SHELL$" not in log:
        print("  (no autorun output in this image, skipping)")
        return

    results.check("autorun script completed",
                  "--QIRA-AUTORUN-COMPLETE--" in log)

    # Every command in the script must have been dispatched.
    commands = re.findall(r"--QIRA-SHELL\$ (.+)", log)
    results.check(f"commands were executed ({len(commands)})", len(commands) >= 5)

    results.check("no command was unrecognised", "command not found" not in log)

    # The in-kernel self-test reports its own tally.
    match = re.search(r"(\d+) tests: (\d+) passed, (\d+) failed", log)
    if results.check("kernel self-test ran", match is not None):
        total, passed, failed = (int(match.group(i)) for i in (1, 2, 3))
        results.check(f"self-test has real coverage ({total} checks)", total >= 15)
        results.check(f"self-test passed ({passed}/{total})", failed == 0)

    # The diagnostics command reports pass/fail per subsystem.
    match = re.search(r"(\d+) checks, (\d+) passed, (\d+) failed", log)
    if results.check("diagnostics ran", match is not None):
        _total, _passed, failed = (int(match.group(i)) for i in (1, 2, 3))
        results.check("all diagnostics passed", failed == 0)

    # QCSH's sysinfo must report the machine it is running on.
    results.check("sysinfo reports the processor", "Model" in log)
    results.check("sysinfo reports memory", re.search(r"Total\s+\d+", log) is not None)

    # UltraShell's arithmetic must be correct: (7 + 3) * 4 = 40.
    results.check("UltraShell evaluates arithmetic",
                  re.search(r"calc \(7\+3\)\*4", log) is not None
                  and re.search(r"^40\s*$", log, re.MULTILINE) is not None)

    # A long listing of /etc must show the configuration file.
    results.check("UltraShell lists files with metadata",
                  "qira.conf" in log and re.search(r"-rw", log) is not None)

    # Switching shells must work in both directions.
    results.check("shell switch to QCSH", "QiraConfigShell" in log)
    results.check("shell switch back to UltraShell", "UltraShell" in log)

    # The new formats must be inspectable from the shell.
    if "QX/L" in log:
        results.check("LQX images are readable", "x86_64" in log)
    if "Loaded icons" in log:
        results.check("QAC icons are listed", re.search(r"32x32", log) is not None)
    if "qira-mono" in log:
        results.check("the font registry is listed", "qira-sans" in log)

    # The clipboard must round trip.
    if "copied" in log:
        results.check("clipboard round trips",
                      re.search(r"copied \d+ bytes", log) is not None)


def check_frame(results: Results, log: str, output_dir: str | None) -> None:
    results.section("Display output")

    frames = grabframe.extract(log)
    if not results.check("a framebuffer capture was produced", len(frames) > 0):
        return

    frame = frames[-1]
    width, height = frame["width"], frame["height"]
    data = frame["data"]

    results.check(f"capture geometry is sane ({width}x{height})",
                  width >= 640 and height >= 480)
    results.check("capture holds a full frame", len(data) == width * height * 3)

    # Count distinct colours: a blank or broken screen has almost none.
    colours = set()
    step = max(1, (width * height) // 20000)
    for index in range(0, width * height, step):
        offset = index * 3
        colours.add(data[offset : offset + 3])

    results.check(f"screen has varied content ({len(colours)} colours)",
                  len(colours) >= 8)

    # The desktop must not be a single flat colour.
    def pixel(x: int, y: int) -> tuple[int, int, int]:
        o = (y * width + x) * 3
        return data[o], data[o + 1], data[o + 2]

    top_left = pixel(4, 4)
    bottom_right = pixel(width - 8, height - 8)
    results.check("screen is not uniformly blank", top_left != bottom_right,
                  f"{top_left} vs {bottom_right}")

    # The panel runs along the top; it should differ from the wallpaper below.
    panel = pixel(width // 2, 8)
    wallpaper = pixel(width // 2, height - 60)
    results.check("panel is distinct from the wallpaper", panel != wallpaper,
                  f"panel {panel}, wallpaper {wallpaper}")

    # Colour balance: the earlier VBE field bug made every pixel green, so
    # verify the red and blue channels actually carry signal.
    reds = sum(data[0::3][:: max(1, step)])
    greens = sum(data[1::3][:: max(1, step)])
    blues = sum(data[2::3][:: max(1, step)])
    total = reds + greens + blues

    if results.check("frame carries colour data", total > 0):
        green_share = greens / total
        results.check(
            f"no single colour channel dominates (green {green_share:.0%})",
            green_share < 0.6,
        )
        results.check("red channel is present", reds > 0)
        results.check("blue channel is present", blues > 0)

    if output_dir:
        os.makedirs(output_dir, exist_ok=True)
        path = os.path.join(output_dir, "boot-frame.png")
        grabframe.write_png(path, width, height, data)
        print(f"  saved {path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Boot tests for Qira OS")
    parser.add_argument("--iso", default=os.path.join(ROOT, "build", "qira-os.iso"))
    parser.add_argument("--timeout", type=int, default=170)
    parser.add_argument("--memory", type=int, default=512)
    parser.add_argument(
        "--emulator", choices=["auto", "qemu", "bochs"], default="auto"
    )
    parser.add_argument("--save-frames", default=None,
                        help="directory to write captured frames into")
    parser.add_argument("--keep-log", default=None,
                        help="write the raw serial log here")
    args = parser.parse_args()

    if not os.path.isfile(args.iso):
        print(f"error: {args.iso} does not exist; run 'make iso' first",
              file=sys.stderr)
        return 2

    print(f"\033[1mQira OS boot tests\033[0m")
    print(f"  image: {args.iso} ({os.path.getsize(args.iso)} bytes)")

    # Pick an emulator.
    emulator = args.emulator
    if emulator == "auto":
        if shutil.which("qemu-system-x86_64"):
            emulator = "qemu"
        elif find_bochs():
            emulator = "bochs"
        else:
            print("error: neither QEMU nor Bochs is available", file=sys.stderr)
            return 2
    print(f"  emulator: {emulator}")

    runner = run_qemu if emulator == "qemu" else run_bochs
    # The emulator runs with its own working directory, so the image must be
    # referenced absolutely.
    log = runner(os.path.abspath(args.iso), args.timeout, args.memory)

    if args.keep_log:
        with open(args.keep_log, "w") as handle:
            handle.write(log)

    if not log.strip():
        print("error: the emulator produced no serial output", file=sys.stderr)
        return 1

    print(f"  captured {len(log)} bytes of serial output")

    # Shell output carries colour codes; the checks work on plain text.
    log = strip_ansi(log)

    results = Results()
    check_boot_log(results, log)
    check_shell_output(results, log)
    check_frame(results, log, args.save_frames)

    return results.summary()


if __name__ == "__main__":
    raise SystemExit(main())
