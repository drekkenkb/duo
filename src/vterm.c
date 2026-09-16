#include "vterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static Cell blank_with(const Cell *pen)
{
    Cell c = { 0, COLOR_DEFAULT, pen->bg, 0 };
    return c;
}

static Cell *row_ptr(VTerm *vt, int y) { return vt->screen + y * vt->cols; }

static void fill_cells(Cell *c, int n, Cell v)
{
    for (int i = 0; i < n; i++)
        c[i] = v;
}

static void reply(VTerm *vt, const char *s)
{
    size_t n = strlen(s);
    if (vt->reply_len + n <= sizeof vt->reply) {
        memcpy(vt->reply + vt->reply_len, s, n);
        vt->reply_len += n;
    }
}

/* ---- scrollback ------------------------------------------------------- */

static void sb_push(VTerm *vt, const Cell *row)
{
    int len = vt->cols;
    while (len > 0 && row[len - 1].ch == 0 && row[len - 1].bg == COLOR_DEFAULT &&
           row[len - 1].attr == 0)
        len--;
    SbLine *l = malloc(sizeof *l + (size_t)len * sizeof(Cell));
    if (!l)
        return;
    l->len = len;
    memcpy(l->cells, row, (size_t)len * sizeof(Cell));

    if (vt->sb_count == VT_SCROLLBACK) {
        free(vt->sb[vt->sb_head]);
        vt->sb[vt->sb_head] = l;
        vt->sb_head = (vt->sb_head + 1) % VT_SCROLLBACK;
    } else {
        vt->sb[(vt->sb_head + vt->sb_count) % VT_SCROLLBACK] = l;
        vt->sb_count++;
    }
    vt->sb_pushed++;
}

static void sb_clear(VTerm *vt)
{
    for (int i = 0; i < vt->sb_count; i++)
        free(vt->sb[(vt->sb_head + i) % VT_SCROLLBACK]);
    vt->sb_head = vt->sb_count = 0;
}

void vt_history_line(const VTerm *vt, int i, Cell *out, int cols)
{
    Cell blank = { 0, COLOR_DEFAULT, COLOR_DEFAULT, 0 };
    fill_cells(out, cols, blank);
    if (i < 0)
        return;
    if (i < vt->sb_count) {
        const SbLine *l = vt->sb[(vt->sb_head + i) % VT_SCROLLBACK];
        memcpy(out, l->cells, (size_t)MIN(l->len, cols) * sizeof(Cell));
    } else if (i - vt->sb_count < vt->rows) {
        memcpy(out, vt->screen + (i - vt->sb_count) * vt->cols,
               (size_t)MIN(vt->cols, cols) * sizeof(Cell));
    }
}

/* ---- scrolling -------------------------------------------------------- */

static void scroll_up(VTerm *vt, int top, int bot, int n)
{
    int h = bot - top + 1;
    n = MIN(n, h);
    if (n <= 0)
        return;
    if (top == 0 && !vt->alt_active)
        for (int i = 0; i < n; i++)
            sb_push(vt, row_ptr(vt, i));
    memmove(row_ptr(vt, top), row_ptr(vt, top + n),
            (size_t)((h - n) * vt->cols) * sizeof(Cell));
    fill_cells(row_ptr(vt, bot - n + 1), n * vt->cols, blank_with(&vt->pen));
}

static void scroll_down(VTerm *vt, int top, int bot, int n)
{
    int h = bot - top + 1;
    n = MIN(n, h);
    if (n <= 0)
        return;
    memmove(row_ptr(vt, top + n), row_ptr(vt, top),
            (size_t)((h - n) * vt->cols) * sizeof(Cell));
    fill_cells(row_ptr(vt, top), n * vt->cols, blank_with(&vt->pen));
}

static void linefeed(VTerm *vt)
{
    if (vt->cy == vt->bot)
        scroll_up(vt, vt->top, vt->bot, 1);
    else if (vt->cy < vt->rows - 1)
        vt->cy++;
}

