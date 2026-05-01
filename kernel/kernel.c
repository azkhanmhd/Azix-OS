#include <stdint.h>
#include <stddef.h>
#include "terminal.h"
#include "keyboard.h"
#include "idt.h"
#include "vga.h"
#include "io.h"
#include "fb.h"
#include "mouse.h"
#include "heap.h"
#include "pci.h"
#include "pcnet.h"
#include "net.h"
#include "tcp.h"
#include "browser.h"

/* Provided by linker script — first byte after the kernel image */
extern uint8_t _kernel_end[];

/* Heap: 4 MB immediately after the kernel */
#define HEAP_SIZE (4 * 1024 * 1024)

#define CMD_BUF_SIZE 256

/* ================================================================== */
/* Full Multiboot v1 info struct (flags, mem, framebuffer fields)      */
/* ================================================================== */
typedef struct {
    uint32_t flags;          /*  0 */
    uint32_t mem_lower;      /*  4 */
    uint32_t mem_upper;      /*  8 */
    uint32_t boot_device;    /* 12 */
    uint32_t cmdline;        /* 16 */
    uint32_t mods_count;     /* 20 */
    uint32_t mods_addr;      /* 24 */
    uint8_t  syms[16];       /* 28 – symbol table union */
    uint32_t mmap_length;    /* 44 */
    uint32_t mmap_addr;      /* 48 */
    uint32_t drives_length;  /* 52 */
    uint32_t drives_addr;    /* 56 */
    uint32_t config_table;   /* 60 */
    uint32_t boot_loader_name; /* 64 */
    uint32_t apm_table;      /* 68 */
    uint32_t vbe_control_info; /* 72 */
    uint32_t vbe_mode_info;  /* 76 */
    uint16_t vbe_mode;       /* 80 */
    uint16_t vbe_interface_seg; /* 82 */
    uint16_t vbe_interface_off; /* 84 */
    uint16_t vbe_interface_len;   /* 86 */
    uint32_t framebuffer_addr_lo; /* 88  low 32 bits of framebuffer address */
    uint32_t framebuffer_addr_hi; /* 92  high 32 bits (always 0 on 32-bit)  */
    uint32_t framebuffer_pitch;   /* 96 */
    uint32_t framebuffer_width; /* 100 */
    uint32_t framebuffer_height;/* 104 */
    uint8_t  framebuffer_bpp;   /* 108 */
    uint8_t  framebuffer_type;  /* 109  0 = indexed, 1 = RGB, 2 = EGA text */
    uint8_t  color_info[6];     /* 110 */
} __attribute__((packed)) multiboot_info_t;

static multiboot_info_t *g_mbi = 0;

/* ================================================================== */
/* Timer tick counter (incremented by IRQ0 stub in idt_asm.asm)       */
/* ================================================================== */
volatile uint32_t tick_count = 0;
void timer_tick(void) { tick_count++; }

/* ================================================================== */
/* String / print helpers                                              */
/* ================================================================== */
static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int str_starts_with(const char *str, const char *prefix)
{
    while (*prefix) if (*str++ != *prefix++) return 0;
    return 1;
}

/* ---- Output abstraction: use GUI if fb is ready, else VGA text ---- */
static void kputs(const char *s, uint32_t gui_color)
{
    if (fb_ok()) gui_puts(s, gui_color);
    else         terminal_writestring(s);
}

static void kputchar(char c, uint32_t gui_color)
{
    if (fb_ok()) gui_putchar(c, gui_color);
    else         terminal_putchar(c);
}

static void print_uint(uint32_t val, uint32_t col)
{
    if (val == 0) { kputchar('0', col); return; }
    char buf[11]; int i = 10; buf[10] = '\0';
    while (val && i) { buf[--i] = (char)('0' + val % 10); val /= 10; }
    kputs(&buf[i], col);
}

static void print_hex32(uint32_t val, uint32_t col)
{
    char buf[9]; buf[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        uint32_t n = val & 0xF;
        buf[i] = (char)(n < 10 ? '0' + n : 'A' + n - 10);
        val >>= 4;
    }
    kputs(buf, col);
}

