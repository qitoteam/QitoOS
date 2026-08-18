/*
 * Qira OS - 16550 UART driver
 *
 * The serial port is the kernel's most dependable output channel: it works
 * before the framebuffer is up and it is trivially captured by emulators, so
 * the log subsystem mirrors everything here.
 */

#include <kernel/serial.h>
#include <kernel/io.h>

/* Register offsets from the port base. */
#define REG_DATA        0   /* DLAB=0: RX/TX buffer      */
#define REG_IER         1   /* DLAB=0: interrupt enable  */
#define REG_DLL         0   /* DLAB=1: divisor low       */
#define REG_DLM         1   /* DLAB=1: divisor high      */
#define REG_FCR         2   /* write: FIFO control       */
#define REG_IIR         2   /* read: interrupt id        */
#define REG_LCR         3   /* line control              */
#define REG_MCR         4   /* modem control             */
#define REG_LSR         5   /* line status               */
#define REG_MSR         6   /* modem status              */
#define REG_SCRATCH     7

#define LSR_DATA_READY  0x01
#define LSR_THR_EMPTY   0x20

static uint16_t serial_port;
static bool_t   serial_ok;

/*
 * Probe a UART by round-tripping a byte through its scratch register.
 */
static bool_t uart_present(uint16_t port)
{
    outb(port + REG_SCRATCH, 0xA5);
    if (inb(port + REG_SCRATCH) != 0xA5) {
        return false;
    }
    outb(port + REG_SCRATCH, 0x5A);
    return inb(port + REG_SCRATCH) == 0x5A;
}

void serial_init(void)
{
    static const uint16_t candidates[] = {COM1, COM2, COM3, COM4};

    serial_ok   = false;
    serial_port = COM1;

    for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
        uint16_t port = candidates[i];

        outb(port + REG_IER, 0x00);        /* mask all interrupts          */
        outb(port + REG_LCR, 0x80);        /* enable DLAB                  */
        outb(port + REG_DLL, 0x01);        /* 115200 baud (divisor 1)      */
        outb(port + REG_DLM, 0x00);
        outb(port + REG_LCR, 0x03);        /* 8 data bits, no parity, 1 stop */
        outb(port + REG_FCR, 0xC7);        /* enable + clear FIFOs, 14B trigger */
        outb(port + REG_MCR, 0x0B);        /* DTR | RTS | OUT2             */

        if (!uart_present(port)) {
            continue;
        }

        /* Loopback self-test: write a byte and expect it straight back. */
        outb(port + REG_MCR, 0x1E);
        outb(port + REG_DATA, 0xAE);
        if (inb(port + REG_DATA) != 0xAE) {
            continue;
        }
        outb(port + REG_MCR, 0x0B);

        serial_port = port;
        serial_ok   = true;
        return;
    }

    /*
     * No UART responded. Emulators without a serial device still tolerate the
     * writes, so keep COM1 configured and carry on with output disabled.
     */
    outb(COM1 + REG_LCR, 0x03);
    outb(COM1 + REG_MCR, 0x0B);
}

bool_t serial_available(void)
{
    return serial_ok;
}

static void wait_tx_ready(void)
{
    /* Bounded spin so a dead UART can never wedge the kernel. */
    for (int i = 0; i < 100000; i++) {
        if (inb(serial_port + REG_LSR) & LSR_THR_EMPTY) {
            return;
        }
    }
}

void serial_putc(char c)
{
    if (c == '\n') {
        wait_tx_ready();
        outb(serial_port + REG_DATA, '\r');
    }
    wait_tx_ready();
    outb(serial_port + REG_DATA, (uint8_t)c);
}

void serial_write(const char *s)
{
    while (*s) {
        serial_putc(*s++);
    }
}

void serial_write_len(const char *s, size_t len)
{
    while (len--) {
        serial_putc(*s++);
    }
}

int serial_getc_nonblock(void)
{
    if (!(inb(serial_port + REG_LSR) & LSR_DATA_READY)) {
        return -1;
    }
    return inb(serial_port + REG_DATA);
}