static void reverse_index(VTerm *vt)
{
    if (vt->cy == vt->top)
        scroll_down(vt, vt->top, vt->bot, 1);
    else if (vt->cy > 0)
        vt->cy--;
}

/* ---- cursor ----------------------------------------------------------- */

static void move_to(VTerm *vt, int x, int y)
{
    int lo = 0, hi = vt->rows - 1;
    if (vt->origin) {
        y += vt->top;
        lo = vt->top;
        hi = vt->bot;
    }
    vt->cx = MAX(0, MIN(x, vt->cols - 1));
    vt->cy = MAX(lo, MIN(y, hi));
    vt->wrap_pending = 0;
}

/* Relative vertical moves stop at the scroll margins when starting inside. */
static void move_rel_y(VTerm *vt, int dy)
{
    int y = vt->cy + dy;
    int lo = (vt->cy >= vt->top) ? vt->top : 0;
    int hi = (vt->cy <= vt->bot) ? vt->bot : vt->rows - 1;
    vt->cy = MAX(lo, MIN(y, hi));
    vt->wrap_pending = 0;
}

static void save_cursor(VTerm *vt)
{
    SavedCursor *s = vt->alt_active ? &vt->saved_alt : &vt->saved_main;
    s->cx = vt->cx;
    s->cy = vt->cy;
    s->wrap_pending = vt->wrap_pending;
    s->origin = vt->origin;
    s->pen = vt->pen;
    memcpy(s->g, vt->g, sizeof s->g);
    s->gl = vt->gl;
}

static void restore_cursor(VTerm *vt)
{
    SavedCursor *s = vt->alt_active ? &vt->saved_alt : &vt->saved_main;
    vt->cx = MIN(s->cx, vt->cols - 1);
    vt->cy = MIN(s->cy, vt->rows - 1);
    vt->wrap_pending = s->wrap_pending;
    vt->origin = s->origin;
    vt->pen = s->pen;
    memcpy(vt->g, s->g, sizeof vt->g);
    vt->gl = s->gl;
}

/* ---- erasing / editing ------------------------------------------------ */

/* Writing over half of a wide character must blank the other half. */
static void fix_wide(VTerm *vt, int x, int y)
{
    Cell *r = row_ptr(vt, y);
    if (x < 0 || x >= vt->cols)
        return;
    if (r[x].ch == CH_WIDE_CONT && x > 0)
        r[x - 1].ch = 0;
    if (x + 1 < vt->cols && r[x + 1].ch == CH_WIDE_CONT)
        r[x + 1].ch = 0;
}

static void erase_cells(VTerm *vt, int y, int x0, int x1)
{
    x0 = MAX(0, x0);
    x1 = MIN(vt->cols, x1);
    if (x0 >= x1)
        return;
    fix_wide(vt, x0, y);
    fix_wide(vt, x1 - 1, y);
    fill_cells(row_ptr(vt, y) + x0, x1 - x0, blank_with(&vt->pen));
}

static void erase_rows(VTerm *vt, int y0, int y1)
{
    for (int y = MAX(0, y0); y < MIN(vt->rows, y1); y++)
        erase_cells(vt, y, 0, vt->cols);
}

static void insert_cells(VTerm *vt, int n)
{
    Cell *r = row_ptr(vt, vt->cy);
    int x = vt->cx;
    n = MIN(n, vt->cols - x);
    fix_wide(vt, x, vt->cy);
    memmove(r + x + n, r + x, (size_t)(vt->cols - x - n) * sizeof(Cell));
    fill_cells(r + x, n, blank_with(&vt->pen));
    /* A wide lead pushed to the right edge has lost its right half; the
     * renderer draws such orphans as blanks. */
}

static void delete_cells(VTerm *vt, int n)
{
    Cell *r = row_ptr(vt, vt->cy);
    int x = vt->cx;
    n = MIN(n, vt->cols - x);
    fix_wide(vt, x, vt->cy);
    fix_wide(vt, x + n - 1, vt->cy);
    memmove(r + x, r + x + n, (size_t)(vt->cols - x - n) * sizeof(Cell));
    fill_cells(r + vt->cols - n, n, blank_with(&vt->pen));
    if (r[x].ch == CH_WIDE_CONT)
        r[x].ch = 0;
}