static void print_hex8(uint8_t val, uint32_t col)
{
    static const char h[] = "0123456789ABCDEF";
    char buf[3] = { h[val >> 4], h[val & 0xF], '\0' };
    kputs(buf, col);
}

static void print_ip(uint32_t ip)
{
    print_uint((ip >> 24) & 0xFF, COL_WHITE); kputchar('.', COL_TEXT);
    print_uint((ip >> 16) & 0xFF, COL_WHITE); kputchar('.', COL_TEXT);
    print_uint((ip >>  8) & 0xFF, COL_WHITE); kputchar('.', COL_TEXT);
    print_uint( ip        & 0xFF, COL_WHITE);
}

/* ================================================================== */
/* Banner (GUI only: drawn as part of gui_init decoration)            */
/* For VGA-text fallback we still print the ASCII art                 */
/* ================================================================== */
static void print_text_banner(void)
{
    if (fb_ok()) return;   /* GUI mode — no separate banner needed */
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writestring("  ___    ______  _____  _  _\n");
    terminal_writestring(" / _ |  /_  /  ||_   _|| || |\n");
    terminal_writestring("/ __ | _/ /  ||   | |   \\  /\n");
    terminal_writestring("/_/ |_|/___| |__|  |_|   \\/\n");
    terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ================================================================== */
/* Shell commands                                                      */
/* ================================================================== */
static void cmd_help(void)
{
    kputs("Available commands:\n", COL_GREEN);
    kputs("  help            - Show this help\n",            COL_TEXT);
    kputs("  about           - About Azix OS\n",             COL_TEXT);
    kputs("  version         - Version information\n",       COL_TEXT);
    kputs("  clear           - Clear the screen\n",          COL_TEXT);
    kputs("  color           - Show colour palette\n",       COL_TEXT);
    kputs("  ram             - RAM information\n",            COL_TEXT);
    kputs("  cpu             - CPU information\n",            COL_TEXT);
    kputs("  uptime          - System uptime\n",             COL_TEXT);
    kputs("  echo <text>     - Print text\n",                COL_TEXT);
    kputs("  reboot          - Reboot (asks y/n)\n",         COL_TEXT);
    kputs("  shutdown        - Shutdown (asks y/n)\n",       COL_TEXT);
    kputs("  halt            - Halt CPU\n",                  COL_TEXT);
    kputs("  mem             - Heap memory stats\n",          COL_TEXT);
    kputs("  pci             - List PCI devices\n",           COL_TEXT);
    kputs("  ifconfig        - Network interface info\n",     COL_TEXT);
    kputs("  ping <ip>       - Ping an IP address\n",         COL_TEXT);
    kputs("  browse <url>    - Fetch and display a web page\n", COL_TEXT);
}

static void cmd_about(void)
{
    kputs("Azix OS\n", COL_CYAN);
    kputs("  A minimal 32-bit x86 operating system.\n", COL_TEXT);
    kputs("  Arch        : x86 32-bit protected mode\n",   COL_TEXT);
    kputs("  Bootloader  : GNU GRUB (Multiboot v1)\n",     COL_TEXT);
    kputs("  Display     : VESA linear framebuffer\n",     COL_TEXT);
    kputs("  Keyboard    : PS/2 via IRQ1\n",               COL_TEXT);
}

static void cmd_version(void)
{
    kputs("Azix OS Version Info\n", COL_CYAN);
    kputs("  Version : 0.2.0\n", COL_TEXT);
    kputs("  Build   : April 2026\n", COL_TEXT);
    kputs("  Kernel  : Custom (C + NASM)\n", COL_TEXT);
    kputs("  GUI     : Framebuffer + 8x8 bitmap font\n", COL_TEXT);
}

static void cmd_color(void)
{
    kputs("Colour sample (GUI: live colours):\n", COL_CYAN);
    if (fb_ok()) {
        struct { uint32_t c; const char *n; } pal[] = {
            {COL_TEXT,     "LIGHT_GREY"}, {COL_WHITE,   "WHITE"},
            {COL_CYAN,     "CYAN"},       {COL_GREEN,   "GREEN"},
            {COL_RED,      "RED"},        {COL_YELLOW,  "YELLOW"},
            {COL_TEXT_DIM, "DARK_GREY"},  {COL_PROMPT,  "PROMPT"},
        };
        for (int i = 0; i < 8; i++) {
            kputs("  [", COL_TEXT);
            kputs(pal[i].n, pal[i].c);
            kputs("]\n", COL_TEXT);
        }
    } else {
        /* VGA text fallback — 16 standard colours */
        for (int i = 0; i < 16; i++) {
            terminal_setcolor((vga_color)i, VGA_COLOR_BLACK);
            terminal_writestring("  colour sample\n");
        }
        terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    }
}

static void cmd_ram(void)
{
    kputs("Memory Information\n", COL_CYAN);
    if (!g_mbi || !(g_mbi->flags & 0x1)) {
        kputs("  RAM info not available.\n", COL_RED);
        return;
    }
    uint32_t lower_kb = g_mbi->mem_lower;
    uint32_t upper_kb = g_mbi->mem_upper;
    uint32_t total_kb = lower_kb + upper_kb;
    kputs("  Lower memory : ", COL_TEXT); print_uint(lower_kb, COL_WHITE);
    kputs(" KB\n", COL_TEXT);
    kputs("  Upper memory : ", COL_TEXT); print_uint(upper_kb / 1024, COL_WHITE);
    kputs(" MB\n", COL_TEXT);
    kputs("  Total        : ", COL_TEXT); print_uint(total_kb / 1024, COL_WHITE);
    kputs(" MB\n", COL_TEXT);

    /* Framebuffer info */
    if (fb_ok()) {
        kputs("  Framebuffer  : ", COL_TEXT);
        print_uint(fb_width(), COL_WHITE); kputs("x", COL_TEXT);
        print_uint(fb_height(), COL_WHITE);
        kputs(" @ 32bpp\n", COL_TEXT);
    }
}

static void cmd_cpu(void)
{
    uint32_t eax, ebx, ecx, edx;
    kputs("CPU Information\n", COL_CYAN);

    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    char vendor[13];
    *(uint32_t *)(vendor+0) = ebx;
    *(uint32_t *)(vendor+4) = edx;
    *(uint32_t *)(vendor+8) = ecx;
    vendor[12] = '\0';
    kputs("  Vendor   : ", COL_TEXT); kputs(vendor, COL_WHITE); kputchar('\n', COL_TEXT);

    __asm__ volatile("cpuid" : "=a"(eax) : "a"(0x80000000) : "ebx","ecx","edx");
    if (eax >= 0x80000004) {
        char brand[49]; uint32_t *p = (uint32_t *)brand;
        __asm__ volatile("cpuid" : "=a"(p[0]),"=b"(p[1]),"=c"(p[2]),"=d"(p[3]) : "a"(0x80000002));
        __asm__ volatile("cpuid" : "=a"(p[4]),"=b"(p[5]),"=c"(p[6]),"=d"(p[7]) : "a"(0x80000003));
        __asm__ volatile("cpuid" : "=a"(p[8]),"=b"(p[9]),"=c"(p[10]),"=d"(p[11]) : "a"(0x80000004));
        brand[48] = '\0';
        const char *bp = brand; while (*bp == ' ') bp++;
        kputs("  Model    : ", COL_TEXT); kputs(bp, COL_WHITE); kputchar('\n', COL_TEXT);
    }

    __asm__ volatile("cpuid"
        : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(1));
    kputs("  Family   : ", COL_TEXT); print_uint((eax>>8)&0xF, COL_WHITE); kputchar('\n', COL_TEXT);
    kputs("  Model    : ", COL_TEXT); print_uint((eax>>4)&0xF, COL_WHITE); kputchar('\n', COL_TEXT);
    kputs("  Stepping : ", COL_TEXT); print_uint(eax&0xF, COL_WHITE);      kputchar('\n', COL_TEXT);
    kputs("  CPUID    : 0x", COL_TEXT); print_hex32(eax, COL_WHITE);       kputchar('\n', COL_TEXT);
}

static void cmd_mem(void)
{
    kputs("Heap Memory\n", COL_CYAN);
    size_t total = heap_total();
    size_t used  = heap_used();
    size_t free  = heap_free_bytes();
    kputs("  Total : ", COL_TEXT); print_uint((uint32_t)(total / 1024), COL_WHITE); kputs(" KB\n", COL_TEXT);
    kputs("  Used  : ", COL_TEXT); print_uint((uint32_t)(used  / 1024), COL_WHITE); kputs(" KB\n", COL_TEXT);
    kputs("  Free  : ", COL_TEXT); print_uint((uint32_t)(free  / 1024), COL_WHITE); kputs(" KB\n", COL_TEXT);
}

static void cmd_uptime(void)
{
    uint32_t ticks = tick_count;
    uint32_t s = ticks / 18, m = s / 60, h = m / 60;
    s %= 60; m %= 60;
    kputs("System Uptime\n", COL_CYAN);
    kputs("  ", COL_TEXT);
    print_uint(h, COL_WHITE); kputs("h ", COL_TEXT);
    print_uint(m, COL_WHITE); kputs("m ", COL_TEXT);
    print_uint(s, COL_WHITE); kputs("s  (", COL_TEXT_DIM);
    print_uint(ticks, COL_TEXT_DIM);
    kputs(" ticks)\n", COL_TEXT_DIM);
}

static int confirm(const char *msg)
{
    kputs(msg, COL_YELLOW);
    kputs(" [y/n]: ", COL_YELLOW);
    char c;
    do { c = keyboard_getchar(); } while (c!='y'&&c!='Y'&&c!='n'&&c!='N');
    kputchar(c, COL_WHITE); kputchar('\n', COL_TEXT);
    return (c=='y' || c=='Y');
}

static void cmd_reboot(void)
{
    if (!confirm("Reboot the system?")) {
        kputs("Cancelled.\n", COL_TEXT_DIM); return;
    }
    kputs("Rebooting...\n", COL_RED);
    __asm__ volatile("cli");
    while (inb(0x64) & 0x02);
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile("hlt");
}

static void cmd_shutdown(void)
{
    if (!confirm("Shut down the system?")) {
        kputs("Cancelled.\n", COL_TEXT_DIM); return;
    }
    kputs("Shutting down...\n", COL_RED);
    __asm__ volatile("cli");
    outw(0x4004, 0x3400);   /* VirtualBox PIIX4 ACPI S5 */
    outw(0x604,  0x2000);   /* QEMU/Bochs fallback       */
    for (;;) __asm__ volatile("hlt");
}

/* ================================================================== */
/* Shell loop                                                          */
/* ================================================================== */
static void shell_run(void)
{
    char cmd[CMD_BUF_SIZE];

    while (1) {
        /* Prompt */
        kputs("azix", COL_PROMPT);
        kputs("> ", COL_WHITE);
        if (fb_ok()) gui_redraw_cursor(1);

        /* Read line */
        size_t i = 0;
        for (;;) {
            if (fb_ok()) gui_redraw_cursor(1);
            char c = keyboard_getchar();
            if (fb_ok()) gui_redraw_cursor(0);

            if (c == '\n') { kputchar('\n', COL_TEXT); break; }
            if (c == '\b') { if (i > 0) { i--; kputchar('\b', COL_TEXT); } continue; }
            if (i < CMD_BUF_SIZE - 1) { cmd[i++] = c; kputchar(c, COL_WHITE); }
        }
        cmd[i] = '\0';
        if (i == 0) continue;

        /* Dispatch */
        if      (str_eq(cmd,"help"))         cmd_help();
        else if (str_eq(cmd,"about"))        cmd_about();
        else if (str_eq(cmd,"version"))      cmd_version();
        else if (str_eq(cmd,"clear"))        {
            if (fb_ok()) gui_clear();
            else terminal_clear();
        }
        else if (str_eq(cmd,"color"))        cmd_color();
        else if (str_eq(cmd,"ram"))          cmd_ram();
        else if (str_eq(cmd,"cpu"))          cmd_cpu();
        else if (str_eq(cmd,"uptime"))       cmd_uptime();
        else if (str_eq(cmd,"reboot"))       cmd_reboot();
        else if (str_eq(cmd,"shutdown"))     cmd_shutdown();
        else if (str_starts_with(cmd,"echo ")) {
            kputs(cmd+5, COL_TEXT); kputchar('\n', COL_TEXT);
        }
        else if (str_eq(cmd,"echo"))         kputchar('\n', COL_TEXT);
        else if (str_eq(cmd,"mem"))          cmd_mem();
        else if (str_eq(cmd,"pci"))          {
            pci_set_output(kputs, print_hex32);
            pci_scan();
        }
        else if (str_eq(cmd,"ifconfig")) {
            if (!pcnet_ready) {
                kputs("  No NIC found.\n", COL_RED);
            } else {
                uint8_t m[6];
                pcnet_get_mac(m);
                kputs("  eth0  MAC : ", COL_TEXT);
                for (int i = 0; i < 6; i++) {
                    print_hex8(m[i], COL_WHITE);
                    if (i < 5) kputchar(':', COL_TEXT);
                }
                kputs("\n        IP  : ", COL_TEXT);
                if (my_ip_addr) {
                    print_ip(my_ip_addr);
                    kputs("\n        GW  : ", COL_TEXT);
                    print_ip(my_gateway);
                    kputs("\n        Mask: ", COL_TEXT);
                    print_ip(my_netmask);
                } else {
                    kputs("(not configured)", COL_TEXT_DIM);
                }
                kputs("\n        State: UP (AMD PCNet)\n", COL_GREEN);
            }
        }
        else if (str_starts_with(cmd,"ping ") || str_eq(cmd,"ping")) {
            if (!pcnet_ready) {
                kputs("  No NIC.\n", COL_RED);
            } else if (str_eq(cmd,"ping")) {
                kputs("  Usage: ping <host> [count]  e.g.  ping google.com  ping 8.8.8.8 4\n", COL_TEXT);
            } else {
                /* --- Parse "ping <host> [count]" --- */
                const char *arg = cmd + 5;   /* skip "ping " */

                /* Split host from optional count */
                char host[128];
                uint32_t count = 4;          /* default 4 packets */
                int hi = 0;
                while (*arg && *arg != ' ' && hi < 127)
                    host[hi++] = *arg++;
                host[hi] = '\0';
                if (*arg == ' ') {
                    arg++;
                    uint32_t n = 0;
                    while (*arg >= '0' && *arg <= '9')
                        n = n * 10 + (uint32_t)(*arg++ - '0');
                    if (n > 0 && n <= 100) count = n;
                }

                /* --- Resolve host to IP --- */
                uint32_t ip = parse_ip(host);
                if (!ip) {
                    kputs("  Resolving ", COL_TEXT);
                    kputs(host, COL_WHITE);
                    kputs("...\n", COL_TEXT);
                    if (!net_dns_resolve(host, &ip)) {
                        kputs("  DNS: could not resolve hostname.\n", COL_RED);
                        goto ping_done;
                    }
                    kputs("  Address: ", COL_TEXT);
                    print_ip(ip);
                    kputchar('\n', COL_TEXT);
                }

                /* --- Send 'count' pings --- */
                kputs("  Pinging ", COL_TEXT);
                print_ip(ip);
                kputs(" with ", COL_TEXT);
                print_uint(count, COL_WHITE);
                kputs(" packet(s):\n", COL_TEXT);

                uint32_t ok = 0, lost = 0, total_rtt = 0;
                for (uint32_t n = 0; n < count; n++) {
                    int rtt = net_ping(ip);
                    if (rtt == -3) {
                        kputs("\n^C\n  Ping cancelled.\n", COL_RED);
                        keyboard_ctrl_c_clear();
                        goto ping_stats;
                    } else if (rtt < 0) {
                        kputs("    Request timed out.\n", COL_RED);
                        lost++;
                    } else {
                        kputs("    Reply from ", COL_GREEN);
                        print_ip(ip);
                        kputs("  time=", COL_GREEN);
                        if (rtt == 0)
                            kputs("<1ms", COL_WHITE);
                        else {
                            print_uint((uint32_t)rtt, COL_WHITE);
                            kputs("ms", COL_TEXT);
                        }
                        kputs("  seq=", COL_TEXT);
                        print_uint(n + 1, COL_WHITE);
                        kputchar('\n', COL_TEXT);
                        ok++;
                        total_rtt += (uint32_t)(rtt < 1 ? 1 : rtt);
                    }
                }
                ping_stats:
                kputs("  --- Stats: ", COL_TEXT);
                print_uint(count, COL_WHITE); kputs(" sent, ", COL_TEXT);
                print_uint(ok, COL_GREEN);    kputs(" received, ", COL_TEXT);
                print_uint(lost, lost ? COL_RED : COL_TEXT);
                kputs(" lost", COL_TEXT);
                if (ok) {
                    kputs("  avg=", COL_TEXT);
                    print_uint(total_rtt / ok, COL_WHITE);
                    kputs("ms", COL_TEXT);
                }
                kputs(" ---\n", COL_TEXT);
                keyboard_ctrl_c_clear();
                ping_done:;
            }
        }
        else if (str_starts_with(cmd,"browse ") || str_eq(cmd,"browse")) {
            if (!pcnet_ready) {
                kputs("  No NIC available.\n", COL_RED);
            } else if (str_eq(cmd,"browse")) {
                kputs("  Usage: browse <url>\n", COL_TEXT);
                kputs("  Examples:\n", COL_TEXT);
                kputs("    browse example.com\n", COL_TEXT);
                kputs("    browse http://neverssl.com\n", COL_TEXT);
            } else {
                const char *url = cmd + 7;  /* skip "browse " */
                keyboard_ctrl_c_clear();
                browser_get(url);
                keyboard_ctrl_c_clear();
            }
        }
        else if (str_eq(cmd,"halt")) {
            kputs("System halted.\n", COL_RED);
            __asm__ volatile("cli");
            for (;;) __asm__ volatile("hlt");
        } else {
            kputs("Unknown: '", COL_RED); kputs(cmd, COL_WHITE);
            kputs("'  (type 'help')\n", COL_RED);
        }
    }
}

/* ================================================================== */
/* Kernel entry                                                        */
/* ================================================================== */
void kernel_main(multiboot_info_t *mbi)
{
    g_mbi = mbi;

    /* ---- Try to set up framebuffer ---- */
    /* Accept type 0 (indexed) or type 1 (direct RGB) when bpp == 32;    */
    /* type 2 is EGA text and cannot be used as a pixel framebuffer.      */
    if (mbi && (mbi->flags & (1 << 12)) &&
        mbi->framebuffer_type != 2 &&
        mbi->framebuffer_bpp == 32 &&
        mbi->framebuffer_addr_lo != 0) {
        uint32_t *fb_addr = (uint32_t *)(uintptr_t)mbi->framebuffer_addr_lo;
        fb_init(fb_addr,
                mbi->framebuffer_pitch,
                mbi->framebuffer_width,
                mbi->framebuffer_height,
                mbi->framebuffer_bpp);
    }

    /* ---- Init subsystems ---- */
    /* Only fall back to VGA text if we have no framebuffer; don't do     */
    /* both — in gfxterm mode, VGA writes would be invisible anyway.      */
    /* Heap must be first — everything else may use kmalloc */
    heap_init((void *)_kernel_end, HEAP_SIZE);

    if (!fb_ok()) terminal_init();
    idt_init();
    keyboard_init();
    __asm__ volatile("sti");
    if (fb_ok()) mouse_init();   /* mouse needs sti + framebuffer ready */

    /* Initialize NIC (polling mode, no IRQ needed) */
    pcnet_init();
    /* Static IP config — VirtualBox NAT defaults */
    if (pcnet_ready)
        net_init(NET_MY_IP, NET_GATEWAY, NET_NETMASK);

    /* ---- Draw GUI or text banner ---- */
    if (fb_ok()) {
        gui_init();
        gui_puts("Welcome to ", COL_TEXT);
        gui_puts("Azix OS", COL_CYAN);
        gui_puts(" v0.2 - type '", COL_TEXT);
        gui_puts("help", COL_GREEN);
        gui_puts("' to begin.\n", COL_TEXT);
    } else {
        print_text_banner();
        terminal_writestring("Welcome to Azix OS. Type 'help'.\n");
    }

    shell_run();
}
