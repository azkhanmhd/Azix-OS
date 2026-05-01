/*
 * fb.c  —  Framebuffer driver + GUI terminal for Azix OS
 *
 * Font data: classic 8x8 IBM VGA bitmap glyphs (public domain).
 * Derived from the font8x8 project (MIT/public domain):
 *   https://github.com/dhepper/font8x8
 */
#include "fb.h"
#include <stdint.h>
#include <stddef.h>

/* ================================================================== */
/* 8x8 bitmap font — ASCII 0x20 (space) through 0x7E (~)              */
/* Each entry is 8 bytes; bit 7 of each byte = leftmost pixel.        */
/* ================================================================== */
static const uint8_t font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x20 space  */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 0x21 !      */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x22 "      */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* 0x23 #      */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* 0x24 $      */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* 0x25 %      */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* 0x26 &      */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* 0x27 '      */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* 0x28 (      */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* 0x29 )      */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 0x2A *      */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* 0x2B +      */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* 0x2C ,      */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* 0x2D -      */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* 0x2E .      */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* 0x2F /      */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 0x30 0      */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 0x31 1      */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 0x32 2      */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 0x33 3      */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 0x34 4      */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 0x35 5      */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 0x36 6      */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 0x37 7      */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 0x38 8      */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 0x39 9      */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* 0x3A :      */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* 0x3B ;      */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* 0x3C <      */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* 0x3D =      */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 0x3E >      */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* 0x3F ?      */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* 0x40 @      */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* 0x41 A      */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* 0x42 B      */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* 0x43 C      */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* 0x44 D      */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* 0x45 E      */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* 0x46 F      */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* 0x47 G      */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* 0x48 H      */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 0x49 I      */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* 0x4A J      */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* 0x4B K      */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* 0x4C L      */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* 0x4D M      */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* 0x4E N      */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* 0x4F O      */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* 0x50 P      */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* 0x51 Q      */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* 0x52 R      */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* 0x53 S      */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 0x54 T      */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* 0x55 U      */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 0x56 V      */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* 0x57 W      */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* 0x58 X      */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* 0x59 Y      */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* 0x5A Z      */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* 0x5B [      */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* 0x5C \      */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* 0x5D ]      */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* 0x5E ^      */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 0x5F _      */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* 0x60 `      */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* 0x61 a      */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* 0x62 b      */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* 0x63 c      */
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, /* 0x64 d      */
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, /* 0x65 e      */
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00}, /* 0x66 f      */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* 0x67 g      */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* 0x68 h      */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* 0x69 i      */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* 0x6A j      */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* 0x6B k      */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 0x6C l      */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* 0x6D m      */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* 0x6E n      */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* 0x6F o      */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* 0x70 p      */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* 0x71 q      */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* 0x72 r      */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* 0x73 s      */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* 0x74 t      */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* 0x75 u      */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 0x76 v      */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* 0x77 w      */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* 0x78 x      */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* 0x79 y      */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* 0x7A z      */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* 0x7B {      */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* 0x7C |      */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* 0x7D }      */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x7E ~      */
};

/* ================================================================== */
/* State                                                               */
/* ================================================================== */
static uint32_t *fb_base   = 0;
static uint32_t  fb_pitch  = 0;   /* bytes per row        */
static uint32_t  fb_w      = 0;
static uint32_t  fb_h      = 0;
static uint8_t   fb_bpp    = 0;

/* GUI terminal state */
int gui_col = 0;
int gui_row = 0;

/* Clickable button regions (set in draw_desktop) */
fb_rect_t btn_reboot_rect;
fb_rect_t btn_shutdown_rect;

/* Hardware mouse cursor state */
static int hw_cur_x          = -1; /* -1 = not yet positioned        */
static int hw_cur_y          = -1;
static int hw_cur_visible    =  0; /* 1 = drawn on screen right now  */
static int cursor_hide_depth =  0; /* nesting: hide before any draw  */

/* Forward declarations — defined later in this file */
static void cursor_push_hide(void);
static void cursor_pop_show(void);

/* character + colour buffer */
static uint8_t  tbuf_ch [TERM_ROWS][TERM_COLS];
static uint32_t tbuf_fg [TERM_ROWS][TERM_COLS];

