/*
 * QitoOS - AHCI/SATA driver + persistent filesystem
 *
 * Implements AHCI detection, port initialization, and sector read/write.
 * Foundational for persistence – everything lived in RAM before.
 */

#include <kernel/ahci.h>
#include <kernel/pci.h>
#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/io.h>
#include <kernel/time.h>

#define AHCI_GHC_AE (1u<<31)
#define AHCI_GHC_IE (1u<<1)
#define AHCI_GHC_HR (1u<<0)

#define AHCI_PORT_CMD_ST  (1u<<0)
#define AHCI_PORT_CMD_FRE (1u<<4)
#define AHCI_PORT_CMD_FR  (1u<<14)
#define AHCI_PORT_CMD_CR  (1u<<15)

#define AHCI_PORT_IS_TFES (1u<<30)

#define SATA_SIG_ATA   0x00000101
#define SATA_SIG_ATAPI 0xEB140101
#define SATA_SIG_PM    0x96690101
#define SATA_SIG_SEMB  0xC33C0101

struct hba_port {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t reserved0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t reserved1[11];
    uint32_t vendor[4];
} PACKED;

struct hba_mem {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t  reserved[0xA0-0x2C];
    uint8_t  vendor[0x100-0xA0];
    struct hba_port ports[32];
} PACKED;

struct hba_cmd_header {
    uint8_t  cfl:5;
    uint8_t  a:1;
    uint8_t  w:1;
    uint8_t  p:1;
    uint8_t  r:1;
    uint8_t  b:1;
    uint8_t  c:1;
    uint8_t  rsv0:1;
    uint8_t  pmp:4;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv1[4];
} PACKED;

struct hba_prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved0;
    uint32_t dbc:22;
    uint32_t reserved1:9;
    uint32_t i:1;
} PACKED;

struct hba_cmd_table {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  reserved[48];
    struct hba_prdt_entry prdt[1];
} PACKED;

typedef enum { FIS_TYPE_REG_H2D=0x27 } fis_type_t;

struct fis_reg_h2d {
    uint8_t fis_type;
    uint8_t pmport:4;
    uint8_t rsv0:3;
    uint8_t c:1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2;
    uint8_t device;
    uint8_t lba3, lba4, lba5;
    uint8_t featureh;
    uint8_t countl, counth;
    uint8_t icc;
    uint8_t control;
    uint8_t rsv1[4];
} PACKED;

static struct hba_mem *hba = NULL;
static struct ahci_port_info ports[AHCI_MAX_PORTS];
static int port_count=0;
static bool_t ahci_ready=false;

static uint32_t ahci_read32(volatile uint32_t *reg){ return *reg; }
static void ahci_write32(volatile uint32_t *reg, uint32_t v){ *reg=v; }