/* ---- printing --------------------------------------------------------- */

static uint32_t dec_graphics(uint32_t c)
{
    static const uint16_t map[] = {
        0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, /* `abcdefg */
        0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, /* hijklmno */
        0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C, /* pqrstuvw */
        0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,         /* xyz{|}~  */
    };
    if (c >= 0x60 && c <= 0x7e)
        return map[c - 0x60];
    if (c == 0x5f)
        return 0x00A0;
    return c;
}

static void print_char(VTerm *vt, uint32_t c)
{
    if (vt->g[vt->gl] == 1)
        c = dec_graphics(c);

    int w = (c < 0x7f) ? 1 : wcwidth((wchar_t)c);
    if (w < 0)
        w = 1;
    if (w == 0)
        return; /* combining marks are dropped */

    if (vt->wrap_pending && vt->autowrap) {
        vt->cx = 0;
        linefeed(vt);
    }
    vt->wrap_pending = 0;

    if (w == 2 && vt->cx == vt->cols - 1) {
        if (!vt->autowrap || vt->cols < 2)
            return;
        erase_cells(vt, vt->cy, vt->cx, vt->cols);
        vt->cx = 0;
        linefeed(vt);
    }

    if (vt->insert)
        insert_cells(vt, w);

    Cell *r = row_ptr(vt, vt->cy);
    fix_wide(vt, vt->cx, vt->cy);
    if (w == 2)
        fix_wide(vt, vt->cx + 1, vt->cy);

    Cell cell = vt->pen;
    cell.ch = c;
    r[vt->cx] = cell;
    if (w == 2) {
        cell.ch = CH_WIDE_CONT;
        r[vt->cx + 1] = cell;
    }
    vt->last_ch = c;

    if (vt->cx + w >= vt->cols) {
        vt->cx = vt->cols - 1;
        vt->wrap_pending = vt->autowrap;
    } else {
        vt->cx += w;
    }
}

/* ---- modes ------------------------------------------------------------ */

static void set_alt_screen(VTerm *vt, int on, int clear)
{
    if (on == vt->alt_active)
        return;
    vt->alt_active = on;
    vt->screen = on ? vt->alt : vt->main;
    if (on && clear)
        fill_cells(vt->alt, vt->rows * vt->cols, blank_with(&vt->pen));
}

static void set_dec_mode(VTerm *vt, int mode, int on)
{
    switch (mode) {
    case 1:    vt->app_cursor = on; break;
    case 6:    vt->origin = on; move_to(vt, 0, 0); break;
    case 7:    vt->autowrap = on; if (!on) vt->wrap_pending = 0; break;
    case 25:   vt->cursor_visible = on; break;
    case 47:
    case 1047: set_alt_screen(vt, on, on); break;
    case 1048: if (on) save_cursor(vt); else restore_cursor(vt); break;
    case 1049:
        if (on) {
            save_cursor(vt);
            set_alt_screen(vt, 1, 1);
        } else {
            set_alt_screen(vt, 0, 0);
            restore_cursor(vt);
        }
        break;
    case 2004: vt->bracketed_paste = on; break;
    default:   break;
    }
}

static void soft_reset(VTerm *vt)
{
    memset(&vt->pen, 0, sizeof vt->pen);
    vt->top = 0;
    vt->bot = vt->rows - 1;
    vt->autowrap = 1;
    vt->origin = vt->insert = 0;
    vt->cursor_visible = 1;
    vt->app_cursor = vt->app_keypad = 0;
    vt->g[0] = vt->g[1] = 0;
    vt->gl = 0;
    vt->wrap_pending = 0;
}

static void reset_tabs(VTerm *vt)
{
    for (int i = 0; i < vt->cols; i++)
        vt->tabs[i] = (i % 8 == 0);
}