/* layout (set in gui_init) */
static int win_x, win_y, win_w, win_h;   /* outer window box     */
static int txt_x, txt_y;                 /* text area top-left   */
static int txt_rows, txt_cols;            /* usable rows/cols     */

/* ================================================================== */
/* Low-level pixel / rect                                              */
/* ================================================================== */
void fb_init(uint32_t *addr, uint32_t pitch, uint32_t width,
             uint32_t height, uint8_t bpp)
{
    fb_base  = addr;
    fb_pitch = pitch;
    fb_w     = width;
    fb_h     = height;
    fb_bpp   = bpp;
}

int      fb_ok(void)     { return fb_base != 0; }
uint32_t fb_width(void)  { return fb_w; }
uint32_t fb_height(void) { return fb_h; }

void fb_putpixel(int x, int y, uint32_t color)
{
    if ((unsigned)x >= fb_w || (unsigned)y >= fb_h) return;
    uint32_t *row = (uint32_t *)((uint8_t *)fb_base + (uint32_t)y * fb_pitch);
    row[x] = color;
}

void fb_fillrect(int x, int y, int w, int h, uint32_t color)
{
    for (int row = y; row < y + h; row++) {
        if ((unsigned)row >= fb_h) break;
        uint32_t *line = (uint32_t *)((uint8_t *)fb_base + (uint32_t)row * fb_pitch);
        for (int col = x; col < x + w; col++) {
            if ((unsigned)col >= fb_w) break;
            line[col] = color;
        }
    }
}

void fb_drawrect(int x, int y, int w, int h, uint32_t color)
{
    fb_fillrect(x,     y,     w, 1, color);  /* top    */
    fb_fillrect(x,     y+h-1, w, 1, color);  /* bottom */
    fb_fillrect(x,     y,     1, h, color);  /* left   */
    fb_fillrect(x+w-1, y,     1, h, color);  /* right  */
}

void fb_putchar(int x, int y, char c, uint32_t fg, uint32_t bg)
{
    unsigned idx = (unsigned)c - 0x20;
    if (idx >= 96) idx = 0;  /* unknown → space */
    const uint8_t *glyph = font8x8[idx];
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        uint32_t *line = (uint32_t *)((uint8_t *)fb_base +
                          (uint32_t)(y + row) * fb_pitch);
        for (int col = 0; col < FONT_W; col++) {
            int px = x + col;
            if ((unsigned)px >= fb_w) break;
            line[px] = (bits & (1 << col)) ? fg : bg;
        }
    }
}

void fb_puts(int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    while (*s) {
        fb_putchar(x, y, *s++, fg, bg);
        x += FONT_W;
    }
}

/* ================================================================== */
/* GUI terminal                                                        */
/* ================================================================== */

/* Draw the desktop wallpaper (simple gradient-ish horizontal bands) */
static void draw_desktop(void)
{
    /* Background fill */
    fb_fillrect(0, 0, (int)fb_w, (int)fb_h, COL_DESKTOP);

    /* ---- Top bar ---- */
    int bar_h = 28;
    fb_fillrect(0, 0, (int)fb_w, bar_h, COL_TOPBAR);
    /* 1px accent line at bottom of top bar */
    fb_fillrect(0, bar_h - 1, (int)fb_w, 1, COL_WIN_BORDER);
    /* OS name */
    fb_puts(10, 8, "Azix OS", COL_TOPBAR_TXT, COL_TOPBAR);

    /* ---- Buttons: Reboot and Shutdown in top-right ---- */
    /* "Reboot"   = 6 chars × 8px = 48px + 8px padding = 64px wide      */
    /* "Shutdown" = 8 chars × 8px = 64px + 8px padding = 80px wide      */
    int btn_h  = 18;
    int btn_y  = 5;
    int btn_rx = (int)fb_w - 160; /* Reboot button x                    */
    int btn_rw = 64;
    int btn_sx = (int)fb_w - 90;  /* Shutdown button x                  */
    int btn_sw = 82;

    /* Store rects for hit testing */
    btn_reboot_rect.x   = btn_rx; btn_reboot_rect.y   = btn_y;
    btn_reboot_rect.w   = btn_rw; btn_reboot_rect.h   = btn_h;
    btn_shutdown_rect.x = btn_sx; btn_shutdown_rect.y = btn_y;
    btn_shutdown_rect.w = btn_sw; btn_shutdown_rect.h = btn_h;

    /* Draw Reboot button */
    fb_fillrect(btn_rx, btn_y, btn_rw, btn_h, RGB(0x22,0x55,0x99));
    fb_drawrect(btn_rx, btn_y, btn_rw, btn_h, RGB(0x55,0xAA,0xFF));
    fb_puts(btn_rx + 8, btn_y + 5, "Reboot", COL_WHITE, RGB(0x22,0x55,0x99));

    /* Draw Shutdown button */
    fb_fillrect(btn_sx, btn_y, btn_sw, btn_h, RGB(0x88,0x22,0x22));
    fb_drawrect(btn_sx, btn_y, btn_sw, btn_h, RGB(0xFF,0x66,0x66));
    fb_puts(btn_sx + 9, btn_y + 5, "Shutdown", COL_WHITE, RGB(0x88,0x22,0x22));
}