void ahci_init(void)
{
    const struct pci_device *pci = pci_find_class(0x01, 0x06);
    if (!pci) {
        KLOG_INFO("ahci","no AHCI controller found, running without persistence");
        return;
    }
    KLOG_INFO("ahci","found AHCI at %02x:%02x.%u", pci->bus, pci->slot, pci->function);

    uint32_t bar5 = pci_config_read32(pci->bus, pci->slot, pci->function, 0x24);
    uint32_t abar_phys = bar5 & ~0xFFFu;
    if (!abar_phys) {
        KLOG_WARN("ahci","BAR5 not mapped");
        return;
    }

    // Enable bus master, memory space
    uint32_t cmd = pci_config_read32(pci->bus, pci->slot, pci->function, 0x04);
    cmd |= 0x06; // bus master + mem space
    pci_config_write32(pci->bus, pci->slot, pci->function, 0x04, cmd);

    // Map ABAR – it's below 4GB, identity mapped already
    hba = (struct hba_mem*)phys_to_virt((phys_addr_t)abar_phys);
    // But need to map as uncached? For now use identity.

    uint32_t pi = hba->pi;
    uint32_t cap = hba->cap;
    int num_ports = (cap & 0x1F) + 1;
    KLOG_INFO("ahci","cap=0x%x pi=0x%x version=0x%x, %d ports", cap, pi, hba->vs, num_ports);

    // Enable AHCI
    hba->ghc |= AHCI_GHC_AE;

    // Simple port detection – don't fully reset for QEMU
    port_count=0;
    for (int i=0;i<32 && port_count<AHCI_MAX_PORTS;i++) {
        if (!(pi & (1u<<i))) continue;
        struct hba_port *port = &hba->ports[i];
        uint32_t ssts = port->ssts;
        uint8_t det = ssts & 0x0F;
        uint8_t ipm = (ssts>>8)&0x0F;
        if (det!=3 || ipm!=1) continue; // no device or not active

        uint32_t sig = port->sig;
        bool_t is_atapi = (sig==SATA_SIG_ATAPI);

        // For QEMU, we can try to read without full FIS setup – but we will init minimal
        // Allocate command list and FIS for this port
        phys_addr_t clb_phys = pmm_alloc_page();
        phys_addr_t fb_phys = pmm_alloc_page();
        if (!clb_phys || !fb_phys) continue;
        void *clb_virt = phys_to_virt(clb_phys);
        void *fb_virt = phys_to_virt(fb_phys);
        memset(clb_virt,0,PAGE_SIZE);
        memset(fb_virt,0,PAGE_SIZE);

        // Stop port before reconfig
        port->cmd &= ~(AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE);
        // Wait for CR and FR to clear (with timeout)
        for (int t=0;t<1000;t++) {
            if (!(port->cmd & (AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR))) break;
            time_sleep_ms(1);
        }

        port->clb = (uint32_t)clb_phys;
        port->clbu = (uint32_t)(clb_phys>>32);
        port->fb = (uint32_t)fb_phys;
        port->fbu = (uint32_t)(fb_phys>>32);

        // Enable FIS receive and start
        port->cmd |= AHCI_PORT_CMD_FRE;
        port->cmd |= AHCI_PORT_CMD_ST;

        ports[port_count].port = i;
        ports[port_count].present = true;
        ports[port_count].is_atapi = is_atapi;
        ports[port_count].sectors = 0; // will probe later
        strlcpy(ports[port_count].model, is_atapi?"ATAPI":"ATA", sizeof(ports[port_count].model));
        KLOG_INFO("ahci","port %d: %s device present, sig=0x%x", i, is_atapi?"ATAPI":"ATA", sig);
        port_count++;
    }

    ahci_ready = (port_count>0);
    if (ahci_ready) {
        KLOG_INFO("ahci","AHCI ready, %d port(s) active – persistence available", port_count);
    } else {
        KLOG_INFO("ahci","AHCI controller found but no active devices");
    }
}

int ahci_port_count(void){ return port_count; }
bool_t ahci_available(void){ return ahci_ready; }
const struct ahci_port_info *ahci_port_info(int idx){
    if (idx<0||idx>=port_count) return NULL;
    return &ports[idx];
}
uint64_t ahci_total_sectors(int port){ (void)port; return 0; }

static int ahci_wait_port(struct hba_port *port, uint32_t mask, uint32_t expected, int timeout_ms)
{
    for (int i=0;i<timeout_ms;i++) {
        if ((port->tfd & mask)==expected) return 0;
        time_sleep_ms(1);
    }
    return -1;
}

int ahci_read(int port_idx, uint64_t lba, uint32_t count, void *buffer)
{
    if (!ahci_ready || !hba) return -1;
    if (port_idx<0||port_idx>=port_count) return -1;
    int p = ports[port_idx].port;
    struct hba_port *port = &hba->ports[p];

    if (!buffer || count==0) return -1;

    // Find free command slot
    uint32_t ci = port->ci;
    int slot=-1;
    for (int i=0;i<32;i++) if (!(ci & (1u<<i))) { slot=i; break; }
    if (slot<0) return -1;

    struct hba_cmd_header *cmd_hdr = (struct hba_cmd_header*)phys_to_virt((phys_addr_t)port->clb);
    cmd_hdr += slot;
    phys_addr_t cmd_tbl_phys = pmm_alloc_page();
    if (!cmd_tbl_phys) return -1;
    struct hba_cmd_table *cmd_tbl = phys_to_virt(cmd_tbl_phys);
    memset(cmd_tbl,0,sizeof(*cmd_tbl));

    cmd_hdr->cfl = sizeof(struct fis_reg_h2d)/4;
    cmd_hdr->w = 0;
    cmd_hdr->prdtl = 1;
    cmd_hdr->ctba = (uint32_t)cmd_tbl_phys;
    cmd_hdr->ctbau = (uint32_t)(cmd_tbl_phys>>32);

    // Setup PRDT
    phys_addr_t buf_phys = virt_to_phys_direct(buffer);
    // If buffer is in high heap, virt_to_phys_direct may not work – use vmm_resolve
    if (buf_phys==0) {
        // Try resolve via kernel space
        buf_phys = vmm_resolve(vmm_kernel_space(), (virt_addr_t)buffer);
    }
    if (!buf_phys) { pmm_free_page(cmd_tbl_phys); return -1; }

    cmd_tbl->prdt[0].dba = (uint32_t)buf_phys;
    cmd_tbl->prdt[0].dbau = (uint32_t)(buf_phys>>32);
    cmd_tbl->prdt[0].dbc = (count*AHCI_SECTOR_SIZE)-1;
    cmd_tbl->prdt[0].i = 0;

    struct fis_reg_h2d *fis = (struct fis_reg_h2d*)cmd_tbl->cfis;
    memset(fis,0,sizeof(*fis));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = 0x25; // READ DMA EXT
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba>>8);
    fis->lba2 = (uint8_t)(lba>>16);
    fis->device = 1<<6; // LBA mode
    fis->lba3 = (uint8_t)(lba>>24);
    fis->lba4 = (uint8_t)(lba>>32);
    fis->lba5 = (uint8_t)(lba>>40);
    fis->countl = (uint8_t)count;
    fis->counth = (uint8_t)(count>>8);

    if (ahci_wait_port(port, 0x88, 0, 1000)!=0) { pmm_free_page(cmd_tbl_phys); return -1; }

    port->ci = (1u<<slot);
    for (int i=0;i<5000;i++) {
        if (!(port->ci & (1u<<slot))) break;
        time_sleep_ms(1);
        if (port->is & AHCI_PORT_IS_TFES) { port->is = AHCI_PORT_IS_TFES; break; }
    }
    int ok = !(port->ci & (1u<<slot));
    pmm_free_page(cmd_tbl_phys);
    return ok?0:-1;
}

