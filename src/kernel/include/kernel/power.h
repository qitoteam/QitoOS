/*
 * Qira OS - power management
 */
#ifndef QIRA_POWER_H
#define QIRA_POWER_H

#include <kernel/types.h>

void power_init(void);

/* Restart the machine. Tries the keyboard controller, then a triple fault. */
NORETURN void power_reboot(void);

/*
 * Power the machine off. Uses the shutdown ports that QEMU, Bochs and
 * VirtualBox recognise; on real hardware without ACPI support this halts.
 */
NORETURN void power_shutdown(void);

/* Halt the CPU until the next interrupt; used by the idle loop. */
void power_idle(void);

#endif /* QIRA_POWER_H */
