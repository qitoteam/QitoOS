/*
 * QitoOS - PCI bus enumeration
 */
#ifndef QITO_PCI_H
#define QITO_PCI_H

#include <kernel/types.h>

struct shell;

#define PCI_MAX_DEVICES 64

struct pci_device {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    uint8_t  interrupt_line;
    uint32_t bar[6];
};

void pci_init(void);
int  pci_device_count(void);
const struct pci_device *pci_device_at(int index);

/* Find the first device matching a class/subclass pair. */
const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass);
const struct pci_device *pci_find_device(uint16_t vendor, uint16_t device);

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t offset, uint32_t value);

const char *pci_class_name(uint8_t class_code, uint8_t subclass);
const char *pci_vendor_name(uint16_t vendor_id);

void pci_print_devices(struct shell *sh);

#endif /* QITO_PCI_H */
