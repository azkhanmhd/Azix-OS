#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* PCI configuration space access (I/O ports 0xCF8 / 0xCFC) */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

/* Scan all 256 buses × 32 devices × 8 functions and print found devices */
void pci_scan(void);

/* Find a device by vendor+device ID. Returns 1 and fills bus/dev/func if found. */
int pci_find(uint16_t vendor, uint16_t device,
             uint8_t *bus_out, uint8_t *dev_out, uint8_t *func_out);

/* Set output callbacks (call before pci_scan) */
void pci_set_output(void (*puts_fn)(const char *, uint32_t),
                    void (*puthex_fn)(uint32_t, uint32_t));

/* Well-known vendor IDs */
#define PCI_VENDOR_INTEL    0x8086
#define PCI_VENDOR_AMD      0x1022
#define PCI_VENDOR_VBOX     0x80EE   /* InnoTek / VirtualBox */
#define PCI_VENDOR_REALTEK  0x10EC
#define PCI_VENDOR_VIRTIO   0x1AF4   /* Red Hat / virtio     */

/* Class codes (byte 3 of class/subclass/prog-if/rev) */
#define PCI_CLASS_NETWORK   0x02
#define PCI_CLASS_DISPLAY   0x03
#define PCI_CLASS_STORAGE   0x01
#define PCI_CLASS_BRIDGE    0x06

#endif /* PCI_H */