/* ---- CSI -------------------------------------------------------------- */

/* Parameter i, with 0 or missing meaning `def`. */
static int P(const VTerm *vt, int i, int def)
{
    if (i >= vt->nparams || vt->params[i] == 0)
        return def;
    return vt->params[i];
}

static void sgr_color(VTerm *vt, int *i, uint32_t *dst)
{
    int k = *i;
    if (k + 1 >= vt->nparams)
        return;
    if (vt->params[k + 1] == 5 && k + 2 < vt->nparams) {
        *dst = COLOR_PAL(vt->params[k + 2] & 0xff);
        *i = k + 2;
    } else if (vt->params[k + 1] == 2 && k + 4 < vt->nparams) {
        *dst = COLOR_RGB(vt->params[k + 2] & 0xff, vt->params[k + 3] & 0xff,
                         vt->params[k + 4] & 0xff);
        *i = k + 4;
    } else {
        *i = vt->nparams;
    }
}

static void sgr(VTerm *vt)
{
    if (vt->nparams == 0) {
        memset(&vt->pen, 0, sizeof vt->pen);
        return;
    }
    for (int i = 0; i < vt->nparams; i++) {
        int p = vt->params[i];
        switch (p) {
        case 0:  memset(&vt->pen, 0, sizeof vt->pen); break;
        case 1:  vt->pen.attr |= ATTR_BOLD; break;
        case 2:  vt->pen.attr |= ATTR_DIM; break;
        case 3:  vt->pen.attr |= ATTR_ITALIC; break;
        case 4:
        case 21: vt->pen.attr |= ATTR_UNDERLINE; break;
        case 5:
        case 6:  vt->pen.attr |= ATTR_BLINK; break;
        case 7:  vt->pen.attr |= ATTR_REVERSE; break;
        case 8:  vt->pen.attr |= ATTR_INVISIBLE; break;
        case 9:  vt->pen.attr |= ATTR_STRIKE; break;
        case 22: vt->pen.attr &= ~(ATTR_BOLD | ATTR_DIM); break;
        case 23: vt->pen.attr &= ~ATTR_ITALIC; break;
        case 24: vt->pen.attr &= ~ATTR_UNDERLINE; break;
        case 25: vt->pen.attr &= ~ATTR_BLINK; break;
        case 27: vt->pen.attr &= ~ATTR_REVERSE; break;
        case 28: vt->pen.attr &= ~ATTR_INVISIBLE; break;
        case 29: vt->pen.attr &= ~ATTR_STRIKE; break;
        case 38: sgr_color(vt, &i, &vt->pen.fg); break;
        case 39: vt->pen.fg = COLOR_DEFAULT; break;
        case 48: sgr_color(vt, &i, &vt->pen.bg); break;
        case 49: vt->pen.bg = COLOR_DEFAULT; break;
        default:
            if (p >= 30 && p <= 37)
                vt->pen.fg = COLOR_PAL(p - 30);
            else if (p >= 40 && p <= 47)
                vt->pen.bg = COLOR_PAL(p - 40);
            else if (p >= 90 && p <= 97)
                vt->pen.fg = COLOR_PAL(p - 90 + 8);
            else if (p >= 100 && p <= 107)
                vt->pen.bg = COLOR_PAL(p - 100 + 8);
            break;
        }
    }
}

