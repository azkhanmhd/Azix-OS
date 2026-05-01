#include <stdint.h>
#include "idt.h"
#include "io.h"
#include "terminal.h"
#include "vga.h"

#define IDT_ENTRIES 256

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtp;

/* Defined in idt_asm.asm */
extern void idt_load(struct idt_ptr *);
extern void irq0(void);
extern void irq1(void);
extern void irq12(void);
extern void irq_default(void);

/* CPU exception stubs (ISR 0-31) — defined in idt_asm.asm */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

/* ---------------------------------------------------------------
 * exception_handler — called from common_isr_stub in idt_asm.asm.
 * Displays which exception fired, then halts the CPU permanently.
 * --------------------------------------------------------------- */
static const char *exception_names[32] = {
    "Division Error (#DE)",
    "Debug (#DB)",
    "Non-Maskable Interrupt",
    "Breakpoint (#BP)",
    "Overflow (#OF)",
    "Bound Range Exceeded (#BR)",
    "Invalid Opcode (#UD)",
    "Device Not Available (#NM)",
    "Double Fault (#DF)",
    "Coprocessor Segment Overrun",
    "Invalid TSS (#TS)",
    "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)",
    "General Protection Fault (#GP)",
    "Page Fault (#PF)",
    "Reserved (15)",
    "x87 FP Exception (#MF)",
    "Alignment Check (#AC)",
    "Machine Check (#MC)",
    "SIMD FP Exception (#XM)",
    "Virtualization Exception (#VE)",
    "Control Protection (#CP)",
    "Reserved (22)", "Reserved (23)", "Reserved (24)",
    "Reserved (25)", "Reserved (26)", "Reserved (27)",
    "Hypervisor Injection (#HV)",
    "VMM Communication (#VC)",
    "Security Exception (#SX)",
    "Reserved (31)"
};

static void print_hex32(uint32_t val)
{
    char buf[9];
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        uint32_t nibble = val & 0xF;
        buf[i] = (char)(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
        val >>= 4;
    }
    terminal_writestring(buf);
}

void exception_handler(uint32_t num, uint32_t err_code)
{
    terminal_setcolor(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_writestring("\n\n*** KERNEL EXCEPTION ***\n");
    terminal_writestring(num < 32 ? exception_names[num] : "Unknown exception");
    terminal_writestring("\nError code : 0x");
    print_hex32(err_code);
    terminal_writestring("\nSystem Halted.\n");
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt[num].base_lo = (uint16_t)(base & 0xFFFF);
    idt[num].base_hi = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].sel     = sel;
    idt[num].always0 = 0;
    idt[num].flags   = flags;
}

void idt_init(void)
{
    idtp.limit = (uint16_t)(sizeof(struct idt_entry) * IDT_ENTRIES - 1);
    idtp.base  = (uint32_t)&idt;

    /* Zero every gate */
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate((uint8_t)i, 0, 0, 0);

    /* Install CPU exception handlers (INT 0-31) so any exception shows
     * a message and halts instead of cascading to a triple fault.
     * Selector 0x10 = GRUB's 32-bit code segment (CS at boot).          */
    static void (*isrs[32])(void) = {
        isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
        isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    };
    for (int i = 0; i < 32; i++)
        idt_set_gate((uint8_t)i, (uint32_t)isrs[i], 0x10, 0x8E);

    /* -------------------------------------------------------
     * Remap the 8259 PIC so hardware IRQs don't conflict with
     * CPU exception vectors (which occupy INT 0x00–0x1F).
     *   Master PIC: IRQ0–7  → INT 0x20–0x27
     *   Slave  PIC: IRQ8–15 → INT 0x28–0x2F
     * ------------------------------------------------------- */
    outb(0x20, 0x11); outb(0xA0, 0x11);  /* ICW1: start initialisation */
    outb(0x21, 0x20); outb(0xA1, 0x28);  /* ICW2: vector offsets        */
    outb(0x21, 0x04); outb(0xA1, 0x02);  /* ICW3: cascade wiring        */
    outb(0x21, 0x01); outb(0xA1, 0x01);  /* ICW4: 8086 mode             */

    /* Mask all IRQs except IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade).
     * 0xF8 = 1111 1000 → bits 0+1+2 clear = IRQ0, IRQ1, IRQ2 unmasked.
     * IRQ2 must be unmasked so slave PIC interrupts (IRQ8-15) reach CPU. */
    outb(0x21, 0xF8);
    /* Slave PIC: unmask IRQ12 only (bit 4 = IRQ12-8).
     * 0xEF = 1110 1111 → bit 4 clear = IRQ12 unmasked.                 */
    outb(0xA1, 0xEF);

    /* Install default handler for ALL 16 IRQ vectors (0x20–0x2F).
     * This prevents triple faults from spurious/unhandled interrupts.
     * Flags: 0x8E = Present | Ring0 | 32-bit interrupt gate             */
    for (int i = 0x20; i <= 0x2F; i++)
        idt_set_gate((uint8_t)i, (uint32_t)irq_default, 0x10, 0x8E);

    /* Override IRQ0 (PIT timer) at vector 0x20                          */
    idt_set_gate(0x20, (uint32_t)irq0,  0x10, 0x8E);
    /* Override IRQ1 (keyboard) with the real handler at vector 0x21     */
    idt_set_gate(0x21, (uint32_t)irq1,  0x10, 0x8E);
    /* Override IRQ12 (PS/2 mouse) at vector 0x2C                        */
    idt_set_gate(0x2C, (uint32_t)irq12, 0x10, 0x8E);

    idt_load(&idtp);
}
