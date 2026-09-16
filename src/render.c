/* Composites the panes, status bar and overlays into one frame, then sends
 * the client only the cells that changed since the last frame. */
#include "config.h"
#include "proto.h"
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static Cell    *frame, *prev;
static uint8_t *cover;          /* cells a pane drew, so the rest is divider */
static int      frame_n;
static int      sent_app_cursor = -1, sent_app_keypad = -1, sent_paste = -1;

static const uint32_t BAR_FG = COLOR_PAL(250), BAR_BG = COLOR_PAL(236);
static const uint32_t SEL_FG = COLOR_PAL(231), SEL_BG = COLOR_PAL(25);
static const uint32_t ALT_BG = COLOR_PAL(238);
static const uint32_t WARN_FG = COLOR_PAL(231), WARN_BG = COLOR_PAL(124);
static const uint32_t EDGE_ON = COLOR_PAL(71), EDGE_OFF = COLOR_PAL(240);

void render_reset(void)
{
    sent_app_cursor = sent_app_keypad = sent_paste = -1;
    S.full_redraw = 1;
}

static void ensure_frame(void)
{
    int n = S.rows * S.cols;
    if (n == frame_n)
        return;
    free(frame);
    free(prev);
    free(cover);
    frame = calloc((size_t)n, sizeof(Cell));
    prev = calloc((size_t)n, sizeof(Cell));
    cover = calloc((size_t)n, 1);
    if (!frame || !prev || !cover)
        abort();
    frame_n = n;
    S.full_redraw = 1;
}

static void put_cell(int x, int y, uint32_t ch, uint32_t fg, uint32_t bg, uint8_t attr)
{
    if (x < 0 || y < 0 || x >= S.cols || y >= S.rows)
        return;
    Cell c = { ch, fg, bg, attr };
    frame[y * S.cols + x] = c;
}

static int u8len(const char *s)
{
    int n = 0;
    for (; *s; s++)
        n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

static int put_str(int x, int y, const char *s, uint32_t fg, uint32_t bg, uint8_t attr, int xmax)
{
    const unsigned char *u = (const unsigned char *)s;
    while (*u && x < xmax) {
        uint32_t cp = *u++;
        int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        cp &= more == 3 ? 0x07 : more == 2 ? 0x0F : more == 1 ? 0x1F : 0x7F;
        while (more-- && (*u & 0xC0) == 0x80)
            cp = (cp << 6) | (*u++ & 0x3F);
        put_cell(x++, y, cp, fg, bg, attr);
    }
    return x;
}

static void fill_row(int y, uint32_t fg, uint32_t bg)
{
    for (int x = 0; x < S.cols; x++)
        put_cell(x, y, 0, fg, bg, 0);
}

/* ---- panes ------------------------------------------------------------ */

static void draw_pane(int p, Rect r)
{
    int w = S.pane[p].win;
    if (w < 0 || w >= S.nwins)
        return;
    VTerm *vt = S.win[w]->vt;
    int cols = MIN(r.w, vt->cols);
    S.win[w]->seen = 1;

    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++)
            cover[(r.y + y) * S.cols + r.x + x] = 1;

    if (S.cm.active && p == S.focus) {
        Cell *line = malloc((size_t)MAX(r.w, 1) * sizeof(Cell));
        if (!line)
            return;
        int l0 = S.cm.sel ? S.cm.sel_line : S.cm.line;
        int c0 = S.cm.sel ? S.cm.sel_col : S.cm.col;
        int l1 = S.cm.line, c1 = S.cm.col;
        if (l0 > l1 || (l0 == l1 && c0 > c1)) {
            int t = l0; l0 = l1; l1 = t;
            t = c0; c0 = c1; c1 = t;
        }
        for (int y = 0; y < r.h; y++) {
            int line_no = S.cm.view_top + y;
            vt_history_line(vt, line_no, line, r.w);
            for (int x = 0; x < r.w; x++) {
                Cell c = line[x];
                if (S.cm.sel && line_no >= l0 && line_no <= l1) {
                    int from = (line_no == l0) ? c0 : 0;
                    int to = (line_no == l1) ? c1 : r.w - 1;
                    if (x >= from && x <= to)
                        c.attr |= ATTR_REVERSE;
                }
                if (S.cm.hl_len && line_no == S.cm.hl_line &&
                    x >= S.cm.hl_col && x < S.cm.hl_col + S.cm.hl_len) {
                    c.fg = COLOR_PAL(16);
                    c.bg = COLOR_PAL(179);
                }
                frame[(r.y + y) * S.cols + r.x + x] = c;
            }
        }
        free(line);
        return;
    }

    for (int y = 0; y < MIN(r.h, vt->rows); y++)
        memcpy(frame + (r.y + y) * S.cols + r.x, vt->screen + y * vt->cols,
               (size_t)cols * sizeof(Cell));
}