static void csi_dispatch(VTerm *vt, char f)
{
    char buf[64];

    if (vt->priv == '?') {
        switch (f) {
        case 'h':
        case 'l':
            for (int i = 0; i < vt->nparams; i++)
                set_dec_mode(vt, vt->params[i], f == 'h');
            break;
        case 'J': goto ed;
        case 'K': goto el;
        default: break;
        }
        return;
    }
    if (vt->priv == '>') {
        if (f == 'c')
            reply(vt, "\033[>0;0;0c");
        return;
    }
    if (vt->priv)
        return;

    if (vt->inter) {
        if (vt->inter == '!' && f == 'p')
            soft_reset(vt);
        return;
    }

    switch (f) {
    case '@': insert_cells(vt, P(vt, 0, 1)); break;
    case 'A': move_rel_y(vt, -P(vt, 0, 1)); break;
    case 'B':
    case 'e': move_rel_y(vt, P(vt, 0, 1)); break;
    case 'C':
    case 'a': vt->cx = MIN(vt->cx + P(vt, 0, 1), vt->cols - 1); vt->wrap_pending = 0; break;
    case 'D': vt->cx = MAX(vt->cx - P(vt, 0, 1), 0); vt->wrap_pending = 0; break;
    case 'E': move_rel_y(vt, P(vt, 0, 1)); vt->cx = 0; break;
    case 'F': move_rel_y(vt, -P(vt, 0, 1)); vt->cx = 0; break;
    case 'G':
    case '`': vt->cx = MIN(P(vt, 0, 1) - 1, vt->cols - 1); vt->wrap_pending = 0; break;
    case 'H':
    case 'f': move_to(vt, P(vt, 1, 1) - 1, P(vt, 0, 1) - 1); break;
    case 'd': move_to(vt, vt->cx, P(vt, 0, 1) - 1); break;
    case 'I':
        for (int n = P(vt, 0, 1); n > 0 && vt->cx < vt->cols - 1; n--)
            do vt->cx++; while (vt->cx < vt->cols - 1 && !vt->tabs[vt->cx]);
        break;
    case 'Z':
        for (int n = P(vt, 0, 1); n > 0 && vt->cx > 0; n--)
            do vt->cx--; while (vt->cx > 0 && !vt->tabs[vt->cx]);
        break;
    case 'J':
    ed:
        switch (vt->nparams ? vt->params[0] : 0) {
        case 0:
            erase_cells(vt, vt->cy, vt->cx, vt->cols);
            erase_rows(vt, vt->cy + 1, vt->rows);
            break;
        case 1:
            erase_rows(vt, 0, vt->cy);
            erase_cells(vt, vt->cy, 0, vt->cx + 1);
            break;
        case 2: erase_rows(vt, 0, vt->rows); break;
        case 3: sb_clear(vt); break;
        }
        vt->wrap_pending = 0;
        break;
    case 'K':
    el:
        switch (vt->nparams ? vt->params[0] : 0) {
        case 0: erase_cells(vt, vt->cy, vt->cx, vt->cols); break;
        case 1: erase_cells(vt, vt->cy, 0, vt->cx + 1); break;
        case 2: erase_cells(vt, vt->cy, 0, vt->cols); break;
        }
        vt->wrap_pending = 0;
        break;
    case 'L':
        if (vt->cy >= vt->top && vt->cy <= vt->bot) {
            scroll_down(vt, vt->cy, vt->bot, P(vt, 0, 1));
            vt->cx = 0;
        }
        break;
    case 'M':
        if (vt->cy >= vt->top && vt->cy <= vt->bot) {
            /* deleting lines never feeds scrollback */
            int save_alt = vt->alt_active;
            vt->alt_active = 1;
            scroll_up(vt, vt->cy, vt->bot, P(vt, 0, 1));
            vt->alt_active = save_alt;
            vt->cx = 0;
        }
        break;
    case 'P': delete_cells(vt, P(vt, 0, 1)); break;
    case 'X': erase_cells(vt, vt->cy, vt->cx, vt->cx + P(vt, 0, 1)); break;
    case 'S': scroll_up(vt, vt->top, vt->bot, P(vt, 0, 1)); break;
    case 'T': if (vt->nparams <= 1) scroll_down(vt, vt->top, vt->bot, P(vt, 0, 1)); break;
    case 'b':
        if (vt->last_ch)
            for (int n = MIN(P(vt, 0, 1), vt->rows * vt->cols); n > 0; n--)
                print_char(vt, vt->last_ch);
        break;
    case 'c': reply(vt, "\033[?62;22c"); break;
    case 'g':
        if (P(vt, 0, 0) == 0)
            vt->tabs[vt->cx] = 0;
        else if (P(vt, 0, 0) == 3)
            memset(vt->tabs, 0, (size_t)vt->cols);
        break;
    case 'h':
    case 'l':
        for (int i = 0; i < vt->nparams; i++)
            if (vt->params[i] == 4)
                vt->insert = (f == 'h');
        break;
    case 'm': sgr(vt); break;
    case 'n':
        if (P(vt, 0, 0) == 5) {
            reply(vt, "\033[0n");
        } else if (P(vt, 0, 0) == 6) {
            int y = vt->cy - (vt->origin ? vt->top : 0);
            snprintf(buf, sizeof buf, "\033[%d;%dR", y + 1, vt->cx + 1);
            reply(vt, buf);
        }
        break;
    case 'r': {
        int t = P(vt, 0, 1) - 1;
        int b = P(vt, 1, vt->rows) - 1;
        b = MIN(b, vt->rows - 1);
        if (t < b) {
            vt->top = t;
            vt->bot = b;
            move_to(vt, 0, 0);
        }
        break;
    }
    case 's': save_cursor(vt); break;
    case 'u': restore_cursor(vt); break;
    default: break;
    }
}