/* Draw the terminal window chrome (title bar + border) */
static void draw_window_chrome(void)
{
    /* Outer border */
    fb_fillrect(win_x, win_y, win_w, win_h, COL_WIN_BG);
    fb_drawrect(win_x, win_y, win_w, win_h, COL_WIN_BORDER);
    /* 1px inner border accent */
    fb_drawrect(win_x+1, win_y+1, win_w-2, win_h-2, RGB(0x1A,0x30,0x55));

    /* Title bar */
    int tb_h = 22;
    fb_fillrect(win_x+2, win_y+2, win_w-4, tb_h, COL_TITLEBAR);
    /* Title bar gradient line */
    fb_fillrect(win_x+2, win_y+2, win_w-4, 1, RGB(0x28,0x50,0x90));
    fb_puts(win_x + 10, win_y + 7, "Terminal", COL_TITLE_TXT, COL_TITLEBAR);

    /* Close / minimise dots (decorative) */
    int bx = win_x + win_w - 16;
    int by = win_y + 9;
    for (int i = 0; i < 3; i++) {
        fb_fillrect(bx - i*12 - 2, by - 2, 6, 6,
                    i == 0 ? RGB(0xFF,0x55,0x55) :
                    i == 1 ? RGB(0xFF,0xCC,0x00) :
                             RGB(0x44,0xCC,0x44));
    }

    /* Separator line below title */
    fb_fillrect(win_x+2, win_y+2+tb_h, win_w-4, 1, COL_WIN_BORDER);

    /* Clear text area */
    fb_fillrect(txt_x, txt_y, txt_cols * FONT_W, txt_rows * FONT_H, COL_WIN_BG);
}

void gui_init(void)
{
    if (!fb_ok()) return;

    draw_desktop();

    /* Window geometry — leave 20px margin from edges, 36px from top (bar+gap) */
    int margin = 20;
    win_x = margin;
    win_y = 36;
    win_w = (int)fb_w - 2 * margin;
    win_h = (int)fb_h - 36 - margin;

    /* Text area starts after title bar (22px) + border (2px) + padding (4px) */
    txt_x = win_x + 6;
    txt_y = win_y + 2 + 22 + 5;
    txt_cols = (win_w - 12) / FONT_W;
    txt_rows = (win_h - 22 - 12) / FONT_H;

    /* Clamp to buffer size */
    if (txt_cols > TERM_COLS) txt_cols = TERM_COLS;
    if (txt_rows > TERM_ROWS) txt_rows = TERM_ROWS;

    draw_window_chrome();

    /* Clear buffers */
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++) {
            tbuf_ch[r][c] = ' ';
            tbuf_fg[r][c] = COL_TEXT;
        }

    gui_col = 0;
    gui_row = 0;
}

/* Redraw a single cell from the buffer */
static void redraw_cell(int row, int col)
{
    if (row >= txt_rows || col >= txt_cols) return;
    int px = txt_x + col * FONT_W;
    int py = txt_y + row * FONT_H;
    fb_putchar(px, py, (char)tbuf_ch[row][col], tbuf_fg[row][col], COL_WIN_BG);
}

