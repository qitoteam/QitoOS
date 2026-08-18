/*
 * Qira OS - NE2000 (RTL8029 / DP8390) network driver
 *
 * The NE2000 is the simplest widely-emulated NIC: programmed I/O, no DMA
 * descriptors, and supported by QEMU, Bochs and VirtualBox. That makes it the
 * right first network driver for a hobby OS.
 */

#include <kernel/net.h>
#include <kernel/pci.h>
#include <kernel/io.h>
#include <kernel/irq.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/time.h>

/* Register offsets from the I/O base, page 0 unless noted. */
#define NE_CR        0x00   /* command                                    */
#define NE_CLDA0     0x01
#define NE_PSTART    0x01   /* page 0 write: receive ring start           */
#define NE_PSTOP     0x02   /* page 0 write: receive ring stop            */
#define NE_BNRY      0x03   /* boundary pointer                           */
#define NE_TSR       0x04   /* page 0 read: transmit status               */
#define NE_TPSR      0x04   /* page 0 write: transmit page start          */
#define NE_TBCR0     0x05   /* transmit byte count low                    */
#define NE_TBCR1     0x06   /* transmit byte count high                   */
#define NE_ISR       0x07   /* interrupt status                           */
#define NE_RSAR0     0x08   /* remote start address low                   */
#define NE_RSAR1     0x09   /* remote start address high                  */
#define NE_RBCR0     0x0A   /* remote byte count low                      */
#define NE_RBCR1     0x0B   /* remote byte count high                     */
#define NE_RCR       0x0C   /* receive configuration                      */
#define NE_TCR       0x0D   /* transmit configuration                     */
#define NE_DCR       0x0E   /* data configuration                         */
#define NE_IMR       0x0F   /* interrupt mask                             */
#define NE_DATA      0x10   /* remote DMA data port                       */
#define NE_RESET     0x1F   /* reset port                                 */

/* Page 1 registers. */
#define NE_PAR0      0x01   /* physical address                           */
#define NE_CURR      0x07   /* current page                               */
#define NE_MAR0      0x08   /* multicast address                          */

/* Command register bits. */
#define CR_STOP      0x01
#define CR_START     0x02
#define CR_TXP       0x04
#define CR_RD_READ   0x08
#define CR_RD_WRITE  0x10
#define CR_RD_ABORT  0x20
#define CR_PAGE0     0x00
#define CR_PAGE1     0x40

/* Interrupt status bits. */
#define ISR_PRX      0x01   /* packet received                            */
#define ISR_PTX      0x02   /* packet transmitted                         */
#define ISR_RXE      0x04
#define ISR_TXE      0x08
#define ISR_OVW      0x10   /* receive ring overflow                      */
#define ISR_RST      0x80

/* Buffer ring layout, in 256-byte pages. */
#define TX_START_PAGE 0x40
#define RX_START_PAGE 0x46
#define RX_STOP_PAGE  0x80

struct ne2000_state {
    uint16_t io_base;
    uint8_t  irq;
    uint8_t  next_packet;
    struct net_interface *iface;
};

static struct ne2000_state ne2000;
static bool_t              ne2000_present;

extern struct net_interface *net_register_interface(
    const char *name, const uint8_t *mac,
    int (*transmit)(struct net_interface *, const void *, size_t),
    void *driver_data);

/* Copy `len` bytes out of the card's buffer memory at `offset`. */
static void ne_read_memory(uint16_t base, uint16_t offset, void *buffer, size_t len)
{
    outb(base + NE_CR, CR_PAGE0 | CR_RD_ABORT | CR_START);
    outb(base + NE_RBCR0, (uint8_t)(len & 0xFF));
    outb(base + NE_RBCR1, (uint8_t)(len >> 8));
    outb(base + NE_RSAR0, (uint8_t)(offset & 0xFF));
    outb(base + NE_RSAR1, (uint8_t)(offset >> 8));
    outb(base + NE_CR, CR_PAGE0 | CR_RD_READ | CR_START);

    /* The card is configured for 16-bit transfers. */
    uint16_t *out   = (uint16_t *)buffer;
    size_t    words = len / 2;
    for (size_t i = 0; i < words; i++) {
        out[i] = inw(base + NE_DATA);
    }
    if (len & 1) {
        ((uint8_t *)buffer)[len - 1] = (uint8_t)inw(base + NE_DATA);
    }
}