/* ---- ESC -------------------------------------------------------------- */

static void esc_dispatch(VTerm *vt, char f)
{
    switch (f) {
    case '7': save_cursor(vt); break;
    case '8': restore_cursor(vt); break;
    case 'D': linefeed(vt); vt->wrap_pending = 0; break;
    case 'E': vt->cx = 0; linefeed(vt); vt->wrap_pending = 0; break;
    case 'M': reverse_index(vt); vt->wrap_pending = 0; break;
    case 'H': vt->tabs[vt->cx] = 1; break;
    case 'c': vt_reset(vt); break;
    case '=': vt->app_keypad = 1; break;
    case '>': vt->app_keypad = 0; break;
    default: break;
    }
}

static void esc_inter_dispatch(VTerm *vt, char f)
{
    switch (vt->inter) {
    case '(': vt->g[0] = (f == '0'); break;
    case ')': vt->g[1] = (f == '0'); break;
    case '#':
        if (f == '8') { /* DECALN: fill with E */
            Cell e = { 'E', COLOR_DEFAULT, COLOR_DEFAULT, 0 };
            fill_cells(vt->screen, vt->rows * vt->cols, e);
        }
        break;
    default: break;
    }
}

/* ---- control characters ----------------------------------------------- */

static void control(VTerm *vt, unsigned char b)
{
    switch (b) {
    case 0x08:
        if (vt->cx > 0)
            vt->cx--;
        vt->wrap_pending = 0;
        break;
    case 0x09:
        while (vt->cx < vt->cols - 1) {
            vt->cx++;
            if (vt->tabs[vt->cx])
                break;
        }
        break;
    case 0x0a:
    case 0x0b:
    case 0x0c:
        linefeed(vt);
        vt->wrap_pending = 0;
        break;
    case 0x0d:
        vt->cx = 0;
        vt->wrap_pending = 0;
        break;
    case 0x0e: vt->gl = 1; break;
    case 0x0f: vt->gl = 0; break;
    default: break;
    }
}

/* ---- the byte-level state machine ------------------------------------- */

static void begin_csi(VTerm *vt)
{
    vt->state = VT_CSI;
    vt->nparams = 0;
    vt->have_param = 0;
    vt->params[0] = 0;
    vt->priv = 0;
    vt->inter = 0;
}

