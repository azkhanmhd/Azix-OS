#ifndef TERMINAL_H
#define TERMINAL_H

#include <stddef.h>
#include "vga.h"

void   terminal_init(void);
void   terminal_clear(void);
void   terminal_setcolor(vga_color fg, vga_color bg);
void   terminal_putchar(char c);
void   terminal_write(const char *data, size_t size);
void   terminal_writestring(const char *data);
size_t terminal_strlen(const char *str);

#endif /* TERMINAL_H */
