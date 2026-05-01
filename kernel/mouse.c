/*
 * mouse.c — PS/2 mouse driver for Azix OS
 *
 * Reads 3-byte PS/2 packets from IRQ12, moves a software cursor on the
 * framebuffer (XOR-drawn), and checks left-click hits on the Reboot /
 * Shutdown buttons drawn in the top bar.
 */
#include "mouse.h"
#include "io.h"
#include "fb.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static int     cur_x    = 0;
static int     cur_y    = 0;
static uint8_t btn_state = 0;
static uint8_t prev_btn  = 0;

/* 3-byte packet accumulator */
static uint8_t mouse_cycle   = 0;
static uint8_t mouse_pkt[3]  = {0, 0, 0};

/* ------------------------------------------------------------------ */
/* 8042 PS/2 controller helpers                                        */
/* ------------------------------------------------------------------ */
static void kb_wait_write(void)
{
    uint32_t t = 200000;
    while (--t && (inb(0x64) & 0x02));   /* wait: input buffer empty */
}

static void kb_wait_read(void)
{
    uint32_t t = 200000;
    while (--t && !(inb(0x64) & 0x01));  /* wait: output buffer full */
}

/* Send one byte to the mouse (routes through 8042 aux port) */
static void mouse_send(uint8_t data)
{
    kb_wait_write();
    outb(0x64, 0xD4);   /* tell controller: next byte → aux device */
    kb_wait_write();
    outb(0x60, data);
}

/* Read one byte (ACK or response) from mouse */
static uint8_t mouse_recv(void)
{
    kb_wait_read();
    return inb(0x60);
}

/* ------------------------------------------------------------------ */
/* Shutdown / reboot (called from interrupt context — fine for x86)   */
/* ------------------------------------------------------------------ */
static void do_shutdown(void)
{
    __asm__ volatile("cli");
    outw(0x4004, 0x3400);   /* VirtualBox PIIX4 ACPI S5  */
    outw(0x604,  0x2000);   /* QEMU / Bochs fallback      */
    for (;;) __asm__ volatile("hlt");
}

static void do_reboot(void)
{
    __asm__ volatile("cli");
    uint8_t v;
    do { v = inb(0x64); } while (v & 0x02);
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile("hlt");
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */
void mouse_init(void)
{
    /* 1. Enable the auxiliary (mouse) device */
    kb_wait_write();
    outb(0x64, 0xA8);

    /* 2. Enable mouse IRQ12 via the compaq status byte */
    kb_wait_write();
    outb(0x64, 0x20);           /* command: read compaq status byte  */
    kb_wait_read();
    uint8_t status = inb(0x60);
    status |=  0x02;            /* bit 1: enable IRQ12               */
    status &= ~0x20;            /* bit 5: enable mouse clock         */
    kb_wait_write();
    outb(0x64, 0x60);           /* command: write compaq status byte */
    kb_wait_write();
    outb(0x60, status);

    /* 3. Reset mouse — receives ACK (0xFA), 0xAA, 0x00 */
    mouse_send(0xFF);
    mouse_recv();   /* 0xFA ACK */
    mouse_recv();   /* 0xAA    */
    mouse_recv();   /* 0x00 ID */

    /* 4. Use default settings */
    mouse_send(0xF6);
    mouse_recv();   /* ACK */

    /* 5. Enable data reporting */
    mouse_send(0xF4);
    mouse_recv();   /* ACK */

    /* Set initial position — cursor appears on first mouse packet */
    cur_x = (int)fb_width()  / 2;
    cur_y = (int)fb_height() / 2;
}

/* ------------------------------------------------------------------ */
/* IRQ12 handler — called from idt_asm.asm irq12 stub                 */
/* ------------------------------------------------------------------ */
void mouse_handler(void)
{
    uint8_t data = inb(0x60);

    /*
     * Sync: byte 0 always has bit 3 set.  If we get an unexpected byte
     * without bit 3, reset the packet accumulator and skip.
     */
    if (mouse_cycle == 0 && !(data & 0x08))
        return;

    mouse_pkt[mouse_cycle++] = data;
    if (mouse_cycle < 3)
        return;     /* need all 3 bytes before processing */
    mouse_cycle = 0;

    uint8_t flags = mouse_pkt[0];

    /* Discard overflow packets */
    if (flags & 0x80) return;   /* X overflow */
    if (flags & 0x40) return;   /* Y overflow */

    btn_state = flags & 0x07;

    /* Signed X delta: bit 4 of byte 0 is the sign (two's complement)   */
    int dx =  (int)mouse_pkt[1] - ((flags & 0x10) ? 256 : 0);
    /* Signed Y delta: bit 5 is sign; PS/2 positive = up = negative row */
    int dy = -((int)mouse_pkt[2] - ((flags & 0x20) ? 256 : 0));

    cur_x += dx;
    cur_y += dy;

    /* Clamp to screen */
    if (cur_x < 0)                       cur_x = 0;
    if (cur_y < 0)                       cur_y = 0;
    if (cur_x >= (int)fb_width())        cur_x = (int)fb_width()  - 1;
    if (cur_y >= (int)fb_height())       cur_y = (int)fb_height() - 1;

    /* Move the software cursor */
    gui_move_cursor(cur_x, cur_y);

    /* Detect left-button click (rising edge) */
    if ((btn_state & MOUSE_BTN_LEFT) && !(prev_btn & MOUSE_BTN_LEFT)) {
        if (fb_rect_contains(&btn_reboot_rect,   cur_x, cur_y)) do_reboot();
        if (fb_rect_contains(&btn_shutdown_rect, cur_x, cur_y)) do_shutdown();
    }

    prev_btn = btn_state;
}

int     mouse_x(void)       { return cur_x; }
int     mouse_y(void)       { return cur_y; }
uint8_t mouse_buttons(void) { return btn_state; }
