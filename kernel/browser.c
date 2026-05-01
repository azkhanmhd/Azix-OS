/*
 * browser.c — Text-mode web browser for Azix OS
 *
 * Features:
 *   - Strips "http://" prefix, splits host / path
 *   - DNS-resolves hostname if not a dotted-decimal IP
 *   - Opens TCP connection to port 80
 *   - Sends HTTP/1.0 GET (Connection: close keeps it simple)
 *   - Skips HTTP response headers (up to first blank line)
 *   - Strips HTML tags, <script>/<style> blocks, HTML entities
 *   - Word-wraps at COLS characters, paginates at ROWS lines
 *   - Ctrl+C cancels at any point
 */

#include "browser.h"
#include "tcp.h"
#include "net.h"
#include "pcnet.h"
#include "keyboard.h"
#include "fb.h"         /* gui_puts, gui_putchar, COL_* */
#include <stdint.h>
#include <stddef.h>

/* Thin wrappers so the rest of the file reads like kernel.c */
#define kputs(s,col)    gui_puts((s),(col))
#define kputchar(c,col) gui_putchar((c),(col))

/* Screen width for word-wrap / page height for pagination */
#define COLS 80
#define ROWS 22

/* ------------------------------------------------------------------ */
/* Small string helpers (no stdlib)                                    */
/* ------------------------------------------------------------------ */
/* Case-insensitive prefix match */
static int b_starts_with_ci(const char *str, const char *pfx)
{
    while (*pfx) {
        char sc = *str, pc = *pfx;
        if (sc >= 'A' && sc <= 'Z') sc += 32;
        if (pc >= 'A' && pc <= 'Z') pc += 32;
        if (sc != pc) return 0;
        str++; pfx++;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* HTTP response buffer (static, 192 KB)                               */
/* ------------------------------------------------------------------ */
#define HTTP_BUF_SZ (192 * 1024)
static char http_buf[HTTP_BUF_SZ];

/* ------------------------------------------------------------------ */
/* HTTP GET over TCP                                                    */
/* Returns bytes in http_buf, or <0 on error.                          */
/* ------------------------------------------------------------------ */
static int do_http_get(uint32_t ip, const char *host, const char *path)
{
    int rc = tcp_connect(ip, 80);
    if (rc != TCP_OK) return rc;
    kputs(" connected\n  Sending GET request...", COL_GREEN);
    static char req[512];
    char *p = req;

    /* GET /path HTTP/1.0\r\n */
    const char *method = "GET ";
    while (*method) *p++ = *method++;
    if (*path != '/') *p++ = '/';
    while (*path) *p++ = *path++;
    const char *ver = " HTTP/1.0\r\n";
    while (*ver) *p++ = *ver++;

    /* Host: header */
    const char *hdr_host = "Host: ";
    while (*hdr_host) *p++ = *hdr_host++;
    while (*host) *p++ = *host++;
    *p++ = '\r'; *p++ = '\n';

    /* User-Agent */
    const char *ua = "User-Agent: AzixBrowser/1.0\r\n";
    while (*ua) *p++ = *ua++;

    /* Accept */
    const char *acc = "Accept: text/html,text/plain\r\n";
    while (*acc) *p++ = *acc++;

    /* Connection close */
    const char *conn = "Connection: close\r\n\r\n";
    while (*conn) *p++ = *conn++;

    *p = '\0';

    rc = tcp_write((const uint8_t*)req, (uint32_t)(p - req));
    if (rc != TCP_OK) { tcp_close(); kputs(" FAILED\n", COL_RED); return rc; }

    kputs(" OK\n  Waiting for response...", COL_GREEN);
    int received = tcp_read((uint8_t*)http_buf, HTTP_BUF_SZ - 1);
    tcp_close();

    if (received < 0) return received;
    http_buf[received] = '\0';
    return received;
}

/* ------------------------------------------------------------------ */
/* Skip HTTP response headers — returns pointer to body start          */
/* ------------------------------------------------------------------ */
static const char *skip_headers(const char *resp, int len)
{
    /* Find \r\n\r\n or \n\n */
    for (int i = 0; i < len - 3; i++) {
        if (resp[i]=='\r' && resp[i+1]=='\n' && resp[i+2]=='\r' && resp[i+3]=='\n')
            return resp + i + 4;
        if (resp[i]=='\n' && resp[i+1]=='\n')
            return resp + i + 2;
    }
    return resp; /* no blank line found — treat everything as body */
}

/* ------------------------------------------------------------------ */
/* HTML entity decode (in-place, single pass)                          */
/* ------------------------------------------------------------------ */
static void decode_entities(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r != '&') { *w++ = *r++; continue; }
        /* try to match a known entity */
        if (b_starts_with_ci(r, "&amp;"))  { *w++ = '&';  r += 5; }
        else if (b_starts_with_ci(r,"&lt;"))   { *w++ = '<';  r += 4; }
        else if (b_starts_with_ci(r,"&gt;"))   { *w++ = '>';  r += 4; }
        else if (b_starts_with_ci(r,"&quot;")) { *w++ = '"';  r += 6; }
        else if (b_starts_with_ci(r,"&apos;")) { *w++ = '\''; r += 6; }
        else if (b_starts_with_ci(r,"&nbsp;")) { *w++ = ' ';  r += 6; }
        else if (b_starts_with_ci(r,"&#"))    {
            /* numeric entity &#N; or &#xN; */
            r += 2;
            int hex = (*r == 'x' || *r == 'X');
            if (hex) r++;
            uint32_t val = 0;
            while (*r && *r != ';') {
                if (*r>='0'&&*r<='9') val=val*(hex?16:10)+(*r-'0');
                else if(hex&&*r>='a'&&*r<='f') val=val*16+(*r-'a'+10);
                else if(hex&&*r>='A'&&*r<='F') val=val*16+(*r-'A'+10);
                r++;
            }
            if (*r==';') r++;
            /* Output ASCII-safe subset */
            if (val >= 32 && val < 127) *w++ = (char)val;
            else *w++ = '?';
        }
        else { *w++ = *r++; }
    }
    *w = '\0';
}