/* Anything between panes is a divider; its shape comes from which of its
 * neighbours a pane drew on. */
static void draw_dividers(Rect focus_rect)
{
    for (int y = 0; y < S.rows - 1; y++) {
        for (int x = 0; x < S.cols; x++) {
            if (cover[y * S.cols + x])
                continue;
            int left = x > 0 && cover[y * S.cols + x - 1];
            int right = x + 1 < S.cols && cover[y * S.cols + x + 1];
            int up = y > 0 && cover[(y - 1) * S.cols + x];
            int down = y + 1 < S.rows - 1 && cover[(y + 1) * S.cols + x];
            uint32_t ch = 0x253C;
            if ((left || right) && !(up || down))
                ch = 0x2502;
            else if ((up || down) && !(left || right))
                ch = 0x2500;

            int touches_focus =
                (x == focus_rect.x - 1 || x == focus_rect.x + focus_rect.w) &&
                    y >= focus_rect.y && y < focus_rect.y + focus_rect.h;
            touches_focus = touches_focus ||
                ((y == focus_rect.y - 1 || y == focus_rect.y + focus_rect.h) &&
                 x >= focus_rect.x && x < focus_rect.x + focus_rect.w);
            put_cell(x, y, ch, touches_focus ? EDGE_ON : EDGE_OFF, COLOR_DEFAULT, 0);
        }
    }
}

/* ---- status bar ------------------------------------------------------- */

static void draw_prompt_bar(int y)
{
    const char *label = "";
    uint32_t bg = SEL_BG, fg = SEL_FG;
    char text[256];

    switch (S.prompt) {
    case PROMPT_RENAME: label = "Rename window: "; break;
    case PROMPT_SEARCH:
        label = S.cm.dir > 0 ? "Search down: " : "Search up: ";
        break;
    case PROMPT_KILL: {
        Window *w = focused_window();
        snprintf(text, sizeof text, " Close window %d:%s? (y/n) ",
                 S.pane[S.focus].win + 1, w ? win_label(w) : "?");
        fill_row(y, WARN_FG, WARN_BG);
        put_str(0, y, text, WARN_FG, WARN_BG, ATTR_BOLD, S.cols);
        return;
    }
    case PROMPT_QUIT:
        snprintf(text, sizeof text, " End the session, closing %d window%s? (y/n) ",
                 S.nwins, S.nwins == 1 ? "" : "s");
        fill_row(y, WARN_FG, WARN_BG);
        put_str(0, y, text, WARN_FG, WARN_BG, ATTR_BOLD, S.cols);
        return;
    default: break;
    }

    fill_row(y, fg, bg);
    snprintf(text, sizeof text, " %s%s", label, S.prompt_buf);
    int x = put_str(0, y, text, fg, bg, ATTR_BOLD, S.cols);
    put_cell(x, y, '_', fg, bg, ATTR_BOLD | ATTR_BLINK);
}

