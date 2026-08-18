# Drivers and hardware support

What Qira OS actually talks to, how the device layer is organised, and
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

| Device | Notes |
| --- | --- |
| ATAPI CD-ROM | via BIOS INT 13h during boot only |
| RAM disk | the QiraFS image loaded at `0x2000000` |

**There is no runtime disk driver.** Qira OS boots entirely from the
ramdisk and runs entirely in RAM. Writes to the filesystem are real but
volatile — they do not survive a reboot. A native AHCI driver is on the
roadmap.

### Network

| Device | Driver | Notes |
| --- | --- | --- |
| NE2000 (PCI) | [`net/ne2000.c`](../src/kernel/net/ne2000.c) | programmed I/O, interrupt-driven receive |

The e1000 is enumerated by the PCI layer but not driven. Configure the
NE2000 in a VM as `ne2k_pci`.

### Timers and firmware

| Device | Driver | Notes |
| --- | --- | --- |
| PIT (8253) | [`time/time.c`](../src/kernel/time/time.c) | 100 Hz scheduler tick |
| RTC | [`time/time.c`](../src/kernel/time/time.c) | wall-clock time, Unix epoch conversion |
| TSC | [`arch/x86_64/cpu.c`](../src/kernel/arch/x86_64/cpu.c) | high-resolution timing, RNG seeding |
| 8259A PIC | [`arch/x86_64/pic.c`](../src/kernel/arch/x86_64/pic.c) | remapped to vectors 32–47 |
| ACPI | [`sys/power.c`](../src/kernel/sys/power.c) | RSDP discovery, shutdown and reset only |

The APIC and HPET are not used; interrupts go through the legacy PIC.
There is no SMP support — Qira OS runs on one core.

### Audio

[`drivers/audio.c`](../src/kernel/drivers/audio.c) drives the PC speaker
through PIT channel 2: tones and simple beeps. Sound Blaster 16
enumeration exists but no PCM playback is implemented.

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

## Known gaps

- No SMP; one core only.
- No APIC or HPET.
- No AHCI or NVMe, so no persistent storage.
- No USB.
- No PCM audio.
- No power management beyond ACPI shutdown and reset.
- The e1000 is enumerated but not driven.