/* ------------------------------------------------------------------ */
/* Strip HTML tags and block elements — output to out[]               */
/* Also collapses whitespace and inserts newlines at block boundaries  */
/* Returns length of output string.                                    */
/* ------------------------------------------------------------------ */
#define OUT_SZ (HTTP_BUF_SZ)
static char out_buf[OUT_SZ];

static int strip_html(const char *in)
{
    char *out    = out_buf;
    char *out_end = out_buf + OUT_SZ - 2;
    const char *p = in;
    int in_tag   = 0;
    int in_script = 0;   /* inside <script>...</script> */
    int in_style  = 0;   /* inside <style>...</style>  */
    int last_ws  = 1;    /* start as if we just saw whitespace */

    while (*p && out < out_end) {
        /* Skip script/style content */
        if (in_script) {
            if (b_starts_with_ci(p, "</script>")) { in_script = 0; p += 9; }
            else p++;
            continue;
        }
        if (in_style) {
            if (b_starts_with_ci(p, "</style>")) { in_style = 0; p += 8; }
            else p++;
            continue;
        }

        if (*p == '<') {
            in_tag = 1;
            p++;

            /* Check for special block/inline tags */
            const char *tag_start = p;

            /* <script> */
            if (b_starts_with_ci(p, "script")) {
                /* skip to > */
                while (*p && *p != '>') p++;
                if (*p == '>') p++;
                in_script = 1; in_tag = 0; continue;
            }
            /* <style> */
            if (b_starts_with_ci(p, "style")) {
                while (*p && *p != '>') p++;
                if (*p == '>') p++;
                in_style = 1; in_tag = 0; continue;
            }

            /* Tags that produce a newline */
            int add_nl = 0;
            if (b_starts_with_ci(p,"br") || b_starts_with_ci(p,"/br"))  add_nl=1;
            if (b_starts_with_ci(p,"p")  || b_starts_with_ci(p,"/p"))   add_nl=1;
            if (b_starts_with_ci(p,"div")|| b_starts_with_ci(p,"/div")) add_nl=1;
            if (b_starts_with_ci(p,"h1") || b_starts_with_ci(p,"h2")  ||
                b_starts_with_ci(p,"h3") || b_starts_with_ci(p,"h4")  ||
                b_starts_with_ci(p,"/h1")|| b_starts_with_ci(p,"/h2") ||
                b_starts_with_ci(p,"/h3")|| b_starts_with_ci(p,"/h4")) add_nl=1;
            if (b_starts_with_ci(p,"li") || b_starts_with_ci(p,"/li")) add_nl=1;
            if (b_starts_with_ci(p,"tr") || b_starts_with_ci(p,"/tr")) add_nl=1;

            /* Skip to end of tag */
            while (*p && *p != '>') p++;
            if (*p == '>') p++;
            in_tag = 0;

            if (add_nl && out < out_end && *(out-1) != '\n') {
                *out++ = '\n'; last_ws = 1;
            }
            (void)tag_start;
            continue;
        }

        if (in_tag) { if (*p == '>') in_tag = 0; p++; continue; }

        /* Regular character */
        char c = *p++;
        /* Collapse whitespace */
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        if (c == '\n') c = ' ';
        if (c == ' ') {
            if (!last_ws && out < out_end) { *out++ = ' '; last_ws = 1; }
            continue;
        }
        last_ws = 0;
        *out++ = c;
    }
    *out = '\0';
    return (int)(out - out_buf);
}