static void draw_status(void)
{
    int y = S.rows - 1, W = S.cols;
    if (S.prompt != PROMPT_NONE) {
        draw_prompt_bar(y);
        return;
    }
    fill_row(y, BAR_FG, BAR_BG);

    char right[256];
    uint32_t rfg = BAR_FG, rbg = BAR_BG;
    uint8_t rattr = 0;
    if (S.cm.active) {
        VTerm *vt = focused_window() ? focused_window()->vt : NULL;
        int back = vt ? (vt->sb_count - S.cm.view_top) : 0;
        snprintf(right, sizeof right, " COPY -%d%s%.24s  Space sel  Enter copy  / search  q ",
                 MAX(back, 0), S.cm.query[0] ? "  /" : "", S.cm.query);
        rfg = COLOR_PAL(16);
        rbg = COLOR_PAL(179);
        rattr = ATTR_BOLD;
    } else if (S.message[0]) {
        snprintf(right, sizeof right, " %s ", S.message);
        rfg = COLOR_PAL(231);
        rattr = ATTR_BOLD;
    } else {
        snprintf(right, sizeof right, " %s  %s ? help ", layout_name(S.layout), PREFIX_NAME);
    }
    int rlen = u8len(right);
    int avail = MAX(W - rlen, 0);

    int x = put_str(0, y, " duo ", COLOR_PAL(16), COLOR_PAL(71), ATTR_BOLD, W) + 1;

    /* Scroll the window list so the focused window stays visible. */
    char label[MAX_WINDOWS][NAME_LEN + 12];
    int width[MAX_WINDOWS];
    int cur = S.pane[S.focus].win;
    for (int i = 0; i < S.nwins; i++) {
        snprintf(label[i], sizeof label[i], " %d:%s%s ", i + 1, win_label(S.win[i]),
                 S.win[i]->activity ? "*" : "");
        width[i] = u8len(label[i]) + 1;
    }
    int start = 0, used;
    for (;;) {
        used = 0;
        for (int i = start; i <= cur && i < S.nwins; i++)
            used += width[i];
        if (x + used <= avail || start >= cur)
            break;
        start++;
    }
    if (start > 0)
        x = put_str(x, y, "<", COLOR_PAL(244), BAR_BG, 0, W) + 1;

    for (int i = start; i < S.nwins; i++) {
        if (x + width[i] > avail) {
            put_str(x, y, ">", COLOR_PAL(244), BAR_BG, 0, W);
            break;
        }
        uint32_t fg = BAR_FG, bg = BAR_BG;
        uint8_t attr = 0;
        if (i == cur) {
            fg = SEL_FG;
            bg = SEL_BG;
            attr = ATTR_BOLD;
        } else if (win_visible_pane(i) >= 0) {
            bg = ALT_BG;
            fg = COLOR_PAL(252);
        }
        if (S.win[i]->activity)
            fg = COLOR_PAL(214);
        x = put_str(x, y, label[i], fg, bg, attr, avail) + 1;
    }

    put_str(MAX(W - rlen, 0), y, right, rfg, rbg, rattr, W);
}

/* ---- overlays --------------------------------------------------------- */

static void draw_box(int x0, int y0, int w, int h, uint32_t fg, uint32_t bg, uint32_t edge)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t ch = 0;
            if (y == 0 || y == h - 1)
                ch = 0x2500;
            if (x == 0 || x == w - 1)
                ch = 0x2502;
            if (y == 0 && x == 0) ch = 0x256D;
            if (y == 0 && x == w - 1) ch = 0x256E;
            if (y == h - 1 && x == 0) ch = 0x2570;
            if (y == h - 1 && x == w - 1) ch = 0x256F;
            put_cell(x0 + x, y0 + y, ch, ch ? edge : fg, bg, 0);
        }
}

static const char *help_lines[] = {
    "duo: press " PREFIX_NAME ", then",
    "",
    "c  new window          1-9  go to window",
    "n  p  next/previous    \"    pick from a list",
    "A  rename window       k    close window",
    PREFIX_NAME "  last window",
    "",
    "S  split: add a pane   X    close this pane",
    "Tab  next pane         arrows  focus by direction",
    "t  one window          v    columns",
    "s  rows                g    grid",
    "Space  cycle layouts",
    "",
    "[  copy mode: move, / search, Space select,",
    "   Enter copy, q leave      ]  paste",
    "",
    "d  detach   r  redraw   a  literal " PREFIX_NAME,
    "Q  end session          ?  this help",
    "",
    "any key closes this",
};

static void draw_help(void)
{
    int n = (int)(sizeof help_lines / sizeof *help_lines);
    int w = 0;
    for (int i = 0; i < n; i++)
        w = MAX(w, u8len(help_lines[i]));
    w += 4;
    int h = n + 2;
    int x0 = MAX((S.cols - w) / 2, 0), y0 = MAX((S.rows - 1 - h) / 2, 0);
    uint32_t fg = COLOR_PAL(252), bg = COLOR_PAL(237);
    draw_box(x0, y0, MIN(w, S.cols), MIN(h, S.rows - 1), fg, bg, EDGE_ON);
    for (int i = 0; i < n; i++)
        put_str(x0 + 2, y0 + 1 + i, help_lines[i], fg, bg, i == 0 ? ATTR_BOLD : 0,
                MIN(x0 + w - 2, S.cols));
}

