#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* One entry in the Interrupt Descriptor Table */
struct idt_entry {
    uint16_t base_lo;   /* lower 16 bits of handler address  */
    uint16_t sel;       /* kernel code segment selector       */
    uint8_t  always0;   /* must be 0                          */
    uint8_t  flags;     /* type + DPL + present bit           */
    uint16_t base_hi;   /* upper 16 bits of handler address   */
} __attribute__((packed));

/* IDTR register value */
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

void idt_init(void);

#endif /* IDT_H */