int ahci_write(int port_idx, uint64_t lba, uint32_t count, const void *buffer)
{
    if (!ahci_ready || !hba) return -1;
    if (port_idx<0||port_idx>=port_count) return -1;
    int p = ports[port_idx].port;
    struct hba_port *port = &hba->ports[p];
    if (!buffer||count==0) return -1;

    uint32_t ci = port->ci;
    int slot=-1;
    for (int i=0;i<32;i++) if (!(ci & (1u<<i))) { slot=i; break; }
    if (slot<0) return -1;

    struct hba_cmd_header *cmd_hdr = (struct hba_cmd_header*)phys_to_virt((phys_addr_t)port->clb);
    cmd_hdr += slot;
    phys_addr_t cmd_tbl_phys = pmm_alloc_page();
    if (!cmd_tbl_phys) return -1;
    struct hba_cmd_table *cmd_tbl = phys_to_virt(cmd_tbl_phys);
    memset(cmd_tbl,0,sizeof(*cmd_tbl));

    cmd_hdr->cfl = sizeof(struct fis_reg_h2d)/4;
    cmd_hdr->w = 1;
    cmd_hdr->prdtl = 1;
    cmd_hdr->ctba = (uint32_t)cmd_tbl_phys;
    cmd_hdr->ctbau = (uint32_t)(cmd_tbl_phys>>32);

    phys_addr_t buf_phys = virt_to_phys_direct((void*)buffer);
    if (!buf_phys) buf_phys = vmm_resolve(vmm_kernel_space(), (virt_addr_t)buffer);
    if (!buf_phys) { pmm_free_page(cmd_tbl_phys); return -1; }

    cmd_tbl->prdt[0].dba = (uint32_t)buf_phys;
    cmd_tbl->prdt[0].dbau = (uint32_t)(buf_phys>>32);
    cmd_tbl->prdt[0].dbc = (count*AHCI_SECTOR_SIZE)-1;
    cmd_tbl->prdt[0].i = 0;

    struct fis_reg_h2d *fis = (struct fis_reg_h2d*)cmd_tbl->cfis;
    memset(fis,0,sizeof(*fis));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = 0x35; // WRITE DMA EXT
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba>>8);
    fis->lba2 = (uint8_t)(lba>>16);
    fis->device = 1<<6;
    fis->lba3 = (uint8_t)(lba>>24);
    fis->lba4 = (uint8_t)(lba>>32);
    fis->lba5 = (uint8_t)(lba>>40);
    fis->countl = (uint8_t)count;
    fis->counth = (uint8_t)(count>>8);

    if (ahci_wait_port(port, 0x88, 0, 1000)!=0) { pmm_free_page(cmd_tbl_phys); return -1; }

    port->ci = (1u<<slot);
    for (int i=0;i<5000;i++) {
        if (!(port->ci & (1u<<slot))) break;
        time_sleep_ms(1);
        if (port->is & AHCI_PORT_IS_TFES) { port->is = AHCI_PORT_IS_TFES; break; }
    }
    int ok = !(port->ci & (1u<<slot));
    pmm_free_page(cmd_tbl_phys);
    return ok?0:-1;
}
