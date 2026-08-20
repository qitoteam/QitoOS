# Drivers and hardware support

What QitoOS actually talks to, how the device layer is organised, and
how to add a driver.

---

## Supported hardware

### Input

| Device | Driver | Notes |
| --- | --- | --- |
| PS/2 keyboard | [`drivers/keyboard.c`](../src/kernel/drivers/keyboard.c) | scancode set 1, modifiers, key repeat, LEDs |
| PS/2 mouse | [`drivers/mouse.c`](../src/kernel/drivers/mouse.c) | 3-button, scroll wheel via the IntelliMouse protocol |

Both hang off the 8042 controller on IRQ 1 and IRQ 12. USB HID is **not**
supported; USB keyboards work only through BIOS legacy emulation, which
most virtual machines provide.

### Display

| Device | Driver | Notes |
| --- | --- | --- |
| VBE linear framebuffer | [`gfx/fb.c`](../src/kernel/gfx/fb.c) | 1024 × 768 × 32 by default, set by stage 2 |
| VGA text mode | [`gfx/console.c`](../src/kernel/gfx/console.c) | fallback when no VBE mode is available |

There is no 2D acceleration and no mode switching after boot — the mode
is chosen by the bootloader via VBE and stays fixed. All drawing is
software into a back buffer that is then copied to the framebuffer.

### Storage

| Device | Driver | Notes |
| --- | --- | --- |
| ATAPI CD-ROM | BIOS INT 13h | during boot only, via bounce buffer 0x10000 |
| RAM disk | QitoFS image at 0x2000000 | initial root filesystem |
| AHCI SATA | `drivers/ahci.c` | **New in 0.4.0 Nova:** detects PCI class 01:06, BAR5 ABAR, command list, FIS, READ DMA EXT 0x25, WRITE DMA EXT 0x35, PRDT, hot-plug, QEMU compatible, provides persistent storage via `/user/persist/` |

QitoOS 0.3.0 booted entirely from ramdisk and ran entirely in RAM – writes were volatile. **0.4.0 Nova adds AHCI/SATA driver + persistent filesystem**: `src/kernel/drivers/ahci.c` detects AHCI controller, initializes ports (CLB, FB, FRE, ST, CR, FR), reads/writes sectors via DMA, and `src/kernel/fs/persist.c` provides persistence layer (`/user/persist/`), snapshots and rollback (`persist_snapshot`, `persist_rollback`), AHCI backing when available. This unblocks `qtpkg` persistence and game world saves.

### Network

| Device | Driver | Notes |
| --- | --- | --- |
| NE2000 (PCI) | [`net/ne2000.c`](../src/kernel/net/ne2000.c) | programmed I/O, interrupt-driven receive |

The e1000 is enumerated by the PCI layer but not driven. Configure the
NE2000 in a VM as `ne2k_pci`.

### Timers and firmware

| Device | Driver | Notes |
| --- | --- | --- |
| PIT (8253) | [`time/time.c`](../src/kernel/time/time.c) | 100 Hz scheduler tick, calibration |
| RTC | [`time/time.c`](../src/kernel/time/time.c) | wall-clock time, Unix epoch conversion |
| TSC | [`arch/x86_64/cpu.c`](../src/kernel/arch/x86_64/cpu.c) + [`time/hpet.c`](../src/kernel/time/hpet.c) | high-resolution timing, RNG seeding, monotonic clock |
| 8259A PIC | [`arch/x86_64/pic.c`](../src/kernel/arch/x86_64/pic.c) | remapped to vectors 32–47 |
| APIC | [`arch/x86_64/apic.c`](../src/kernel/arch/x86_64/apic.c) | **New in 0.4.0:** Local APIC at 0xFEE00000, MSR 0x1B base, ID, EOI, spurious, timer, provides high-res timing |
| HPET | [`time/hpet.c`](../src/kernel/time/hpet.c) | **New in 0.4.0:** HPET at 0xFED00000, cap, period, freq, counter, ticks_to_ns/us, monotonic_ns/us/ms, udelay/ndelay, frame_pacer 60fps |
| ACPI | [`sys/power.c`](../src/kernel/sys/power.c) | RSDP discovery, shutdown and reset only |