/* ------------------------------------------------------------------ */
/* Display with word-wrap and pagination                               */
/* ------------------------------------------------------------------ */
static void display_text(const char *text, int len)
{
    int col  = 0;
    int row  = 0;
    int i    = 0;

    (void)len;

    while (text[i]) {
        if (keyboard_ctrl_c_flag) { kputs("\n^C\n", COL_RED); return; }

        char c = text[i++];

        if (c == '\n') {
            kputchar('\n', COL_TEXT);
            col = 0;
            row++;
        } else {
            /* Word-wrap: look ahead to find word length */
            if (c == ' ' && col == 0) continue; /* skip leading space on new line */

            if (col >= COLS) {
                kputchar('\n', COL_TEXT);
                col = 0;
                row++;
                if (c == ' ') continue;
            }

            kputchar(c, COL_TEXT);
            col++;
        }

        /* Pagination */
        if (row >= ROWS) {
            kputs("\n[-- More --] (any key to continue, Q to quit)", COL_YELLOW);
            char ch = keyboard_getchar();
            kputs("\r                                               \r", COL_TEXT);
            if (ch == 'q' || ch == 'Q' || keyboard_ctrl_c_flag) return;
            row = 0;
        }
    }
    kputchar('\n', COL_TEXT);
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */
void browser_get(const char *url)
{
    if (!pcnet_ready) { kputs("  No NIC available.\n", COL_RED); return; }
    if (!my_ip_addr)  { kputs("  Network not configured.\n", COL_RED); return; }

    /* Strip http:// or https:// prefix */
    const char *p = url;
    if (b_starts_with_ci(p, "http://"))   p += 7;
    else if (b_starts_with_ci(p, "https://")) {
        kputs("  HTTPS not supported. Try http://\n", COL_RED); return;
    }

    /* Split host and path */
    char host[128];
    char path[256];
    int hi = 0;
    while (*p && *p != '/' && hi < 127) host[hi++] = *p++;
    host[hi] = '\0';
    if (*p == '/') {
        int pi = 0;
        while (*p && pi < 255) path[pi++] = *p++;
        path[pi] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }

    if (host[0] == '\0') { kputs("  Usage: browse <url>\n", COL_TEXT); return; }

    /* Resolve hostname */
    uint32_t ip = 0;
    ip = parse_ip(host);   /* try dotted-decimal first */
    if (!ip) {
        kputs("  Resolving ", COL_TEXT);
        kputs(host, COL_WHITE);
        kputs("...", COL_TEXT);
        if (!net_dns_resolve(host, &ip)) {
            kputs(" failed.\n", COL_RED);
            return;
        }
        /* Print resolved IP */
        kputs(" OK\n", COL_GREEN);
    }

    /* Connect and fetch */
    kputs("  TCP connecting to ", COL_TEXT);
    kputs(host, COL_WHITE);
    kputs(":80...", COL_TEXT_DIM);

    int received = do_http_get(ip, host, path);

    if (received == TCP_CANCEL) {
        kputs("  ^C  Cancelled.\n", COL_RED);
        keyboard_ctrl_c_clear();
        return;
    }
    if (received == -1) {
        kputs("\n  TCP: timeout (SYN-ACK not received).\n", COL_RED);
        return;
    }
    if (received == -2) {
        kputs("\n  TCP: NIC/ARP error.\n", COL_RED);
        return;
    }
    if (received == -3) {
        kputs("\n  TCP: connection reset by server.\n", COL_RED);
        return;
    }
    if (received < 0) {
        kputs("\n  TCP: failed.\n", COL_RED);
        return;
    }

    /* Find body (skip headers) */
    const char *body = skip_headers(http_buf, received);
    /* Decode HTML entities in-place (body is in http_buf, safe to modify) */
    decode_entities((char*)body);

    /* Strip HTML tags → out_buf */
    int out_len = strip_html(body);

    kputs("  ──────────────────────────────────────────────────────\n", COL_CYAN);
    display_text(out_buf, out_len);
    kputs("  ──────────────────────────────────────────────────────\n", COL_CYAN);
}
