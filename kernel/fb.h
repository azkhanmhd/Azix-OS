#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stddef.h>

/* Pack R,G,B into a 32-bpp pixel (0x00RRGGBB — XRGB in little-endian memory
   comes out as B,G,R,X which is what VirtualBox/VESA expect). */
#define RGB(r,g,b) ((uint32_t)(((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b)))

/* ---- Palette ---- */
#define COL_BLACK       RGB(0x00,0x00,0x00)
#define COL_WHITE       RGB(0xFF,0xFF,0xFF)
#define COL_DESKTOP     RGB(0x1E,0x34,0x64)   /* clearly visible dark blue  */
#define COL_TOPBAR      RGB(0x14,0x30,0x78)   /* distinct blue top bar      */
#define COL_TOPBAR_TXT  RGB(0xFF,0xFF,0xFF)   /* white title text           */
#define COL_WIN_BG      RGB(0x0C,0x15,0x26)   /* dark but blue-tinted bg    */
#define COL_WIN_BORDER  RGB(0x40,0x90,0xD0)   /* bright blue border         */
#define COL_TITLEBAR    RGB(0x1C,0x4A,0x98)   /* medium blue title bar      */
#define COL_TITLE_TXT   RGB(0xFF,0xFF,0xFF)   /* white title text           */
#define COL_PROMPT      RGB(0x40,0xFF,0x70)   /* bright green prompt        */
#define COL_TEXT        RGB(0xE0,0xE0,0xE0)   /* light grey text            */
#define COL_TEXT_DIM    RGB(0x80,0x80,0x90)   /* dimmed text                */
#define COL_CYAN        RGB(0x40,0xE8,0xFF)   /* heading / info text        */
#define COL_GREEN       RGB(0x40,0xFF,0x70)
#define COL_RED         RGB(0xFF,0x60,0x60)
#define COL_YELLOW      RGB(0xFF,0xE0,0x40)
#define COL_CURSOR_FG   RGB(0x0C,0x15,0x26)   /* cursor fg (matches win bg) */
#define COL_CURSOR_BG   RGB(0xE0,0xE0,0xE0)   /* cursor bar                 */

/* ---- Glyph size ---- */
#define FONT_W 8
#define FONT_H 8

/* ---- Low-level pixel API ---- */
void fb_init(uint32_t *addr, uint32_t pitch, uint32_t width,
             uint32_t height, uint8_t bpp);
int      fb_ok(void);
uint32_t fb_width(void);
uint32_t fb_height(void);
void     fb_putpixel(int x, int y, uint32_t color);
void     fb_fillrect(int x, int y, int w, int h, uint32_t color);
void     fb_drawrect(int x, int y, int w, int h, uint32_t color);
void     fb_putchar(int x, int y, char c, uint32_t fg, uint32_t bg);
void     fb_puts(int x, int y, const char *s, uint32_t fg, uint32_t bg);

/* ---- GUI terminal (virtual 80×40 text buffer on the framebuffer) ---- */
#define TERM_COLS 90
#define TERM_ROWS 44

/* ---- Hit-testable rectangle (used for mouse buttons) ---- */
typedef struct { int x, y, w, h; } fb_rect_t;

/* Clickable button regions — set by gui_init(), read by mouse driver   */
extern fb_rect_t btn_reboot_rect;
extern fb_rect_t btn_shutdown_rect;

int fb_rect_contains(const fb_rect_t *r, int x, int y);

void gui_init(void);      /* draw desktop + window frame, clear buffer */
void gui_putchar(char c, uint32_t fg); /* write char at cursor, advance */
void gui_puts(const char *s, uint32_t fg);
void gui_newline(void);
void gui_clear(void);
void gui_redraw_cursor(int show); /* toggle text cursor block */
void gui_move_cursor(int x, int y); /* move/redraw hardware mouse cursor */

/* cursor position (read by shell to echo input) */
extern int gui_col, gui_row;

#endif /* FB_H */
