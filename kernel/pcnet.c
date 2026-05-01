/*
 * pcnet.c — AMD PCNet-PCI II (Am79C970A) driver for Azix OS
 *
 * VirtualBox emulates this card at PCI 00:03.0, vendor 0x1022, device 0x2000.
 *
 * Uses 32-bit SWSTYLE=2 mode and polling (no IRQ) — enough for ping.
 *
 * Key steps:
 *   1. Find via pci_find(0x1022, 0x2000)
 *   2. Read I/O base from BAR0
 *   3. Software reset
 *   4. Switch to SWSTYLE=2 (BCR20)
 *   5. Read MAC from APROM
 *   6. Build RX/TX descriptor rings + init block
 *   7. INIT → wait IDON → STRT
 */

#include "pcnet.h"
#include "pci.h"
#include "io.h"
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Ring sizes (must be powers of 2, max 512)                          */
/* ------------------------------------------------------------------ */
#define NUM_RX     4
#define NUM_TX     4
#define BUF_SIZE   1536          /* max Ethernet frame                */
#define RLEN_LOG2  2             /* log2(NUM_RX) — stored in RLEN     */
#define TLEN_LOG2  2             /* log2(NUM_TX) — stored in TLEN     */

/* ------------------------------------------------------------------ */
/* PCNet I/O register offsets (32-bit SWSTYLE=2)                      */
/* ------------------------------------------------------------------ */
#define PCNET_APROM   0x00       /* Address PROM: MAC at bytes 0-5    */
#define PCNET_RDP     0x10       /* Register Data Port (32-bit)       */
#define PCNET_RAP     0x14       /* Register Address Port (32-bit)    */
#define PCNET_RESET   0x18       /* Read to issue SW reset            */
#define PCNET_BDP     0x1C       /* BCR Data Port (32-bit)            */

/* CSR (Control/Status Register) numbers accessed via RAP → RDP       */
#define CSR0          0          /* Status / Control                  */
#define CSR1          1          /* Init block addr [15:0]            */
#define CSR2          2          /* Init block addr [31:16]           */
#define CSR4          4          /* Feature control (auto-pad TX)     */
#define CSR15         15         /* Mode                              */

/* CSR0 control/status bits                                            */
#define CSR0_INIT     0x0001     /* Start initialization              */
#define CSR0_STRT     0x0002     /* Start the chip                   */
#define CSR0_STOP     0x0004     /* Stop the chip                    */
#define CSR0_IDON     0x0100     /* Initialization done               */

/* BCR (Bus Configuration Register) number accessed via RAP → BDP     */
#define BCR20         20         /* Software style / mode             */

/* ------------------------------------------------------------------ */
/* Descriptor status bits (in the high 16-bit word of RMD1/TMD1)     */
/* ------------------------------------------------------------------ */
#define DESC_OWN      0x8000     /* 1 = card owns this descriptor     */
#define DESC_ERR      0x4000     /* Error summary                     */
#define DESC_STP      0x0200     /* Start of packet                   */
#define DESC_ENP      0x0100     /* End of packet                     */

/* ------------------------------------------------------------------ */
/* RX descriptor (SWSTYLE=2, 16 bytes, little-endian)                 */
/*                                                                      */
/*  Dword 0 (bytes 0-3):  RBADR — buffer physical address             */
/*  Dword 1 (bytes 4-7):  low 16 = BCNT (neg buf size | 0xF000)      */
/*                         high 16 = FLAGS (OWN=bit15, ERR=14, ...)   */
/*  Dword 2 (bytes 8-11): bits[11:0] = MCNT (received byte count)    */
/*  Dword 3 (bytes 12-15): reserved                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t base;
    uint16_t bcnt;
    uint16_t status;
    uint32_t mcnt;
    uint32_t rsvd;
} __attribute__((packed)) rx_desc_t;

/* ------------------------------------------------------------------ */
/* TX descriptor (SWSTYLE=2, 16 bytes)                                */
/*  Same Dword 1 layout as RX: low 16 = BCNT, high 16 = FLAGS        */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t base;
    uint16_t bcnt;
    uint16_t status;
    uint32_t misc;
    uint32_t rsvd;
} __attribute__((packed)) tx_desc_t;

