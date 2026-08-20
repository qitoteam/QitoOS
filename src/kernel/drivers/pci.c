/*
 * QitoOS - PCI bus enumeration
 *
 * Uses the legacy configuration mechanism #1 (I/O ports 0xCF8/0xCFC) to walk
 * every bus, slot and function, recording what is present so drivers and the
 * `hwinfo` command can find their hardware.
 */

#include <kernel/pci.h>
#include <kernel/io.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/shell.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static struct pci_device devices[PCI_MAX_DEVICES];
static int               device_count;

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t address = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                       ((uint32_t)func << 8) | (offset & 0xFC);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                        uint32_t value)
{
    uint32_t address = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                       ((uint32_t)func << 8) | (offset & 0xFC);

    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static uint16_t config_read16(uint8_t bus, uint8_t slot, uint8_t func,
                              uint8_t offset)
{
    uint32_t value = pci_config_read32(bus, slot, func, offset);
    return (uint16_t)((value >> ((offset & 2) * 8)) & 0xFFFF);
}

static uint8_t config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t value = pci_config_read32(bus, slot, func, offset);
    return (uint8_t)((value >> ((offset & 3) * 8)) & 0xFF);
}

const char *pci_vendor_name(uint16_t vendor_id)
{
    switch (vendor_id) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "ATI/AMD";
    case 0x1234: return "QEMU/Bochs";
    case 0x15AD: return "VMware";
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x1B36: return "Red Hat";
    case 0x10EC: return "Realtek";
    case 0x1013: return "Cirrus Logic";
    case 0x106B: return "Apple";
    case 0x5333: return "S3 Graphics";
    case 0x80EE: return "VirtualBox";
    default:     return "unknown";
    }
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass)
{
    switch (class_code) {
    case 0x00: return "unclassified device";
    case 0x01:
        switch (subclass) {
        case 0x01: return "IDE storage controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default:   return "mass storage controller";
        }
    case 0x02: return "network controller";
    case 0x03:
        return (subclass == 0x00) ? "VGA display controller" : "display controller";
    case 0x04:
        switch (subclass) {
        case 0x00: return "multimedia video controller";
        case 0x01: return "multimedia audio controller";
        case 0x03: return "audio device";
        default:   return "multimedia controller";
        }
    case 0x05: return "memory controller";
    case 0x06:
        switch (subclass) {
        case 0x00: return "host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-to-PCI bridge";
        case 0x80: return "bridge device";
        default:   return "bridge";
        }
    case 0x07: return "communication controller";
    case 0x08: return "system peripheral";
    case 0x09: return "input device controller";
    case 0x0B: return "processor";
    case 0x0C:
        switch (subclass) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default:   return "serial bus controller";
        }
    case 0x0D: return "wireless controller";
    default:   return "device";
    }
}

static void record(uint8_t bus, uint8_t slot, uint8_t func)
{
    if (device_count >= PCI_MAX_DEVICES) {
        return;
    }

    struct pci_device *device = &devices[device_count];

    device->bus       = bus;
    device->slot      = slot;
    device->function  = func;
    device->vendor_id = config_read16(bus, slot, func, 0x00);
    device->device_id = config_read16(bus, slot, func, 0x02);
    device->revision  = config_read8(bus, slot, func, 0x08);
    device->prog_if   = config_read8(bus, slot, func, 0x09);
    device->subclass  = config_read8(bus, slot, func, 0x0A);
    device->class_code = config_read8(bus, slot, func, 0x0B);
    device->header_type = config_read8(bus, slot, func, 0x0E);
    device->interrupt_line = config_read8(bus, slot, func, 0x3C);

    /* Base address registers are only meaningful for header type 0. */
    if ((device->header_type & 0x7F) == 0) {
        for (int i = 0; i < 6; i++) {
            device->bar[i] = pci_config_read32(bus, slot, func,
                                               (uint8_t)(0x10 + i * 4));
        }
    }

    device_count++;
}

void pci_init(void)
{
    device_count = 0;

    /*
     * Brute-force scan of all 256 buses. This is slower than following the
     * bridge topology but is simple and reliable on the hardware Qito targets.
     */
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint16_t vendor = config_read16((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            if (vendor == 0xFFFF) {
                continue;
            }

            record((uint8_t)bus, (uint8_t)slot, 0);

            /* Bit 7 of the header type marks a multi-function device. */
            uint8_t header = config_read8((uint8_t)bus, (uint8_t)slot, 0, 0x0E);
            if (!(header & 0x80)) {
                continue;
            }
            for (int func = 1; func < 8; func++) {
                if (config_read16((uint8_t)bus, (uint8_t)slot, (uint8_t)func,
                                  0x00) != 0xFFFF) {
                    record((uint8_t)bus, (uint8_t)slot, (uint8_t)func);
                }
            }
        }
    }

    KLOG_INFO("pci", "%d device(s) found", device_count);
    for (int i = 0; i < device_count; i++) {
        const struct pci_device *d = &devices[i];
        KLOG_DEBUG("pci", "  %02x:%02x.%u %04x:%04x %s (%s)", d->bus, d->slot,
                   d->function, d->vendor_id, d->device_id,
                   pci_class_name(d->class_code, d->subclass),
                   pci_vendor_name(d->vendor_id));
    }
}

int pci_device_count(void)
{
    return device_count;
}

const struct pci_device *pci_device_at(int index)
{
    if (index < 0 || index >= device_count) {
        return NULL;
    }
    return &devices[index];
}

const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass)
{
    for (int i = 0; i < device_count; i++) {
        if (devices[i].class_code == class_code &&
            devices[i].subclass == subclass) {
            return &devices[i];
        }
    }
    return NULL;
}

const struct pci_device *pci_find_device(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == vendor && devices[i].device_id == device) {
            return &devices[i];
        }
    }
    return NULL;
}

void pci_print_devices(struct shell *sh)
{
    if (device_count == 0) {
        shell_printf(sh, "  no PCI devices detected\n");
        return;
    }

    for (int i = 0; i < device_count; i++) {
        const struct pci_device *d = &devices[i];
        shell_printf(sh, "  %02x:%02x.%u  %04x:%04x  %-28s %s\n", d->bus, d->slot,
                     d->function, d->vendor_id, d->device_id,
                     pci_class_name(d->class_code, d->subclass),
                     pci_vendor_name(d->vendor_id));
    }
}