static void ne_write_memory(uint16_t base, uint16_t offset, const void *buffer,
                            size_t len)
{
    /* Round up to a whole word; the card transfers 16 bits at a time. */
    size_t words = (len + 1) / 2;

    outb(base + NE_CR, CR_PAGE0 | CR_RD_ABORT | CR_START);
    outb(base + NE_ISR, 0x40);   /* clear remote DMA complete */
    outb(base + NE_RBCR0, (uint8_t)((words * 2) & 0xFF));
    outb(base + NE_RBCR1, (uint8_t)((words * 2) >> 8));
    outb(base + NE_RSAR0, (uint8_t)(offset & 0xFF));
    outb(base + NE_RSAR1, (uint8_t)(offset >> 8));
    outb(base + NE_CR, CR_PAGE0 | CR_RD_WRITE | CR_START);

    const uint8_t *bytes = (const uint8_t *)buffer;
    for (size_t i = 0; i < words; i++) {
        uint16_t word = bytes[i * 2];
        if (i * 2 + 1 < len) {
            word |= (uint16_t)(bytes[i * 2 + 1] << 8);
        }
        outw(base + NE_DATA, word);
    }

    /* Wait for the DMA to complete. */
    for (int i = 0; i < 10000; i++) {
        if (inb(base + NE_ISR) & 0x40) {
            break;
        }
    }
    outb(base + NE_ISR, 0x40);
}

static int ne2000_transmit(struct net_interface *iface, const void *frame,
                           size_t len)
{
    struct ne2000_state *state = (struct ne2000_state *)iface->driver_data;
    uint16_t             base  = state->io_base;

    if (len > 1514) {
        return -1;
    }
    if (len < 60) {
        len = 60;   /* pad to the Ethernet minimum */
    }

    ne_write_memory(base, TX_START_PAGE << 8, frame, len);

    outb(base + NE_CR, CR_PAGE0 | CR_RD_ABORT | CR_START);
    outb(base + NE_TPSR, TX_START_PAGE);
    outb(base + NE_TBCR0, (uint8_t)(len & 0xFF));
    outb(base + NE_TBCR1, (uint8_t)(len >> 8));
    outb(base + NE_CR, CR_PAGE0 | CR_RD_ABORT | CR_TXP | CR_START);

    return 0;
}

/* Drain every packet waiting in the receive ring. */
static void ne2000_receive_all(struct ne2000_state *state)
{
    uint16_t base = state->io_base;

    for (int guard = 0; guard < 32; guard++) {
        outb(base + NE_CR, CR_PAGE1 | CR_RD_ABORT | CR_START);
        uint8_t current = inb(base + NE_CURR);
        outb(base + NE_CR, CR_PAGE0 | CR_RD_ABORT | CR_START);

        if (state->next_packet == current) {
            break;   /* ring is empty */
        }

        /* Each packet is preceded by a 4-byte header. */
        struct {
            uint8_t  status;
            uint8_t  next;
            uint16_t length;
        } PACKED header;

        ne_read_memory(base, (uint16_t)(state->next_packet << 8), &header,
                       sizeof(header));

        uint16_t length = header.length;
        if (length < 4 || length > 1518) {
            /* Corrupt header: resynchronise on the current page. */
            state->next_packet = current;
            outb(base + NE_BNRY,
                 (uint8_t)((current == RX_START_PAGE) ? RX_STOP_PAGE - 1
                                                      : current - 1));
            break;
        }
        length -= 4;   /* drop the CRC */

        uint8_t frame[1518];
        ne_read_memory(base, (uint16_t)((state->next_packet << 8) + 4), frame,
                       MIN(length, sizeof(frame)));

        net_receive(state->iface, frame, MIN(length, sizeof(frame)));

        state->next_packet = header.next;
        outb(base + NE_BNRY,
             (uint8_t)((header.next == RX_START_PAGE) ? RX_STOP_PAGE - 1
                                                      : header.next - 1));
    }
}

static void ne2000_irq(struct interrupt_frame *frame, void *context)
{
    UNUSED(frame);

    struct ne2000_state *state = (struct ne2000_state *)context;
    uint16_t             base  = state->io_base;

    for (int guard = 0; guard < 16; guard++) {
        uint8_t status = inb(base + NE_ISR);
        if (status == 0) {
            break;
        }

        if (status & ISR_PRX) {
            outb(base + NE_ISR, ISR_PRX);
            ne2000_receive_all(state);
        }
        if (status & ISR_PTX) {
            outb(base + NE_ISR, ISR_PTX);
        }
        if (status & ISR_OVW) {
            outb(base + NE_ISR, ISR_OVW);
            KLOG_WARN("ne2000", "receive ring overflow");
            ne2000_receive_all(state);
        }
        if (status & (ISR_RXE | ISR_TXE)) {
            outb(base + NE_ISR, (uint8_t)(status & (ISR_RXE | ISR_TXE)));
            state->iface->stats.tx_errors++;
        }
    }
}