static void draw_picker(void)
{
    int w = 34, h = S.nwins + 4;
    int x0 = MAX((S.cols - w) / 2, 0), y0 = MAX((S.rows - 1 - h) / 2, 0);
    uint32_t fg = COLOR_PAL(252), bg = COLOR_PAL(237);
    draw_box(x0, y0, MIN(w, S.cols), MIN(h, S.rows - 1), fg, bg, EDGE_ON);
    put_str(x0 + 2, y0 + 1, "Windows", fg, bg, ATTR_BOLD, MIN(x0 + w - 2, S.cols));
    for (int i = 0; i < S.nwins; i++) {
        char line[64];
        snprintf(line, sizeof line, " %d:%-*s%s", i + 1, NAME_LEN, win_label(S.win[i]),
                 win_visible_pane(i) >= 0 ? "on screen" : "");
        int sel = (i == S.picker_sel);
        for (int x = 1; x < w - 1 && sel; x++)
            put_cell(x0 + x, y0 + 2 + i, 0, SEL_FG, SEL_BG, 0);
        put_str(x0 + 2, y0 + 2 + i, line, sel ? SEL_FG : fg, sel ? SEL_BG : bg,
                sel ? ATTR_BOLD : 0, MIN(x0 + w - 2, S.cols));
    }
    put_str(x0 + 2, y0 + h - 2, "Enter select   Esc cancel", COLOR_PAL(244), bg, 0,
            MIN(x0 + w - 2, S.cols));
}

/* ---- frame ------------------------------------------------------------ */

static Rect compose(void)
{
    Cell blank = { 0, COLOR_DEFAULT, COLOR_DEFAULT, 0 };
    for (int i = 0; i < frame_n; i++) {
        frame[i] = blank;
        cover[i] = 0;
    }

    Rect r[MAX_PANES];
    int lay = layout_rects(S.layout, S.npanes, S.rows, S.cols, r);
    if (lay == LAYOUT_TABS) {
        draw_pane(S.focus, r[S.focus]);
    } else {
        for (int p = 0; p < S.npanes; p++)
            draw_pane(p, r[p]);
        draw_dividers(r[S.focus]);
    }

    draw_status();
    if (S.picker)
        draw_picker();
    if (S.help)
        draw_help();
    return r[S.focus];
}

static int cell_eq(const Cell *a, const Cell *b)
{
    return a->ch == b->ch && a->fg == b->fg && a->bg == b->bg && a->attr == b->attr;
}

static void emit_color(Buf *o, uint32_t c, int base)
{
    uint32_t v = c & 0xFFFFFF;
    if (COLOR_KIND(c) == 1) {
        if (v < 8)
            buf_appendf(o, ";%u", base + v);
        else if (v < 16)
            buf_appendf(o, ";%u", base + 60 + v - 8);
        else
            buf_appendf(o, ";%d;5;%u", base + 8, v);
    } else if (COLOR_KIND(c) == 2) {
        buf_appendf(o, ";%d;2;%u;%u;%u", base + 8, v >> 16, (v >> 8) & 0xff, v & 0xff);
    }
}

static void emit_sgr(Buf *o, const Cell *c)
{
    static const struct { uint8_t bit; const char *code; } attrs[] = {
        { ATTR_BOLD, ";1" }, { ATTR_DIM, ";2" }, { ATTR_ITALIC, ";3" },
        { ATTR_UNDERLINE, ";4" }, { ATTR_BLINK, ";5" }, { ATTR_REVERSE, ";7" },
        { ATTR_INVISIBLE, ";8" }, { ATTR_STRIKE, ";9" },
    };
    buf_append(o, "\033[0", 3);
    for (size_t i = 0; i < sizeof attrs / sizeof *attrs; i++)
        if (c->attr & attrs[i].bit)
            buf_append(o, attrs[i].code, 2);
    emit_color(o, c->fg, 30);
    emit_color(o, c->bg, 40);
    buf_append(o, "m", 1);
}