static void scroll_up(void)
{
    cursor_push_hide();
    /* Move all rows up by 1 */
    for (int r = 0; r < txt_rows - 1; r++)
        for (int c = 0; c < txt_cols; c++) {
            tbuf_ch[r][c] = tbuf_ch[r+1][c];
            tbuf_fg[r][c] = tbuf_fg[r+1][c];
        }
    /* Clear last row */
    for (int c = 0; c < txt_cols; c++) {
        tbuf_ch[txt_rows-1][c] = ' ';
        tbuf_fg[txt_rows-1][c] = COL_TEXT;
    }
    /* Redraw entire text area */
    fb_fillrect(txt_x, txt_y, txt_cols * FONT_W, txt_rows * FONT_H, COL_WIN_BG);
    for (int r = 0; r < txt_rows; r++)
        for (int c = 0; c < txt_cols; c++)
            if (tbuf_ch[r][c] != ' ')
                redraw_cell(r, c);
    cursor_pop_show();
}

void gui_newline(void)
{
    gui_col = 0;
    if (gui_row < txt_rows - 1) {
        gui_row++;
    } else {
        scroll_up();
        /* gui_row stays at txt_rows-1 */
    }
}

void gui_putchar(char c, uint32_t fg)
{
    if (!fb_ok()) return;
    cursor_push_hide();

    if (c == '\n') {
        gui_newline();
    } else if (c == '\r') {
        gui_col = 0;
    } else if (c == '\b') {
        if (gui_col > 0) {
            gui_col--;
            tbuf_ch[gui_row][gui_col] = ' ';
            tbuf_fg[gui_row][gui_col] = COL_TEXT;
            redraw_cell(gui_row, gui_col);
        }
    } else {
        if (gui_col >= txt_cols) gui_newline();
        tbuf_ch[gui_row][gui_col] = (uint8_t)c;
        tbuf_fg[gui_row][gui_col] = fg;
        redraw_cell(gui_row, gui_col);
        gui_col++;
    }

    cursor_pop_show();
}

void gui_puts(const char *s, uint32_t fg)
{
    while (*s) gui_putchar(*s++, fg);
}

void gui_clear(void)
{
    cursor_push_hide();
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++) {
            tbuf_ch[r][c] = ' ';
            tbuf_fg[r][c] = COL_TEXT;
        }
    fb_fillrect(txt_x, txt_y, txt_cols * FONT_W, txt_rows * FONT_H, COL_WIN_BG);
    gui_col = 0;
    gui_row = 0;
    cursor_pop_show();
}

void gui_redraw_cursor(int show)
{
    int px = txt_x + gui_col * FONT_W;
    int py = txt_y + gui_row * FONT_H;
    if (show)
        fb_fillrect(px, py + FONT_H - 2, FONT_W, 2, COL_CURSOR_BG);
    else
        fb_fillrect(px, py + FONT_H - 2, FONT_W, 2, COL_WIN_BG);
}

/* ================================================================== */
/* Hardware mouse cursor — save-and-restore technique                  */
/*                                                                     */
/* Save the pixels behind the cursor before drawing, restore them on  */
/* erase.  This is immune to background redraws (unlike XOR cursors   */
/* which go out of sync whenever text or fills touch cursor pixels).  */
/* ================================================================== */
#define HW_CUR_W 11
#define HW_CUR_H 18

/* Arrow cursor shape — each byte is one row, bit0 = leftmost pixel.
   Two layers: FG (white body) and OUTLINE (black border, 1px wider). */
static const uint8_t hw_cur_fg[HW_CUR_H] = {
    0x01,        /*  #.......... */
    0x03,        /*  ##......... */
    0x07,        /*  ###........ */
    0x0F,        /*  ####....... */
    0x1F,        /*  #####...... */
    0x3F,        /*  ######..... */
    0x7F,        /*  #######.... */
    0xFF,        /*  ########... */
    0x1F,        /*  #####...... */
    0x1B,        /*  ##.##...... */
    0x31,        /*  #...##..... */
    0x30,        /*  ....##..... */
    0x60,        /*  .....##.... */
    0x60,        /*  .....##.... */
    0xC0,        /*  ......##... */
    0x00,
    0x00,
    0x00,
};
/* Outline = 3×3 dilation of fg, then fg subtracted (bit-trick below) */

/* Saved background pixels (full bounding box) */
static uint32_t hw_cur_save[HW_CUR_W * HW_CUR_H];
static int      hw_cur_saved = 0; /* 1 when save buffer is valid */

