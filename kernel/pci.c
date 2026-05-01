/*
 * pci.c — PCI bus enumeration for Azix OS
 *
 * Uses the legacy I/O port mechanism (CONFIG_ADDRESS 0xCF8 / CONFIG_DATA 0xCFC)
 * which works on every x86 system since PCI 2.0 and is fully available in
 * VirtualBox without any special setup.
 *
 * CONFIG_ADDRESS format (32-bit write to 0xCF8):
 *   bit 31      : Enable bit (must be 1)
 *   bits 23-16  : Bus number  (0-255)
 *   bits 15-11  : Device number (0-31)
 *   bits 10-8   : Function number (0-7)
 *   bits 7-2    : Register offset (dword-aligned, i.e. offset & ~3)
 *   bits 1-0    : 0 (must be zero)
 */

#include "pci.h"
#include "io.h"
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* I/O port helpers                                                    */
/* ------------------------------------------------------------------ */
#define PCI_ADDR_PORT 0xCF8
#define PCI_DATA_PORT 0xCFC

static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    return (1u << 31)
         | ((uint32_t)bus  << 16)
         | ((uint32_t)(dev  & 0x1F) << 11)
         | ((uint32_t)(func & 0x07) << 8)
         | (offset & 0xFC);   /* dword-align */
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    outl(PCI_ADDR_PORT, pci_addr(bus, dev, func, offset));
    return inl(PCI_DATA_PORT);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    uint32_t val = pci_read32(bus, dev, func, offset);
    /* Pick the right 16-bit half */
    return (uint16_t)((val >> ((offset & 2) * 8)) & 0xFFFF);
}

/* ------------------------------------------------------------------ */
/* Class/subclass name table                                           */
/* ------------------------------------------------------------------ */
typedef struct { uint8_t cls; uint8_t sub; const char *name; } cls_entry_t;
static const cls_entry_t cls_table[] = {
    {0x00, 0x00, "Unknown device"},
    {0x00, 0x01, "VGA-compatible (legacy)"},
    {0x01, 0x00, "SCSI controller"},
    {0x01, 0x01, "IDE controller"},
    {0x01, 0x06, "SATA controller (AHCI)"},
    {0x01, 0x08, "NVM Express (NVMe)"},
    {0x02, 0x00, "Ethernet controller"},
    {0x02, 0x80, "Network controller"},
    {0x03, 0x00, "VGA display adapter"},
    {0x03, 0x80, "Display controller"},
    {0x04, 0x01, "Audio device"},
    {0x04, 0x03, "HD Audio controller"},
    {0x06, 0x00, "Host/PCI bridge"},
    {0x06, 0x01, "ISA bridge"},
    {0x06, 0x04, "PCI-to-PCI bridge"},
    {0x06, 0x80, "Bridge"},
    {0x07, 0x00, "Serial controller"},
    {0x0C, 0x03, "USB controller"},
    {0x0C, 0x05, "SMBus controller"},
    {0xFF, 0xFF, NULL} /* sentinel */
};

static const char *cls_name(uint8_t cls, uint8_t sub)
{
    for (int i = 0; cls_table[i].name; i++)
        if (cls_table[i].cls == cls && cls_table[i].sub == sub)
            return cls_table[i].name;
    /* Fallback: match on class only */
    for (int i = 0; cls_table[i].name; i++)
        if (cls_table[i].cls == cls)
            return cls_table[i].name;
    return "Unknown";
}

/* ------------------------------------------------------------------ */
/* Vendor name table (common ones)                                     */
/* ------------------------------------------------------------------ */
typedef struct { uint16_t vid; const char *name; } vid_entry_t;
static const vid_entry_t vid_table[] = {
    {0x8086, "Intel"},
    {0x1022, "AMD"},
    {0x10EC, "Realtek"},
    {0x1AF4, "VirtIO (Red Hat)"},
    {0x80EE, "InnoTek/VirtualBox"},
    {0x15AD, "VMware"},
    {0x1234, "QEMU/Bochs"},
    {0x104C, "Texas Instruments"},
    {0x10B7, "3Com"},
    {0x14E4, "Broadcom"},
    {0x106B, "Apple"},
    {0xFFFF, NULL} /* sentinel */
};

