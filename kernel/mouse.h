#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

void    mouse_init(void);
void    mouse_handler(void);   /* called from IRQ12 stub in idt_asm.asm */
int     mouse_x(void);
int     mouse_y(void);
uint8_t mouse_buttons(void);

#endif /* MOUSE_H */
