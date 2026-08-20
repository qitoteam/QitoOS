/*
 * QitoOS - Interrupt Descriptor Table and the central interrupt dispatcher
 *
 * All 256 vectors are populated with assembly stubs from isr.S. The dispatcher
 * routes CPU exceptions to the fault handler, hardware IRQs to registered
 * device handlers, and the syscall vector into the system call layer.
 */

#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/io.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;          /* bits 0-2 select an IST stack, rest zero */
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} PACKED;

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} PACKED;

#define IDT_ENTRIES 256

#define GATE_INTERRUPT 0x0E   /* interrupts disabled on entry */
#define GATE_TRAP      0x0F   /* interrupts stay enabled      */
#define GATE_PRESENT   0x80
#define GATE_RING3     0x60

static struct idt_entry   idt[IDT_ENTRIES] ALIGNED(16);
static struct idt_pointer idt_ptr;

/* Stub addresses exported by isr.S. */
extern uint64_t isr_stub_table[IDT_ENTRIES];

struct handler_slot {
    irq_handler_fn handler;
    void          *context;
    const char    *name;
    uint64_t       count;
};

static struct handler_slot handlers[IDT_ENTRIES];
static uint64_t            total_interrupts;

static const char *exception_name(uint64_t vector)
{
    static const char *names[32] = {
        "divide error",
        "debug",
        "non-maskable interrupt",
        "breakpoint",
        "overflow",
        "bound range exceeded",
        "invalid opcode",
        "device not available",
        "double fault",
        "coprocessor segment overrun",
        "invalid TSS",
        "segment not present",
        "stack-segment fault",
        "general protection fault",
        "page fault",
        "reserved",
        "x87 floating-point exception",
        "alignment check",
        "machine check",
        "SIMD floating-point exception",
        "virtualization exception",
        "control protection exception",
        "reserved", "reserved", "reserved", "reserved",
        "reserved", "reserved",
        "hypervisor injection exception",
        "VMM communication exception",
        "security exception",
        "reserved",
    };
    return (vector < 32) ? names[vector] : "external interrupt";
}

static void idt_set_gate(int vector, uint64_t handler, uint8_t type,
                         uint8_t dpl, uint8_t ist)
{
    idt[vector].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[vector].selector    = SEL_KERNEL_CODE;
    idt[vector].ist         = ist & 0x07;
    idt[vector].type_attr   = (uint8_t)(GATE_PRESENT | (dpl == RING3 ? GATE_RING3 : 0) |
                                        type);
    idt[vector].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)(handler >> 32);
    idt[vector].reserved    = 0;
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    memset(handlers, 0, sizeof(handlers));

    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, isr_stub_table[i], GATE_INTERRUPT, RING0, 0);
    }

    /*
     * Faults that can occur with a corrupt or missing stack run on their own
     * IST stacks so the handler is always able to execute.
     */
    idt_set_gate(2, isr_stub_table[2], GATE_INTERRUPT, RING0, 2);   /* NMI  */
    idt_set_gate(8, isr_stub_table[8], GATE_INTERRUPT, RING0, 1);   /* #DF  */
    idt_set_gate(14, isr_stub_table[14], GATE_INTERRUPT, RING0, 3); /* #PF  */

    /* Breakpoint and overflow are usable from user mode. */
    idt_set_gate(3, isr_stub_table[3], GATE_TRAP, RING3, 0);
    idt_set_gate(4, isr_stub_table[4], GATE_TRAP, RING3, 0);

    /* The legacy syscall gate must be callable from ring 3. */
    idt_set_gate(SYSCALL_VECTOR, isr_stub_table[SYSCALL_VECTOR], GATE_INTERRUPT,
                 RING3, 0);

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)idt;
    __asm__ volatile("lidt %0" : : "m"(idt_ptr) : "memory");

    KLOG_INFO("idt", "%d interrupt vectors installed", IDT_ENTRIES);
}

int interrupt_register(uint8_t vector, irq_handler_fn handler, void *context,
                       const char *name)
{
    handlers[vector].handler = handler;
    handlers[vector].context = context;
    handlers[vector].name    = name;
    return 0;
}

int irq_register(uint8_t irq, irq_handler_fn handler, void *context,
                 const char *name)
{
    if (irq >= IRQ_COUNT) {
        return -1;
    }
    interrupt_register((uint8_t)(IRQ_BASE_VECTOR + irq), handler, context, name);
    pic_unmask_irq(irq);
    KLOG_DEBUG("irq", "IRQ %u -> %s", irq, name ? name : "?");
    return 0;
}

void irq_unregister(uint8_t irq)
{
    if (irq >= IRQ_COUNT) {
        return;
    }
    pic_mask_irq(irq);
    handlers[IRQ_BASE_VECTOR + irq].handler = NULL;
}