static void emit_utf8(Buf *o, uint32_t cp)
{
    char s[4];
    int n;
    if (cp < 0x80) {
        s[0] = (char)cp; n = 1;
    } else if (cp < 0x800) {
        s[0] = (char)(0xC0 | cp >> 6); s[1] = (char)(0x80 | (cp & 0x3F)); n = 2;
    } else if (cp < 0x10000) {
        s[0] = (char)(0xE0 | cp >> 12); s[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        s[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        s[0] = (char)(0xF0 | cp >> 18); s[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        s[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); s[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    buf_append(o, s, (size_t)n);
}

static void emit_mode(Buf *o, int want, int *sent, const char *on, const char *off)
{
    if (want != *sent) {
        buf_append(o, want ? on : off, strlen(want ? on : off));
        *sent = want;
    }
}

/* Hands the copy buffer to the terminal's own clipboard, for terminals that
 * take OSC 52. Harmless where they don't. */
static void emit_clipboard(Buf *o)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = MIN(S.clip.len, 64u * 1024);
    if (!n)
        return;
    buf_append(o, "\033]52;c;", 7);
    for (size_t i = 0; i < n; i += 3) {
        unsigned char a = (unsigned char)S.clip.data[i];
        unsigned char b = i + 1 < n ? (unsigned char)S.clip.data[i + 1] : 0;
        unsigned char c = i + 2 < n ? (unsigned char)S.clip.data[i + 2] : 0;
        char q[4] = {
            b64[a >> 2], b64[((a & 3) << 4) | (b >> 4)],
            i + 1 < n ? b64[((b & 15) << 2) | (c >> 6)] : '=',
            i + 2 < n ? b64[c & 63] : '=',
        };
        buf_append(o, q, 4);
    }
    buf_append(o, "\a", 1);
}

void render(Buf *out)
{
    static Buf o;
    ensure_frame();
    Rect focus = compose();

    o.len = 0;
    buf_append(&o, "\033[?25l", 6);
    if (S.full_redraw)
        buf_append(&o, "\033[0m\033[2J", 8);

    Cell pen;
    int pen_valid = 0, ox = -1, oy = -1;
    for (int y = 0; y < S.rows; y++) {
        for (int x = 0; x < S.cols;) {
            const Cell *f = frame + y * S.cols + x, *p = prev + y * S.cols + x;
            int wide = f->ch != 0 && f->ch != CH_WIDE_CONT && x + 1 < S.cols &&
                       f[1].ch == CH_WIDE_CONT;
            int step = wide ? 2 : 1;
            if (!S.full_redraw && cell_eq(f, p) && (!wide || cell_eq(f + 1, p + 1))) {
                x += step;
                continue;
            }
            if (ox != x || oy != y)
                buf_appendf(&o, "\033[%d;%dH", y + 1, x + 1);
            if (!pen_valid || f->attr != pen.attr || f->fg != pen.fg || f->bg != pen.bg) {
                emit_sgr(&o, f);
                pen = *f;
                pen_valid = 1;
            }
            uint32_t ch = f->ch;
            if (ch == 0 || ch == CH_WIDE_CONT ||
                (!wide && ch >= 0x1100 && wcwidth((wchar_t)ch) == 2))
                ch = ' ';
            emit_utf8(&o, ch);
            x += step;
            ox = x < S.cols ? x : -1;
            oy = y;
        }
    }
    memcpy(prev, frame, (size_t)frame_n * sizeof(Cell));

    Window *w = focused_window();
    VTerm *vt = w ? w->vt : NULL;
    if (vt) {
        emit_mode(&o, S.cm.active ? 0 : vt->app_cursor, &sent_app_cursor, "\033[?1h", "\033[?1l");
        emit_mode(&o, S.cm.active ? 0 : vt->app_keypad, &sent_app_keypad, "\033=", "\033>");
        emit_mode(&o, vt->bracketed_paste, &sent_paste, "\033[?2004h", "\033[?2004l");
    }
    if (S.clip_osc) {
        emit_clipboard(&o);
        S.clip_osc = 0;
    }

    int show_cursor = 0, cx = 0, cy = 0;
    if (S.cm.active) {
        cy = focus.y + S.cm.line - S.cm.view_top;
        cx = focus.x + MIN(S.cm.col, focus.w - 1);
        show_cursor = cy >= focus.y && cy < focus.y + focus.h;
    } else if (S.prompt != PROMPT_NONE) {
        show_cursor = 0;
    } else if (vt && vt->cursor_visible && !S.help && !S.picker) {
        cy = focus.y + vt->cy;
        cx = focus.x + MIN(vt->cx, focus.w - 1);
        show_cursor = vt->cy < focus.h;
    }
    if (show_cursor)
        buf_appendf(&o, "\033[0m\033[%d;%dH\033[?25h", cy + 1, cx + 1);

    msg_put(out, MSG_OUTPUT, o.data, (uint32_t)o.len);
    if (o.cap > (1u << 20))
        buf_free(&o);
    S.full_redraw = 0;
    S.dirty = 0;
}
