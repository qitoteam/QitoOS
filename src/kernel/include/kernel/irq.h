/*
 * Qira OS - interrupt controller and IRQ dispatch
 */
#ifndef QIRA_IRQ_H
#define QIRA_IRQ_H

#include <kernel/types.h>
#include <kernel/cpu.h>

/* Hardware IRQs are remapped to vectors 32-47. */
#define IRQ_BASE_VECTOR 32
#define IRQ_COUNT       16

#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1
#define IRQ_CASCADE  2
#define IRQ_COM2     3
#define IRQ_COM1     4
#define IRQ_LPT2     5
#define IRQ_FLOPPY   6
#define IRQ_LPT1     7
#define IRQ_RTC      8
#define IRQ_FREE1    9
#define IRQ_FREE2    10
#define IRQ_FREE3    11
#define IRQ_MOUSE    12
#define IRQ_FPU      13
#define IRQ_ATA1     14
#define IRQ_ATA2     15

/* Syscall vector used by the legacy `int 0x80` path. */
#define SYSCALL_VECTOR 0x80

typedef void (*irq_handler_fn)(struct interrupt_frame *frame, void *context);

void pic_init(void);
void pic_mask_irq(uint8_t irq);
void pic_unmask_irq(uint8_t irq);
void pic_send_eoi(uint8_t irq);
void pic_disable(void);
uint16_t pic_get_mask(void);

int  irq_register(uint8_t irq, irq_handler_fn handler, void *context,
                  const char *name);
void irq_unregister(uint8_t irq);

/* Install a handler on a raw interrupt vector (0-255). */
int  interrupt_register(uint8_t vector, irq_handler_fn handler, void *context,
                        const char *name);

/* Statistics for diagnostics. */
uint64_t irq_get_count(uint8_t irq);
const char *irq_get_name(uint8_t irq);
uint64_t interrupt_total_count(void);

#endif /* QIRA_IRQ_H */