uint64_t irq_get_count(uint8_t irq)
{
    if (irq >= IRQ_COUNT) {
        return 0;
    }
    return handlers[IRQ_BASE_VECTOR + irq].count;
}

const char *irq_get_name(uint8_t irq)
{
    if (irq >= IRQ_COUNT || !handlers[IRQ_BASE_VECTOR + irq].name) {
        return "-";
    }
    return handlers[IRQ_BASE_VECTOR + irq].name;
}

uint64_t interrupt_total_count(void)
{
    return total_interrupts;
}

void cpu_dump_frame(const struct interrupt_frame *f)
{
    kprintf("  RIP=%016llx  CS =%04llx  RFLAGS=%016llx\n",
            (unsigned long long)f->rip, (unsigned long long)f->cs,
            (unsigned long long)f->rflags);
    kprintf("  RSP=%016llx  SS =%04llx  ERR   =%016llx\n",
            (unsigned long long)f->rsp, (unsigned long long)f->ss,
            (unsigned long long)f->error_code);
    kprintf("  RAX=%016llx RBX=%016llx RCX=%016llx\n",
            (unsigned long long)f->rax, (unsigned long long)f->rbx,
            (unsigned long long)f->rcx);
    kprintf("  RDX=%016llx RSI=%016llx RDI=%016llx\n",
            (unsigned long long)f->rdx, (unsigned long long)f->rsi,
            (unsigned long long)f->rdi);
    kprintf("  RBP=%016llx R8 =%016llx R9 =%016llx\n",
            (unsigned long long)f->rbp, (unsigned long long)f->r8,
            (unsigned long long)f->r9);
    kprintf("  R10=%016llx R11=%016llx R12=%016llx\n",
            (unsigned long long)f->r10, (unsigned long long)f->r11,
            (unsigned long long)f->r12);
    kprintf("  R13=%016llx R14=%016llx R15=%016llx\n",
            (unsigned long long)f->r13, (unsigned long long)f->r14,
            (unsigned long long)f->r15);
    kprintf("  CR2=%016llx CR3=%016llx\n", (unsigned long long)read_cr2(),
            (unsigned long long)read_cr3());
}

/*
 * Handle a CPU exception. Faults taken in user mode terminate the offending
 * task; faults in kernel mode are fatal.
 */
static void handle_exception(struct interrupt_frame *frame)
{
    bool_t from_user = (frame->cs & 3) == RING3;

    if (frame->vector == 14) {
        /* Give the VM layer a chance to service a demand-paged address. */
        extern int vmm_handle_page_fault(uint64_t addr, uint64_t error_code);
        if (vmm_handle_page_fault(read_cr2(), frame->error_code) == 0) {
            return;
        }
    }

    if (from_user) {
        KLOG_ERR("fault", "%s (vector %llu, err %llx) in task %d at rip=%llx",
                 exception_name(frame->vector),
                 (unsigned long long)frame->vector,
                 (unsigned long long)frame->error_code, sched_current_pid(),
                 (unsigned long long)frame->rip);
        if (frame->vector == 14) {
            KLOG_ERR("fault", "  faulting address %llx",
                     (unsigned long long)read_cr2());
        }
        sched_kill_current(128 + (int)frame->vector);
        return;
    }

    kprintf("\n*** CPU EXCEPTION: %s (vector %llu) ***\n",
            exception_name(frame->vector), (unsigned long long)frame->vector);
    cpu_dump_frame(frame);
    panic("unhandled kernel exception %llu (%s)",
          (unsigned long long)frame->vector, exception_name(frame->vector));
}

/*
 * The single entry point called by every assembly stub.
 *
 * Returns the frame to restore, which allows the scheduler to perform a
 * context switch simply by handing back a different task's saved frame.
 */
struct interrupt_frame *interrupt_dispatch(struct interrupt_frame *frame)
{
    uint64_t vector = frame->vector;

    total_interrupts++;
    handlers[vector].count++;

    if (vector < 32) {
        handle_exception(frame);
    } else if (vector == SYSCALL_VECTOR) {
        syscall_dispatch(frame);
    } else if (vector >= IRQ_BASE_VECTOR && vector < IRQ_BASE_VECTOR + IRQ_COUNT) {
        uint8_t irq = (uint8_t)(vector - IRQ_BASE_VECTOR);

        /*
         * Spurious IRQ 7/15 are raised by the PIC itself and must not be
         * acknowledged if the corresponding in-service bit is clear.
         */
        if (handlers[vector].handler) {
            handlers[vector].handler(frame, handlers[vector].context);
        }
        pic_send_eoi(irq);
    } else if (handlers[vector].handler) {
        handlers[vector].handler(frame, handlers[vector].context);
    }

    /* The scheduler decides whether another task should run now. */
    return sched_on_interrupt_return(frame);
}