/* Expand one row's fg bits to an outline: OR with neighbors */
static uint32_t outline_row(int r)
{
    uint32_t a  = (r > 0)              ? (uint32_t)hw_cur_fg[r-1] : 0u;
    uint32_t b  =                        (uint32_t)hw_cur_fg[r];
    uint32_t c  = (r < HW_CUR_H - 1)  ? (uint32_t)hw_cur_fg[r+1] : 0u;
    uint32_t m  = a | b | c;
    m = (m | (m << 1) | (m >> 1)) & ((1u << HW_CUR_W) - 1); /* expand, clamp to bbox */
    return m & ~(uint32_t)hw_cur_fg[r]; /* border only, not body */
}

static void cursor_save_draw(int x, int y)
{
    for (int r = 0; r < HW_CUR_H; r++) {
        int py = y + r;
        uint32_t outline = outline_row(r);
        for (int c = 0; c < HW_CUR_W; c++) {
            int px = x + c;
            int idx = r * HW_CUR_W + c;
            if (py < 0 || (uint32_t)py >= fb_h || px < 0 || (uint32_t)px >= fb_w) {
                hw_cur_save[idx] = 0;
                continue;
            }
            uint32_t *pixel = (uint32_t *)((uint8_t *)fb_base
                              + (uint32_t)py * fb_pitch) + px;
            hw_cur_save[idx] = *pixel; /* save original */
            uint32_t bit = 1u << c;
            if ((uint32_t)hw_cur_fg[r] & bit)
                *pixel = 0x00FFFFFF; /* white body   */
            else if (outline & bit)
                *pixel = 0x00000000; /* black border */
            /* else: transparent — leave pixel as-is (already saved) */
        }
    }
    hw_cur_saved = 1;
}

static void cursor_restore(int x, int y)
{
    if (!hw_cur_saved) return;
    for (int r = 0; r < HW_CUR_H; r++) {
        int py = y + r;
        for (int c = 0; c < HW_CUR_W; c++) {
            int px = x + c;
            if (py < 0 || (uint32_t)py >= fb_h || px < 0 || (uint32_t)px >= fb_w) continue;
            uint32_t *pixel = (uint32_t *)((uint8_t *)fb_base
                              + (uint32_t)py * fb_pitch) + px;
            *pixel = hw_cur_save[r * HW_CUR_W + c];
        }
    }
    hw_cur_saved = 0;
}

/* ---- cursor hide / show (for terminal drawing to call) ----------- */

/* Remove cursor from screen, mark invisible. No-op if already hidden. */
static void mouse_cursor_hide(void)
{
    if (!hw_cur_visible || hw_cur_x < 0) return;
    cursor_restore(hw_cur_x, hw_cur_y);
    hw_cur_visible = 0;
}

/* Draw cursor at current position. No-op if already visible. */
static void mouse_cursor_show(void)
{
    if (hw_cur_x < 0 || hw_cur_visible) return;
    cursor_save_draw(hw_cur_x, hw_cur_y);
    hw_cur_visible = 1;
}

/* Nesting-safe bracket: call before any fb write that may touch cursor pixels. */
static void cursor_push_hide(void)
{
    if (cursor_hide_depth++ == 0) mouse_cursor_hide();
}

static void cursor_pop_show(void)
{
    if (cursor_hide_depth > 0 && --cursor_hide_depth == 0) mouse_cursor_show();
}

/* Called from IRQ12. Moves cursor; if main thread is drawing, just stores
   the new position and defers the actual draw until pop_show(). */
void gui_move_cursor(int x, int y)
{
    if (!fb_ok()) return;
    if (hw_cur_visible) {                       /* erase at old position */
        cursor_restore(hw_cur_x, hw_cur_y);
        hw_cur_visible = 0;
    }
    hw_cur_x = x;
    hw_cur_y = y;
    if (cursor_hide_depth == 0) {               /* draw only if not bracketed */
        cursor_save_draw(x, y);
        hw_cur_visible = 1;
    }
}

/* ================================================================== */
/* Rectangle hit test                                                  */
/* ================================================================== */
int fb_rect_contains(const fb_rect_t *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w &&
           y >= r->y && y < r->y + r->h;
}
