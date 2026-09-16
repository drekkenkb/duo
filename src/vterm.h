/* vterm — a small VT100/xterm-subset terminal emulator.
 *
 * Bytes a shell writes go in through vt_feed(); the result is a grid of cells
 * the server composites onto the real terminal. Only what bash, less, vim,
 * htop and friends actually use is implemented.
 */
#ifndef DUO_VTERM_H
#define DUO_VTERM_H

#include <stddef.h>
#include <stdint.h>

/* Colours: top byte is the kind, low 24 bits the value. */
#define COLOR_DEFAULT   0u
#define COLOR_PAL(i)    (0x01000000u | (uint32_t)(i))
#define COLOR_RGB(r, g, b) \
    (0x02000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define COLOR_KIND(c)   ((c) >> 24)

#define ATTR_BOLD       0x01
#define ATTR_DIM        0x02
#define ATTR_ITALIC     0x04
#define ATTR_UNDERLINE  0x08
#define ATTR_BLINK      0x10
#define ATTR_REVERSE    0x20
#define ATTR_INVISIBLE  0x40
#define ATTR_STRIKE     0x80

/* Right half of a double-width character. */
#define CH_WIDE_CONT    0xFFFFFFFFu

typedef struct {
    uint32_t ch;        /* codepoint, 0 = blank */
    uint32_t fg, bg;
    uint8_t  attr;
} Cell;

typedef struct {
    int     len;        /* trailing blanks are trimmed */
    Cell    cells[];
} SbLine;

typedef struct {
    int     cx, cy;
    int     wrap_pending;
    int     origin;
    Cell    pen;
    uint8_t g[2];
    int     gl;
} SavedCursor;

enum { VT_GROUND, VT_ESC, VT_ESC_INTER, VT_CSI, VT_OSC, VT_STRING, VT_STRING_ESC };

#define VT_MAX_PARAMS 16
#define VT_SCROLLBACK 2000

typedef struct {
    int     rows, cols;
    Cell   *main, *alt;
    Cell   *screen;         /* main or alt */
    int     alt_active;

    int     cx, cy;
    int     wrap_pending;
    int     top, bot;       /* scroll region, inclusive */
    Cell    pen;
    uint8_t g[2];           /* G0/G1: 0 = ASCII, 1 = DEC line drawing */
    int     gl;
    uint8_t *tabs;

    int     autowrap, origin, insert;
    int     cursor_visible;
    int     app_cursor, app_keypad, bracketed_paste;

    SavedCursor saved_main, saved_alt;

    /* parser */
    int     state;
    int     params[VT_MAX_PARAMS];
    int     nparams;
    int     have_param;
    char    priv;           /* '?', '>', '=' or 0 */
    char    inter;          /* intermediate byte or 0 */
    uint32_t utf8_cp;
    int     utf8_left;
    uint32_t last_ch;

    /* scrollback ring (main screen only) */
    SbLine *sb[VT_SCROLLBACK];
    int     sb_head;        /* index of oldest line */
    int     sb_count;
    uint64_t sb_pushed;     /* lifetime count, lets a viewer stay pinned */

    /* replies (DSR, DA) the owner must write back to the pty */
    char    reply[256];
    size_t  reply_len;
} VTerm;

VTerm  *vt_new(int rows, int cols);
void    vt_free(VTerm *vt);
void    vt_feed(VTerm *vt, const char *buf, size_t len);
void    vt_resize(VTerm *vt, int rows, int cols);
void    vt_reset(VTerm *vt);

/* Line `i` of the history view: 0..sb_count-1 is scrollback (oldest first),
 * sb_count.. is the live screen. Fills `out` with `cols` cells. */
void    vt_history_line(const VTerm *vt, int i, Cell *out, int cols);

#endif