static void feed_byte(VTerm *vt, unsigned char b)
{
    /* These interrupt any sequence. */
    if (vt->state != VT_OSC && vt->state != VT_STRING && vt->state != VT_STRING_ESC) {
        if (b == 0x18 || b == 0x1a) {
            vt->state = VT_GROUND;
            return;
        }
        if (b == 0x1b) {
            vt->state = VT_ESC;
            vt->inter = 0;
            return;
        }
    }

    switch (vt->state) {
    case VT_GROUND:
        if (vt->utf8_left) {
            if ((b & 0xC0) == 0x80) {
                vt->utf8_cp = (vt->utf8_cp << 6) | (b & 0x3F);
                if (--vt->utf8_left == 0)
                    print_char(vt, vt->utf8_cp);
                return;
            }
            vt->utf8_left = 0;
            print_char(vt, 0xFFFD);
        }
        if (b < 0x20 || b == 0x7f) {
            control(vt, b);
        } else if (b < 0x80) {
            print_char(vt, b);
        } else if ((b & 0xE0) == 0xC0) {
            vt->utf8_cp = b & 0x1F;
            vt->utf8_left = 1;
        } else if ((b & 0xF0) == 0xE0) {
            vt->utf8_cp = b & 0x0F;
            vt->utf8_left = 2;
        } else if ((b & 0xF8) == 0xF0) {
            vt->utf8_cp = b & 0x07;
            vt->utf8_left = 3;
        } else {
            print_char(vt, 0xFFFD);
        }
        break;

    case VT_ESC:
        if (b < 0x20) {
            control(vt, b);
        } else if (b == '[') {
            begin_csi(vt);
        } else if (b == ']') {
            vt->state = VT_OSC;
        } else if (b == 'P' || b == '_' || b == '^' || b == 'X') {
            vt->state = VT_STRING;
        } else if (b >= 0x20 && b <= 0x2f) {
            vt->inter = (char)b;
            vt->state = VT_ESC_INTER;
        } else {
            esc_dispatch(vt, (char)b);
            if (vt->state == VT_ESC)
                vt->state = VT_GROUND;
        }
        break;

    case VT_ESC_INTER:
        if (b < 0x20) {
            control(vt, b);
        } else if (b <= 0x2f) {
            /* further intermediates: ignore */
        } else {
            esc_inter_dispatch(vt, (char)b);
            vt->state = VT_GROUND;
        }
        break;

    case VT_CSI:
        if (b < 0x20) {
            control(vt, b);
        } else if (b >= '0' && b <= '9') {
            if (vt->nparams == 0)
                vt->nparams = 1;
            int *p = &vt->params[vt->nparams - 1];
            if (*p < 100000)
                *p = *p * 10 + (b - '0');
        } else if (b == ';' || b == ':') {
            if (vt->nparams == 0)
                vt->nparams = 1;
            if (vt->nparams < VT_MAX_PARAMS)
                vt->params[vt->nparams++] = 0;
        } else if (b >= '<' && b <= '?') {
            vt->priv = (char)b;
        } else if (b >= 0x20 && b <= 0x2f) {
            vt->inter = (char)b;
        } else if (b >= 0x40 && b <= 0x7e) {
            vt->state = VT_GROUND;
            csi_dispatch(vt, (char)b);
        }
        break;

    case VT_OSC:
    case VT_STRING:
        /* Titles, clipboard and DCS payloads are swallowed. */
        if (b == 0x07 && vt->state == VT_OSC)
            vt->state = VT_GROUND;
        else if (b == 0x1b)
            vt->state = VT_STRING_ESC;
        else if (b == 0x18 || b == 0x1a)
            vt->state = VT_GROUND;
        break;

    case VT_STRING_ESC:
        if (b == '\\') {
            vt->state = VT_GROUND;
        } else if (b == '[') {
            begin_csi(vt);
        } else {
            vt->state = VT_ESC;
            feed_byte(vt, b);
        }
        break;
    }
}

void vt_feed(VTerm *vt, const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        feed_byte(vt, (unsigned char)buf[i]);
}

/* ---- lifecycle -------------------------------------------------------- */

