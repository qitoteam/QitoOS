/*
 * QitoOS - APIC (Advanced Programmable Interrupt Controller)
 */

#ifndef QITO_APIC_H
#define QITO_APIC_H

#include <kernel/types.h>

void apic_init(void);
bool_t apic_available(void);
void apic_eoi(void);
uint64_t apic_ticks(void);
void apic_set_timer(uint64_t us);

#endif