static const char *vendor_name(uint16_t vid)
{
    for (int i = 0; vid_table[i].name; i++)
        if (vid_table[i].vid == vid) return vid_table[i].name;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Output helpers (write to whatever output the kernel registered)    */
/* The pci module is independent of fb/terminal: it calls these weak  */
/* functions that kernel.c overrides via function pointer.            */
/* ------------------------------------------------------------------ */
static void (*pci_puts_fn)(const char *, uint32_t) = 0;
static void (*pci_puthex_fn)(uint32_t, uint32_t)   = 0;

void pci_set_output(void (*puts_fn)(const char *, uint32_t),
                    void (*puthex_fn)(uint32_t, uint32_t))
{
    pci_puts_fn   = puts_fn;
    pci_puthex_fn = puthex_fn;
}

static void p(const char *s)
{
    if (pci_puts_fn) pci_puts_fn(s, 0xE0E0E0); /* COL_TEXT */
}
static void pc(const char *s, uint32_t col)
{
    if (pci_puts_fn) pci_puts_fn(s, col);
}
/* Compact formatters — independent of the 32-bit print_hex32 callback */
static const char pci_hex[] = "0123456789ABCDEF";

/* 2 hex digits, e.g. bus byte */
static void phex_byte(uint8_t v)
{
    char s[3] = { pci_hex[(v>>4)&0xF], pci_hex[v&0xF], '\0' };
    p(s);
}
/* 1 hex digit, e.g. function 0-7 */
static void phex_nibble(uint8_t v)
{
    char s[2] = { pci_hex[v&0xF], '\0' };
    p(s);
}
/* 4 hex digits for vendor/device IDs */
static void phex16(uint16_t v)
{
    char s[5] = {
        pci_hex[(v>>12)&0xF], pci_hex[(v>>8)&0xF],
        pci_hex[(v>>4)&0xF],  pci_hex[v&0xF], '\0'
    };
    if (pci_puts_fn) pci_puts_fn(s, 0xFFFFFF);
}

/* ------------------------------------------------------------------ */
/* pci_scan                                                            */
/* ------------------------------------------------------------------ */
void pci_scan(void)
{
    int found = 0;

    pc("PCI Device Scan\n", 0x40E8FF); /* COL_CYAN */

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            /* Check function 0 first */
            uint32_t id = pci_read32((uint8_t)bus, dev, 0, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            if (vendor == 0xFFFF) continue; /* slot empty */

            /* How many functions does this device expose? */
            uint32_t hdr_reg = pci_read32((uint8_t)bus, dev, 0, 0x0C);
            uint8_t  hdr_type = (uint8_t)((hdr_reg >> 16) & 0xFF);
            uint8_t  max_func = (hdr_type & 0x80) ? 8 : 1; /* multi-function? */

            for (uint8_t func = 0; func < max_func; func++) {
                id = pci_read32((uint8_t)bus, dev, func, 0x00);
                vendor = (uint16_t)(id & 0xFFFF);
                if (vendor == 0xFFFF) continue;

                uint16_t device_id = (uint16_t)(id >> 16);

                uint32_t class_reg = pci_read32((uint8_t)bus, dev, func, 0x08);
                uint8_t  cls = (uint8_t)((class_reg >> 24) & 0xFF);
                uint8_t  sub = (uint8_t)((class_reg >> 16) & 0xFF);

                /* Print: [BB:DD.F] Vendor:DevID  ClassName */
                p("  [");
                phex_byte((uint8_t)bus);  p(":");
                phex_byte(dev);           p(".");
                phex_nibble(func);        p("] ");

                /* Vendor name or raw ID */
                const char *vn = vendor_name(vendor);
                if (vn) {
                    pc(vn, 0xFFE040); /* COL_YELLOW */
                } else {
                    p("0x"); phex16(vendor);
                }
                p(":"); p("0x"); phex16(device_id);

                p("  ");
                pc(cls_name(cls, sub), 0x40FF70); /* COL_GREEN */
                p("\n");

                found++;
            }
        }
    }

    if (!found) {
        pc("  No PCI devices found.\n", 0xFF6060);
    } else {
        p("  Total: ");
        /* print decimal count */
        char buf[8]; int i = 7; buf[7] = '\0';
        int n = found;
        do { buf[--i] = (char)('0' + n % 10); n /= 10; } while (n);
        p(&buf[i]);
        p(" device(s)\n");
    }
}

/* ------------------------------------------------------------------ */
/* pci_find                                                            */
/* ------------------------------------------------------------------ */
int pci_find(uint16_t vendor, uint16_t device,
             uint8_t *bus_out, uint8_t *dev_out, uint8_t *func_out)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read32((uint8_t)bus, dev, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;

            uint32_t hdr_reg = pci_read32((uint8_t)bus, dev, 0, 0x0C);
            uint8_t  max_func = ((hdr_reg >> 16) & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                id = pci_read32((uint8_t)bus, dev, func, 0x00);
                if ((uint16_t)(id & 0xFFFF)  == vendor &&
                    (uint16_t)(id >> 16)      == device) {
                    if (bus_out)  *bus_out  = (uint8_t)bus;
                    if (dev_out)  *dev_out  = dev;
                    if (func_out) *func_out = func;
                    return 1;
                }
            }
        }
    }
    return 0;
}