void ne2000_init(void)
{
    /* Look for the PCI variant (RTL8029) first, then the ISA default port. */
    const struct pci_device *pci = pci_find_device(0x10EC, 0x8029);
    uint16_t base = 0x300;
    uint8_t  irq  = 9;

    if (pci) {
        base = (uint16_t)(pci->bar[0] & ~0x3u);
        irq  = pci->interrupt_line;
        KLOG_INFO("ne2000", "RTL8029 at PCI %02x:%02x.%u, I/O 0x%x, IRQ %u",
                  pci->bus, pci->slot, pci->function, base, irq);
    }

    /* Reset the card and wait for it to report completion. */
    outb(base + NE_RESET, inb(base + NE_RESET));

    bool_t reset_ok = false;
    for (int i = 0; i < 10000; i++) {
        if (inb(base + NE_ISR) & ISR_RST) {
            reset_ok = true;
            break;
        }
        io_wait();
    }
    if (!reset_ok) {
        KLOG_DEBUG("ne2000", "no NE2000 compatible card at I/O 0x%x", base);
        return;
    }
    outb(base + NE_ISR, 0xFF);

    /* Stop the card while it is configured. */
    outb(base + NE_CR, CR_PAGE0 | CR_RD_ABORT | CR_STOP);
    outb(base + NE_DCR, 0x49);   /* word transfers, normal operation */
    outb(base + NE_RBCR0, 0);
    outb(base + NE_RBCR1, 0);
    outb(base + NE_RCR, 0x20);   /* monitor mode while initialising  */
    outb(base + NE_TCR, 0x02);   /* internal loopback                */

    /* Read the MAC address out of the card's PROM. */
    uint8_t prom[32];
    ne_read_memory(base, 0, prom, sizeof(prom));

    uint8_t mac[ETH_ALEN];
    for (int i = 0; i < ETH_ALEN; i++) {
        mac[i] = prom[i * 2];   /* the PROM duplicates each byte */
    }

    /* A card that reports an all-zero or all-ones MAC is not really there. */
    bool_t plausible = false;
    for (int i = 0; i < ETH_ALEN; i++) {
        if (mac[i] != 0x00 && mac[i] != 0xFF) {
            plausible = true;
        }
    }
    if (!plausible) {
        KLOG_DEBUG("ne2000", "no card detected (MAC address looks invalid)");
        return;
    }

    /* Program the receive ring. */
    outb(base + NE_PSTART, RX_START_PAGE);
    outb(base + NE_PSTOP, RX_STOP_PAGE);
    outb(base + NE_BNRY, RX_START_PAGE);

    /* Page 1: set the station address and the current page pointer. */
    outb(base + NE_CR, CR_PAGE1 | CR_RD_ABORT | CR_STOP);
    for (int i = 0; i < ETH_ALEN; i++) {
        outb(base + NE_PAR0 + i, mac[i]);
    }
    for (int i = 0; i < 8; i++) {
        outb(base + NE_MAR0 + i, 0xFF);   /* accept all multicast */
    }
    outb(base + NE_CURR, RX_START_PAGE + 1);

    /* Back to page 0 and start the card. */
    outb(base + NE_CR, CR_PAGE0 | CR_RD_ABORT | CR_START);
    outb(base + NE_ISR, 0xFF);
    outb(base + NE_IMR, ISR_PRX | ISR_PTX | ISR_RXE | ISR_TXE | ISR_OVW);
    outb(base + NE_TCR, 0x00);   /* normal transmit                   */
    outb(base + NE_RCR, 0x04);   /* accept broadcast                  */

    ne2000.io_base     = base;
    ne2000.irq         = irq;
    ne2000.next_packet = RX_START_PAGE + 1;

    struct net_interface *iface =
        net_register_interface("eth0", mac, ne2000_transmit, &ne2000);
    if (!iface) {
        KLOG_ERR("ne2000", "no free network interface slot");
        return;
    }

    ne2000.iface    = iface;
    iface->address  = IPV4(10, 0, 2, 15);      /* the usual emulator guest IP */
    iface->netmask  = IPV4(255, 255, 255, 0);
    iface->gateway  = IPV4(10, 0, 2, 2);
    iface->up       = true;

    irq_register(irq, ne2000_irq, &ne2000, "ne2000");
    ne2000_present = true;

    KLOG_INFO("ne2000", "eth0 up, MAC %02x:%02x:%02x:%02x:%02x:%02x, IRQ %u",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], irq);
}

/*
 * The RTL8139 is a very common emulated NIC but needs bus-master DMA and a
 * ring of descriptors. It is not implemented yet; the probe is here so the
 * hardware is reported rather than silently ignored.
 */
void rtl8139_init(void)
{
    const struct pci_device *pci = pci_find_device(0x10EC, 0x8139);
    if (pci) {
        KLOG_INFO("rtl8139", "RTL8139 present at %02x:%02x.%u but no driver is "
                             "implemented yet",
                  pci->bus, pci->slot, pci->function);
    }
}
