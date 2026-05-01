#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_init(void);
void keyboard_handler(void);  /* called from IRQ1 stub in idt_asm.asm */
char keyboard_getchar(void);  /* blocking read — spins until a key is ready */

/* Set to 1 by the IRQ handler when Ctrl+C is pressed. */
extern volatile int keyboard_ctrl_c_flag;
/* Clear the flag (call after handling the cancel). */
void keyboard_ctrl_c_clear(void);

#endif /* KEYBOARD_H */
