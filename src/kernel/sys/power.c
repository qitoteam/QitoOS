/*
 * QitoOS - power management
 *
 * Reboot and shutdown paths. Full ACPI is not implemented; instead the
 * well-known emulator shutdown ports are tried in turn, falling back to
 * halting the processor.
 */

#include <kernel/power.h>
#include <kernel/io.h>
#include <kernel/log.h>
#include <kernel/time.h>

void power_init(void)
{
    KLOG_INFO("power", "power management ready (reboot and shutdown available)");
}

void power_idle(void)
{
    cpu_halt();
}

NORETURN void power_reboot(void)
{
    KLOG_INFO("power", "restarting the system");
    time_sleep_ms(100);

    cpu_cli();

    /* 1. Pulse the CPU reset line through the 8042 keyboard controller. */
    for (int attempt = 0; attempt < 100; attempt++) {
        uint8_t status = inb(0x64);
        if (!(status & 0x02)) {
            outb(0x64, 0xFE);
            break;
        }
        io_wait();
    }
    time_sleep_ms(50);

    /* 2. The PCI reset control register. */
    outb(0xCF9, 0x02);
    io_wait();
    outb(0xCF9, 0x06);
    time_sleep_ms(50);

    /*
     * 3. Force a triple fault by loading a null IDT and raising an interrupt.
     *    Every x86 implementation resets in response.
     */
    struct {
        uint16_t limit;
        uint64_t base;
    } PACKED null_idt = {0, 0};

    __asm__ volatile("lidt %0" : : "m"(null_idt));
    __asm__ volatile("int3");

    for (;;) {
        cpu_halt();
    }
}

NORETURN void power_shutdown(void)
{
    KLOG_INFO("power", "powering off");
    time_sleep_ms(100);

    cpu_cli();

    /* QEMU (since 2.0) and Bochs ACPI shutdown port. */
    outw(0x604, 0x2000);
    io_wait();

    /* Older QEMU / Bochs APM port. */
    outw(0xB004, 0x2000);
    io_wait();

    /* VirtualBox. */
    outw(0x4004, 0x3400);
    io_wait();

    /* Cloud Hypervisor / some firmware. */
    outw(0x600, 0x34);
    io_wait();

    KLOG_WARN("power", "no shutdown mechanism responded; halting the CPU");
    KLOG_INFO("power", "it is now safe to close the emulator");

    for (;;) {
        cpu_cli();
        cpu_halt();
    }
}