/* ------------------------------------------------------------------ */
/* Initialization block (SWSTYLE=2, 28 bytes)                         */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t mode;       /* 0x0000 = normal mode                      */
    uint8_t  rlen;       /* RLEN_LOG2 << 4 (e.g. 0x20 for 4 buffers) */
    uint8_t  tlen;       /* TLEN_LOG2 << 4                            */
    uint8_t  mac[6];     /* Station MAC address                       */
    uint16_t rsvd;
    uint32_t filter[2];  /* Multicast filter (0 = promisc off)        */
    uint32_t rxbase;     /* Physical address of RX ring               */
    uint32_t txbase;     /* Physical address of TX ring               */
} __attribute__((packed)) init_block_t;

/* ------------------------------------------------------------------ */
/* Static DMA buffers (physical == virtual in Azix — no paging)       */
/* All aligned so the card can DMA to/from them directly.             */
/* ------------------------------------------------------------------ */
static rx_desc_t   rx_ring[NUM_RX]        __attribute__((aligned(16)));
static tx_desc_t   tx_ring[NUM_TX]        __attribute__((aligned(16)));
static uint8_t     rx_buf[NUM_RX][BUF_SIZE] __attribute__((aligned(4)));
static uint8_t     tx_buf[NUM_TX][BUF_SIZE] __attribute__((aligned(4)));
static init_block_t init_block             __attribute__((aligned(4)));

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */
static uint16_t io_base  = 0;
static uint8_t  mac_addr[6];
static int      rx_idx   = 0;   /* next RX desc to check             */
static int      tx_idx   = 0;   /* next TX desc to use               */
int             pcnet_ready = 0;

/* ------------------------------------------------------------------ */
/* Register helpers                                                    */
/* ------------------------------------------------------------------ */
static void csr_write(uint32_t csr, uint32_t val)
{
    outl(io_base + PCNET_RAP, csr);
    outl(io_base + PCNET_RDP, val);
}
static uint32_t csr_read(uint32_t csr)
{
    outl(io_base + PCNET_RAP, csr);
    return inl(io_base + PCNET_RDP);
}
static void bcr_write(uint32_t bcr, uint32_t val)
{
    outl(io_base + PCNET_RAP, bcr);
    outl(io_base + PCNET_BDP, val);
}

/* Crude µs-level busy delay using a known-slow I/O port read (~1µs). */
static void udelay(uint32_t us)
{
    for (uint32_t i = 0; i < us * 4; i++) inb(0x80);
}

