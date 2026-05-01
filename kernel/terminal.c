#include <stdint.h>
#include <stddef.h>
#include "terminal.h"
#include "vga.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static uint16_t * const VGA_MEMORY = (uint16_t *)0xB8000;

static size_t  terminal_row;
static size_t  terminal_col;
static uint8_t terminal_color;

/* ------------------------------------------------------------------ */

size_t terminal_strlen(const char *str)
{
    size_t n = 0;
    while (str[n]) n++;
    return n;
}

void terminal_init(void)
{
    terminal_row   = 0;
    terminal_col   = 0;
    terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
}

void terminal_clear(void)
{
    terminal_row = 0;
    terminal_col = 0;

    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
}

void terminal_setcolor(vga_color fg, vga_color bg)
{
    terminal_color = vga_entry_color(fg, bg);
}

/* Scroll the screen up by one line */
static void terminal_scroll(void)
{
    for (size_t y = 1; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[(y - 1) * VGA_WIDTH + x] = VGA_MEMORY[y * VGA_WIDTH + x];

    /* Blank the last row */
    for (size_t x = 0; x < VGA_WIDTH; x++)
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', terminal_color);

    terminal_row = VGA_HEIGHT - 1;
}

void terminal_putchar(char c)
{
    if (c == '\n') {
        terminal_col = 0;
        if (++terminal_row == VGA_HEIGHT) terminal_scroll();
        return;
    }
    if (c == '\r') {
        terminal_col = 0;
        return;
    }
    if (c == '\b') {
        if (terminal_col > 0) {
            terminal_col--;
            VGA_MEMORY[terminal_row * VGA_WIDTH + terminal_col] =
                vga_entry(' ', terminal_color);
        }
        return;
    }

    VGA_MEMORY[terminal_row * VGA_WIDTH + terminal_col] = vga_entry(c, terminal_color);

    if (++terminal_col == VGA_WIDTH) {
        terminal_col = 0;
        if (++terminal_row == VGA_HEIGHT) terminal_scroll();
    }
}

void terminal_write(const char *data, size_t size)
{
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_writestring(const char *data)
{
    terminal_write(data, terminal_strlen(data));
}
