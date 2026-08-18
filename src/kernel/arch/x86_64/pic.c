/*
 * Qira OS - 8259A Programmable Interrupt Controller
 *
 * The two cascaded PICs are remapped away from vectors 0-15 (which collide
 * with the CPU exceptions) to vectors 32-47.
 */

#include <kernel/irq.h>
#include <kernel/io.h>
#include <kernel/log.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_ICW4      0x01   /* an ICW4 will follow           */
#define ICW1_INIT      0x10   /* start initialisation sequence */
#define ICW4_8086      0x01   /* 8086/88 mode                  */

#define PIC_EOI        0x20
#define PIC_READ_IRR   0x0A
#define PIC_READ_ISR   0x0B

static uint16_t current_mask = 0xFFFF;

void pic_init(void)
{
    /* Start the initialisation sequence on both chips. */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* ICW2: vector offsets. */
    outb(PIC1_DATA, IRQ_BASE_VECTOR);
    io_wait();
    outb(PIC2_DATA, IRQ_BASE_VECTOR + 8);
    io_wait();

    /* ICW3: wire the slave to IRQ line 2 of the master. */
    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    /* ICW4: 8086 mode. */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /*
     * Mask everything except the cascade line; drivers unmask their own IRQ
     * when they register a handler.
     */
    current_mask = 0xFFFF & ~(1u << IRQ_CASCADE);
    outb(PIC1_DATA, (uint8_t)(current_mask & 0xFF));
    outb(PIC2_DATA, (uint8_t)(current_mask >> 8));

    KLOG_INFO("pic", "8259A remapped to vectors %d-%d", IRQ_BASE_VECTOR,
              IRQ_BASE_VECTOR + 15);
}

void pic_mask_irq(uint8_t irq)
{
    if (irq >= IRQ_COUNT) {
        return;
    }
    current_mask |= (uint16_t)(1u << irq);
    if (irq < 8) {
        outb(PIC1_DATA, (uint8_t)(current_mask & 0xFF));
    } else {
        outb(PIC2_DATA, (uint8_t)(current_mask >> 8));
    }
}

void pic_unmask_irq(uint8_t irq)
{
    if (irq >= IRQ_COUNT) {
        return;
    }
    current_mask &= (uint16_t)~(1u << irq);
    if (irq < 8) {
        outb(PIC1_DATA, (uint8_t)(current_mask & 0xFF));
    } else {
        /* Unmasking a slave line also requires the cascade line to be open. */
        current_mask &= (uint16_t)~(1u << IRQ_CASCADE);
        outb(PIC1_DATA, (uint8_t)(current_mask & 0xFF));
        outb(PIC2_DATA, (uint8_t)(current_mask >> 8));
    }
}

uint16_t pic_get_mask(void)
{
    return current_mask;
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

void pic_disable(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    current_mask = 0xFFFF;
}
