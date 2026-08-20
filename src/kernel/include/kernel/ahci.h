/*
 * QitoOS - AHCI driver header
 */

#ifndef QITO_AHCI_H
#define QITO_AHCI_H

#include <kernel/types.h>
#include <kernel/pci.h>

#define AHCI_MAX_PORTS 32
#define AHCI_SECTOR_SIZE 512

struct ahci_port_info {
    int      port;
    bool_t   present;
    bool_t   is_atapi;
    uint64_t sectors;
    char     model[40];
};

void ahci_init(void);
int  ahci_port_count(void);
const struct ahci_port_info *ahci_port_info(int index);
int  ahci_read(int port, uint64_t lba, uint32_t count, void *buffer);
int  ahci_write(int port, uint64_t lba, uint32_t count, const void *buffer);
bool_t ahci_available(void);
uint64_t ahci_total_sectors(int port);

#endif /* QITO_AHCI_H */
