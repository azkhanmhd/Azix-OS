#include <stdint.h>
#include "keyboard.h"
#include "io.h"

#define KEYBOARD_DATA_PORT 0x60
#define BUF_SIZE           256

/* Circular character buffer shared with keyboard_getchar() */
static volatile char    key_buffer[BUF_SIZE];
static volatile uint8_t buf_head = 0;
static volatile uint8_t buf_tail = 0;

static volatile uint8_t shift_active = 0;
static volatile uint8_t ctrl_active  = 0;
volatile int keyboard_ctrl_c_flag    = 0;   /* set when Ctrl+C pressed */

/* US QWERTY scancode → ASCII (no modifier) */
static const char normal_map[] = {
/*  0    1    2    3    4    5    6    7    8    9   10   11   12   13   14  */
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
/* 15   16   17   18   19   20   21   22   23   24   25   26   27   28      */
  '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
/* 29   30   31   32   33   34   35   36   37   38   39   40   41           */
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
/* 42   43   44   45   46   47   48   49   50   51   52   53               */
    0, '\\','z','x','c','v','b','n','m',',','.','/',
/* 54   55   56   57                                                        */
    0, '*',  0, ' '
};

/* US QWERTY scancode → ASCII (Shift held) */
static const char shift_map[] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
  '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?',
    0, '*',  0, ' '
};

#define MAP_SIZE ((int)(sizeof(normal_map)))

/* ------------------------------------------------------------------ */

void keyboard_init(void)
{
    buf_head     = 0;
    buf_tail     = 0;
    shift_active = 0;
}

/* Called from the IRQ1 stub in idt_asm.asm — runs with interrupts disabled */
void keyboard_handler(void)
{
    uint8_t sc = inb(KEYBOARD_DATA_PORT);

    /* Track Shift state (scancodes 0x2A / 0x36 = press, 0xAA / 0xB6 = release) */
    if (sc == 0x2A || sc == 0x36) { shift_active = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { shift_active = 0; return; }

    /* Track Ctrl state (Left Ctrl = 0x1D / 0x9D) */
    if (sc == 0x1D) { ctrl_active = 1; return; }
    if (sc == 0x9D) { ctrl_active = 0; return; }

    /* Ctrl+C: scancode 0x2E = 'c' */
    if (ctrl_active && sc == 0x2E) { keyboard_ctrl_c_flag = 1; return; }

    /* High bit set → key-release event; ignore */
    if (sc & 0x80) return;

    if (sc < MAP_SIZE) {
        char c = shift_active ? shift_map[sc] : normal_map[sc];
        if (c) {
            uint8_t next = (uint8_t)(buf_tail + 1);
            if (next != buf_head) {   /* drop if buffer is full */
                key_buffer[buf_tail] = c;
                buf_tail = next;
            }
        }
    }
}

/* Blocking read — halts the CPU until the next interrupt arrives.
 * With IF=1, 'hlt' tells the hypervisor to idle and deliver pending IRQs.
 * After iret from the IRQ1 handler, execution resumes and we re-check. */
char keyboard_getchar(void)
{
    while (buf_head == buf_tail)
        __asm__ volatile("hlt");    /* sleep until next IRQ wakes us */

    char c = key_buffer[buf_head];
    buf_head = (uint8_t)(buf_head + 1);
    return c;
}

void keyboard_ctrl_c_clear(void)
{
    keyboard_ctrl_c_flag = 0;
}