/* ------------------------------------------------------------------ */
/* pcnet_init                                                          */
/* ------------------------------------------------------------------ */
int pcnet_init(void)
{
    uint8_t bus, dev, func;

    /* Step 1: Locate the PCNet card on the PCI bus */
    if (!pci_find(0x1022, 0x2000, &bus, &dev, &func))
        return 0;

    /* Step 2: Read BAR0 — must be I/O space (bit 0 == 1) */
    uint32_t bar0 = pci_read32(bus, dev, func, 0x10);
    if (!(bar0 & 0x1)) return 0;          /* unexpected MMIO BAR */
    io_base = (uint16_t)(bar0 & 0xFFFC);  /* mask I/O flag bits  */

    /* Step 3: Software reset — read RESET port, then write 0 to RDP */
    inl(io_base + PCNET_RESET);
    udelay(10);
    outl(io_base + PCNET_RDP, 0);   /* release reset (write 0 in 32-bit mode) */
    udelay(10);

    /* Step 4: Switch to 32-bit SWSTYLE=2 (must be done right after reset) */
    bcr_write(BCR20, 2);

    /* Step 5: Read MAC from APROM (bytes 0-5) */
    uint32_t ap0 = inl(io_base + PCNET_APROM);
    uint32_t ap1 = inl(io_base + PCNET_APROM + 4);
    mac_addr[0] = (uint8_t)(ap0);
    mac_addr[1] = (uint8_t)(ap0 >> 8);
    mac_addr[2] = (uint8_t)(ap0 >> 16);
    mac_addr[3] = (uint8_t)(ap0 >> 24);
    mac_addr[4] = (uint8_t)(ap1);
    mac_addr[5] = (uint8_t)(ap1 >> 8);

    /* Step 6: Build RX ring — give all descriptors to the card */
    for (int i = 0; i < NUM_RX; i++) {
        rx_ring[i].base   = (uint32_t)(uintptr_t)rx_buf[i];
        /* BCNT = 2's-complement of BUF_SIZE, upper nibble forced 0xF */
        rx_ring[i].bcnt   = (uint16_t)(-BUF_SIZE) | 0xF000;
        rx_ring[i].status = DESC_OWN;      /* card owns it */
        rx_ring[i].mcnt   = 0;
        rx_ring[i].rsvd   = 0;
    }

    /* Build TX ring — all owned by host initially */
    for (int i = 0; i < NUM_TX; i++) {
        tx_ring[i].base   = (uint32_t)(uintptr_t)tx_buf[i];
        tx_ring[i].bcnt   = 0;
        tx_ring[i].status = 0;
        tx_ring[i].misc   = 0;
        tx_ring[i].rsvd   = 0;
    }

    /* Step 7a: Fill init block */
    init_block.mode      = 0x0000;              /* normal mode */
    init_block.rlen      = (uint8_t)(RLEN_LOG2 << 4);
    init_block.tlen      = (uint8_t)(TLEN_LOG2 << 4);
    for (int i = 0; i < 6; i++) init_block.mac[i] = mac_addr[i];
    init_block.rsvd      = 0;
    init_block.filter[0] = 0;                  /* accept all multicast */
    init_block.filter[1] = 0;
    init_block.rxbase    = (uint32_t)(uintptr_t)rx_ring;
    init_block.txbase    = (uint32_t)(uintptr_t)tx_ring;

    /* Step 7b: Point card at init block via CSR1/CSR2 */
    uint32_t ib = (uint32_t)(uintptr_t)&init_block;
    csr_write(CSR1, ib & 0xFFFF);
    csr_write(CSR2, ib >> 16);

    /* CSR4 = 0x0915: enable auto-pad short TX frames to 64 bytes       */
    csr_write(CSR4, 0x0915);

    /* Step 7c: Issue INIT and wait for IDON (~1 ms) */
    csr_write(CSR0, CSR0_INIT);
    uint32_t tries = 50000;
    while (tries-- && !(csr_read(CSR0) & CSR0_IDON))
        udelay(1);
    if (!tries) return 0;   /* init timed out */

    /* Step 7d: Clear IDON, start the chip */
    csr_write(CSR0, CSR0_IDON | CSR0_STRT);

    pcnet_ready = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* pcnet_send — transmit one raw Ethernet frame                       */
/* ------------------------------------------------------------------ */
int pcnet_send(const uint8_t *data, uint16_t len)
{
    if (!pcnet_ready || len == 0 || len > BUF_SIZE) return 0;

    tx_desc_t *td = &tx_ring[tx_idx];

    /* Wait until host owns this slot (card finishes previous send) */
    uint32_t tries = 200000;
    while ((td->status & DESC_OWN) && tries--) udelay(1);
    if (td->status & DESC_OWN) return 0;   /* TX ring full */

    /* Copy frame into the TX buffer */
    uint8_t *buf = tx_buf[tx_idx];
    for (uint16_t i = 0; i < len; i++) buf[i] = data[i];

    /* Set descriptor: STP+ENP = single-buffer packet */
    td->bcnt   = (uint16_t)(-len) | 0xF000;
    td->misc   = 0;
    td->status = DESC_OWN | DESC_STP | DESC_ENP;   /* hand to card */

    /* Poke STRT so card checks the TX ring immediately */
    csr_write(CSR0, CSR0_STRT);

    tx_idx = (tx_idx + 1) % NUM_TX;
    return 1;
}

/* ------------------------------------------------------------------ */
/* pcnet_poll — check for one received Ethernet frame (polling)       */
/* ------------------------------------------------------------------ */
uint16_t pcnet_poll(uint8_t *buf)
{
    if (!pcnet_ready) return 0;

    rx_desc_t *rd = &rx_ring[rx_idx];

    /* Card still owns this descriptor — nothing received */
    if (rd->status & DESC_OWN) return 0;

    /* Error — recycle and skip */
    if (rd->status & DESC_ERR) {
        rd->mcnt   = 0;
        rd->status = DESC_OWN;
        rx_idx = (rx_idx + 1) % NUM_RX;
        return 0;
    }

    /* Extract received byte count from MCNT[11:0] */
    uint16_t len = (uint16_t)(rd->mcnt & 0xFFF);

    /* Copy to caller's buffer */
    uint8_t *src = rx_buf[rx_idx];
    for (uint16_t i = 0; i < len; i++) buf[i] = src[i];

    /* Return descriptor to card */
    rd->mcnt   = 0;
    rd->status = DESC_OWN;
    rx_idx = (rx_idx + 1) % NUM_RX;

    return len;
}

/* ------------------------------------------------------------------ */
/* pcnet_get_mac                                                       */
/* ------------------------------------------------------------------ */
void pcnet_get_mac(uint8_t out[6])
{
    for (int i = 0; i < 6; i++) out[i] = mac_addr[i];
}