void vt_reset(VTerm *vt)
{
    set_alt_screen(vt, 0, 0);
    soft_reset(vt);
    vt->bracketed_paste = 0;
    Cell blank = { 0, COLOR_DEFAULT, COLOR_DEFAULT, 0 };
    fill_cells(vt->main, vt->rows * vt->cols, blank);
    fill_cells(vt->alt, vt->rows * vt->cols, blank);
    vt->cx = vt->cy = 0;
    reset_tabs(vt);
    save_cursor(vt);
    vt->saved_alt = vt->saved_main;
    vt->state = VT_GROUND;
    vt->utf8_left = 0;
    vt->last_ch = 0;
}

VTerm *vt_new(int rows, int cols)
{
    VTerm *vt = calloc(1, sizeof *vt);
    if (!vt)
        return NULL;
    vt->rows = MAX(rows, 1);
    vt->cols = MAX(cols, 1);
    vt->main = calloc((size_t)(vt->rows * vt->cols), sizeof(Cell));
    vt->alt = calloc((size_t)(vt->rows * vt->cols), sizeof(Cell));
    vt->tabs = calloc((size_t)vt->cols, 1);
    if (!vt->main || !vt->alt || !vt->tabs) {
        vt_free(vt);
        return NULL;
    }
    vt->screen = vt->main;
    vt_reset(vt);
    return vt;
}

void vt_free(VTerm *vt)
{
    if (!vt)
        return;
    sb_clear(vt);
    free(vt->main);
    free(vt->alt);
    free(vt->tabs);
    free(vt);
}

static Cell *resize_grid(const Cell *old, int orows, int ocols, int skip,
                         int rows, int cols)
{
    Cell *g = calloc((size_t)(rows * cols), sizeof(Cell));
    if (!g)
        return NULL;
    for (int y = 0; y < rows && y + skip < orows; y++) {
        memcpy(g + y * cols, old + (y + skip) * ocols,
               (size_t)MIN(cols, ocols) * sizeof(Cell));
        if (cols < ocols && g[y * cols + cols - 1].ch != CH_WIDE_CONT &&
            ocols > cols && old[(y + skip) * ocols + cols].ch == CH_WIDE_CONT)
            g[y * cols + cols - 1].ch = 0; /* wide char cut in half */
    }
    return g;
}

void vt_resize(VTerm *vt, int rows, int cols)
{
    rows = MAX(rows, 1);
    cols = MAX(cols, 1);
    if (rows == vt->rows && cols == vt->cols)
        return;

    /* When shrinking, keep the cursor line on screen by pushing the lines
     * above it into scrollback, the way a real terminal would. */
    int skip = 0;
    if (vt->cy >= rows)
        skip = vt->cy - rows + 1;
    if (!vt->alt_active)
        for (int i = 0; i < skip; i++)
            sb_push(vt, vt->main + i * vt->cols);

    int alt_skip = skip;
    Cell *m = resize_grid(vt->main, vt->rows, vt->cols, vt->alt_active ? 0 : skip, rows, cols);
    Cell *a = resize_grid(vt->alt, vt->rows, vt->cols, vt->alt_active ? alt_skip : 0, rows, cols);
    uint8_t *t = calloc((size_t)cols, 1);
    if (!m || !a || !t) {
        free(m);
        free(a);
        free(t);
        return;
    }
    free(vt->main);
    free(vt->alt);
    free(vt->tabs);
    vt->main = m;
    vt->alt = a;
    vt->tabs = t;
    vt->screen = vt->alt_active ? a : m;
    vt->rows = rows;
    vt->cols = cols;
    reset_tabs(vt);

    vt->cy = MAX(0, vt->cy - skip);
    vt->cx = MIN(vt->cx, cols - 1);
    vt->wrap_pending = 0;
    vt->top = 0;
    vt->bot = rows - 1;

    SavedCursor *s = vt->alt_active ? &vt->saved_main : &vt->saved_alt;
    s->cx = MIN(s->cx, cols - 1);
    s->cy = MIN(s->cy, rows - 1);
    s = vt->alt_active ? &vt->saved_alt : &vt->saved_main;
    s->cx = MIN(s->cx, cols - 1);
    s->cy = MAX(0, MIN(s->cy - skip, rows - 1));
}