**0.4.0 Nova:** Switch to APIC/HPET for high-resolution timing and frame pacing – games need stable frame times, 100 Hz PIT tick isn't enough. `frame_pacer_init(target_fps)` and `frame_pacer_wait()` provide stable 60fps pacing.

No SMP yet – QitoOS runs on one core, but APIC is initialized.

### Audio

[`drivers/audio.c`](../src/kernel/drivers/audio.c) – **extended in 0.4.0**:

- PC speaker via PIT channel 2: tones and beeps (startup chime C5,E5,G5,C6)
- **PCM audio (AC'97/SB16):** `pcm_init` detects Intel AC'97 8086:2415/266E or SB16 1102:0002, 8 channels, 44.1kHz, mixing, WAV playback
- `pcm_play_wav` parses RIFF/WAVE fmt/data chunks, `pcm_play_pcm`, `pcm_stop`, `pcm_set_volume`, `pcm_load_wav`, `pcm_active_channels`
- Needed for any game (Minecraft needs WAV)

Old: Sound Blaster 16 enumeration existed but no PCM playback – now PCM playback implemented.

### Buses

[`drivers/pci.c`](../src/kernel/drivers/pci.c) enumerates PCI via the
configuration-space mechanism at ports `0xCF8`/`0xCFC`, reads BARs and
interrupt lines, and identifies known vendor and device IDs. `hwinfo`
prints the inventory.

---

## How the device layer fits together

```
  applications
       │
  ┌────▼──────────────────────────────┐
  │ VFS  /dev  /proc                  │   fs/vfs.c, fs/devfs.c, fs/procfs.c
  ├───────────────────────────────────┤
  │ subsystems                        │   dev/input.c, net/net.c, gfx/fb.c
  ├───────────────────────────────────┤
  │ drivers                           │   keyboard, mouse, ne2000, serial…
  ├───────────────────────────────────┤
  │ arch: ports, IRQs, PCI            │   arch/x86_64/, drivers/pci.c
  └───────────────────────────────────┘
```

Drivers do not talk to applications directly. A driver publishes events
or data to a subsystem, and the subsystem exposes them — through the VFS,
through `/dev`, or through a C API.

### The input subsystem

[`dev/input.c`](../src/kernel/dev/input.c) is a worked example. The
keyboard and mouse drivers handle their own IRQs and decode their own
protocols, then push normalised events into a shared queue. The desktop
pulls from that queue and neither knows nor cares which hardware produced
an event. Adding a USB HID driver later means writing a decoder that
pushes the same event type — nothing above the queue changes.

### Device files

`devfs` presents drivers as files under `/dev`:

| Path | Backed by |
| --- | --- |
| `/dev/console` | the framebuffer or VGA console |
| `/dev/serial0` | COM1 |
| `/dev/null`, `/dev/zero` | trivial pseudo-devices |
| `/dev/random`, `/dev/urandom` | the kernel RNG |
| `/dev/fb0` | the framebuffer |

> `/dev/random` and `/dev/urandom` are the **same** generator: a
> xoshiro256\*\* PRNG seeded from the TSC and the RTC. It is fast and
> statistically decent. It is **not** cryptographically secure and must
> not be used to generate keys.

### Process and kernel state

`procfs` exposes read-only text under `/proc`: `meminfo`, `cpuinfo`,
`uptime`, `loadavg`, `version`, and a directory per task.

---

## Interrupts

Vectors 32–47 are the PIC's IRQ lines. A driver registers a handler:

```c
#include <kernel/idt.h>

static void my_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    /* acknowledge the device, queue the work */
}

irq_register(IRQ_LINE, my_irq_handler);
```

Rules that matter:

- **Keep handlers short.** They run with interrupts disabled. Acknowledge
  the device, move the data into a queue, and return.
- **Do not allocate from the heap in a handler.** The heap is not
  reentrant.
- **Acknowledge the device before returning**, or the IRQ will not fire
  again.

The IRQ dispatcher sends the end-of-interrupt to the PIC. The scheduler
runs its stack-guard check on the return path from every interrupt.

---

## Writing a driver

1. **Create the file** in `src/kernel/drivers/` (or `src/kernel/net/` for
   a NIC). The build picks up new `.c` files automatically — there is no
   file list to edit.

2. **Write an init function** that probes for the hardware and returns
   cleanly if it is absent. A missing device must never be fatal.

   ```c
   void mydev_init(void)
   {
       struct pci_device *dev = pci_find(VENDOR_ID, DEVICE_ID);
       if (!dev) {
           KLOG_DEBUG("mydev", "not present");
           return;
       }
       /* map BARs, reset the device, register the IRQ */
       KLOG_INFO("mydev", "initialised at 0x%x", dev->bar[0]);
   }
   ```

3. **Declare it** in a header under `src/kernel/include/kernel/`.

4. **Call it from `kmain()`** in phase 5, after `pci_init()`.

5. **Publish through a subsystem** rather than exposing the driver
   directly — the input queue, the network stack, or a devfs node.

6. **Add a self-test** in `selftest` if the device can be exercised
   without one being present, and extend `hwinfo` so the device shows up
   in the inventory.

### Conventions

- Port I/O goes through the `inb`/`outb` family in
  [`include/kernel/io.h`](../src/kernel/include/kernel/io.h).
- Log with `KLOG_INFO` for one-line initialisation messages, `KLOG_DEBUG`
  for detail, `KLOG_WARN` for recoverable trouble. Use a short, lowercase
  subsystem tag.
- Never busy-wait without a timeout. A device that never responds must
  not hang the boot.

---

## Verifying hardware support

```
qcsh> hwinfo        # full PCI and device inventory
qcsh> irqinfo       # interrupt counts per vector — a driver that never
                    # fires shows zero here
qcsh> diag          # subsystem health
qcsh> selftest      # 21 in-kernel assertions
qcsh> netinfo       # interfaces, addresses and packet counters
```

`irqinfo` is the fastest way to tell a driver that is not working from a
device that is not present.

---

## What is new in 0.4.0 Nova vs 0.3.0

- **AHCI/SATA + persistence** – was "no persistent storage", now AHCI driver + `/user/persist/` + snapshots/rollback
- **APIC/HPET** – was "no APIC or HPET", now APIC at 0xFEE00000 and HPET at 0xFED00000 with monotonic clock and frame pacing
- **PCM audio** – was "no PCM", now AC'97/SB16 detection, 8 channels, WAV playback
- **TLS 1.2** – `src/kernel/net/tls.c` stub with honest https error, API for AES-GCM, SHA-256, RSA/ECDHE, X.509 ready
- **QTX/QDL/QTI/qtpkg/qasm/qcc** – new native formats and package manager, see `docs/QTX.md`, `QDL.md`, `QTI.md`, `QTPKG.md`, `sdk/`
- **Ring3 execution** – QTX runs as genuine user processes with fault isolation, GDT 0x23/0x1B, TSS rsp0, IST
- **SHA-256 + Ed25519** – `src/kernel/lib/sha256.c` real SHA-256, `ed25519.c` API, `qtpkg_verify_*`
- **Snapshot/rollback** – `persist.c`
- **Software 3D rasterizer** – `src/kernel/gfx/gfx3d.c` depth buffer, perspective-correct textured triangles
- **UTF-8/Unicode** – `src/kernel/gfx/unicode.c` UTF-8 decode, glyph cache, Latin-1/Greek/Cyrillic/box-drawing
- **Demand paging/mmap/page cache** – `src/kernel/mm/mmap.c`

## Known gaps (still)

- No SMP; one core only (APIC present but not used for SMP)
- No USB
- No full TLS 1.2 crypto yet (stub with honest error, plain HTTP mirrors work)
- No NVMe (AHCI only)
- No power management beyond ACPI shutdown/reset
- e1000 enumerated but not driven (NE2000 is driven)
- No ASLR yet

