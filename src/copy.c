/* Copy mode: move around the scrollback, search it, select and copy. */
#include "session.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static VTerm *cm_vt(void)
{
    Window *w = focused_window();
    return w ? w->vt : NULL;
}

static int view_height(void)
{
    Rect r;
    pane_rect(S.focus, &r);
    return MAX(r.h, 1);
}

static int max_line(const VTerm *vt)
{
    return vt->sb_count + vt->rows - 1;
}

/* One history line as UTF-8, trailing blanks trimmed. */
void cm_line_text(const VTerm *vt, int line, char *out, size_t n)
{
    Cell cells[1024];
    int cols = MIN(vt->cols, (int)(sizeof cells / sizeof *cells));
    vt_history_line(vt, line, cells, cols);

    int end = cols;
    while (end > 0 && (cells[end - 1].ch == 0 || cells[end - 1].ch == ' '))
        end--;

    size_t at = 0;
    for (int x = 0; x < end && at + 4 < n; x++) {
        uint32_t c = cells[x].ch;
        if (c == CH_WIDE_CONT)
            continue;
        if (c == 0)
            c = ' ';
        if (c < 0x80) {
            out[at++] = (char)c;
        } else if (c < 0x800) {
            out[at++] = (char)(0xC0 | c >> 6);
            out[at++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[at++] = (char)(0xE0 | c >> 12);
            out[at++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[at++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[at++] = (char)(0xF0 | c >> 18);
            out[at++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[at++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[at++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[at] = 0;
}

/* Lower-cased ASCII, one char per cell, so a match's column is a cell column.
 * Non-ASCII cells become \x01, which no query can contain. */
static int line_ascii(const VTerm *vt, int line, char *out, int cap)
{
    Cell cells[1024];
    int cols = MIN(vt->cols, MIN(cap - 1, (int)(sizeof cells / sizeof *cells)));
    vt_history_line(vt, line, cells, cols);
    for (int x = 0; x < cols; x++) {
        uint32_t c = cells[x].ch;
        if (c == 0 || c == CH_WIDE_CONT)
            out[x] = ' ';
        else if (c < 0x80)
            out[x] = (char)tolower((int)c);
        else
            out[x] = 1;
    }
    out[cols] = 0;
    return cols;
}

static void scroll_into_view(void)
{
    int h = view_height();
    if (S.cm.line < S.cm.view_top)
        S.cm.view_top = S.cm.line;
    if (S.cm.line > S.cm.view_top + h - 1)
        S.cm.view_top = S.cm.line - h + 1;
    if (S.cm.view_top < 0)
        S.cm.view_top = 0;
}

void cm_enter(void)
{
    VTerm *vt = cm_vt();
    if (!vt)
        return;
    memset(&S.cm, 0, sizeof S.cm);
    S.cm.active = 1;
    S.cm.line = vt->sb_count + vt->cy;
    S.cm.col = vt->cx;
    S.cm.view_top = vt->sb_count;
    S.cm.pushed = vt->sb_pushed;
    S.cm.count = vt->sb_count;
    S.cm.hl_len = 0;
    scroll_into_view();
    S.dirty = 1;
}

void cm_exit(void)
{
    S.cm.active = 0;
    S.dirty = 1;
}

/* Output while copy mode is open shifts history indices only once the
 * scrollback ring is full and starts dropping its oldest lines. */
void cm_follow_output(void)
{
    VTerm *vt = cm_vt();
    if (!vt || !S.cm.active)
        return;
    uint64_t pushed = vt->sb_pushed - S.cm.pushed;
    int grew = vt->sb_count - S.cm.count;
    int dropped = (int)pushed - grew;
    S.cm.pushed = vt->sb_pushed;
    S.cm.count = vt->sb_count;
    if (dropped > 0) {
        S.cm.line = MAX(0, S.cm.line - dropped);
        S.cm.view_top = MAX(0, S.cm.view_top - dropped);
        S.cm.sel_line = MAX(0, S.cm.sel_line - dropped);
        S.cm.hl_line = MAX(0, S.cm.hl_line - dropped);
    }
}

void cm_search(const char *query, int dir)
{
    VTerm *vt = cm_vt();
    if (!vt || !query[0])
        return;
    snprintf(S.cm.query, sizeof S.cm.query, "%s", query);
    S.cm.dir = dir;

    char needle[64];
    size_t nl = 0;
    for (size_t i = 0; query[i] && nl + 1 < sizeof needle; i++)
        needle[nl++] = (char)tolower((unsigned char)query[i]);
    needle[nl] = 0;

    char hay[1025];
    int last = max_line(vt);
    for (int step = 1; step <= last + 1; step++) {
        int line = S.cm.line + dir * step;
        if (line < 0 || line > last)
            break;
        line_ascii(vt, line, hay, (int)sizeof hay);
        const char *hit = strstr(hay, needle);
        if (hit) {
            S.cm.line = line;
            S.cm.col = (int)(hit - hay);
            S.cm.hl_line = line;
            S.cm.hl_col = S.cm.col;
            S.cm.hl_len = (int)nl;
            scroll_into_view();
            S.dirty = 1;
            return;
        }
    }
    set_message("not found: %s", query);
}

void cm_copy_selection(void)
{
    VTerm *vt = cm_vt();
    if (!vt)
        return;
    int l0 = S.cm.sel ? S.cm.sel_line : S.cm.line, c0 = S.cm.sel ? S.cm.sel_col : 0;
    int l1 = S.cm.line, c1 = S.cm.col;
    if (!S.cm.sel)
        c1 = vt->cols - 1; /* no selection: copy the cursor's line */
    if (l0 > l1 || (l0 == l1 && c0 > c1)) {
        int t = l0; l0 = l1; l1 = t;
        t = c0; c0 = c1; c1 = t;
    }

    S.clip.len = 0;
    char text[4096];
    for (int line = l0; line <= l1; line++) {
        cm_line_text(vt, line, text, sizeof text);
        int len = (int)strlen(text);
        int start = (line == l0) ? c0 : 0;
        int end = (line == l1) ? MIN(c1 + 1, len) : len;
        /* start/end are cell columns; for ASCII text they are byte offsets. */
        start = MIN(start, len);
        end = MAX(end, start);
        if (end > start)
            buf_append(&S.clip, text + start, (size_t)(end - start));
        if (line != l1)
            buf_append(&S.clip, "\n", 1);
    }
    S.clip_osc = 1;
    set_message("copied %d line%s (%zu bytes) - C-a ] pastes",
                l1 - l0 + 1, l1 == l0 ? "" : "s", S.clip.len);
    cm_exit();
}

void cm_paste(void)
{
    Window *w = focused_window();
    if (!w || !S.clip.len) {
        set_message("nothing copied yet (C-a [ then Space, move, Enter)");
        return;
    }
    if (w->vt->bracketed_paste)
        win_write(w, "\033[200~", 6);
    win_write(w, S.clip.data, S.clip.len);
    if (w->vt->bracketed_paste)
        win_write(w, "\033[201~", 6);
}

static void cm_key_one(const char *p, size_t n)
{
    VTerm *vt = cm_vt();
    if (!vt)
        return;
    int h = view_height();
    int last = max_line(vt);
    int moved = 1;

    if (n >= 3 && p[0] == 0x1b && (p[1] == '[' || p[1] == 'O')) {
        switch (p[2]) {
        case 'A': S.cm.line--; break;
        case 'B': S.cm.line++; break;
        case 'C': S.cm.col++; break;
        case 'D': S.cm.col--; break;
        case 'H': S.cm.col = 0; break;
        case 'F': S.cm.col = vt->cols - 1; break;
        case '5': S.cm.line -= h; break;
        case '6': S.cm.line += h; break;
        default:  moved = 0; break;
        }
    } else if (n == 1) {
        switch (p[0]) {
        case 'q': case 0x1b: cm_exit(); return;
        case 'k': S.cm.line--; break;
        case 'j': S.cm.line++; break;
        case 'h': S.cm.col--; break;
        case 'l': S.cm.col++; break;
        case 'b': case 0x02: S.cm.line -= h; break;
        case 'f': case 0x06: S.cm.line += h; break;
        case 'g': S.cm.line = 0; break;
        case 'G': S.cm.line = last; break;
        case '0': S.cm.col = 0; break;
        case '$': S.cm.col = vt->cols - 1; break;
        case ' ':
            S.cm.sel = !S.cm.sel;
            S.cm.sel_line = S.cm.line;
            S.cm.sel_col = S.cm.col;
            set_message(S.cm.sel ? "selection started; Enter copies" : "selection cleared");
            break;
        case '\r': case '\n': case 'y':
            cm_copy_selection();
            return;
        case 'n':
            if (S.cm.query[0])
                cm_search(S.cm.query, S.cm.dir ? S.cm.dir : -1);
            break;
        case 'N':
            if (S.cm.query[0])
                cm_search(S.cm.query, S.cm.dir > 0 ? -1 : 1);
            break;
        case '/':
            S.prompt = PROMPT_SEARCH;
            S.prompt_len = 0;
            S.prompt_buf[0] = 0;
            S.cm.dir = -1;
            break;
        case '?':
            S.prompt = PROMPT_SEARCH;
            S.prompt_len = 0;
            S.prompt_buf[0] = 0;
            S.cm.dir = 1;
            break;
        default: moved = 0; break;
        }
    } else {
        moved = 0;
    }

    if (moved) {
        S.cm.line = MAX(0, MIN(S.cm.line, last));
        S.cm.col = MAX(0, MIN(S.cm.col, vt->cols - 1));
        scroll_into_view();
    }
    S.dirty = 1;
}

/* Input can arrive in bursts (fast typing, or a paste), so walk the chunk
 * key by key instead of looking at only the first one. */
void cm_keys(const char *p, size_t n)
{
    size_t i = 0;
    while (i < n && S.cm.active) {
        if (S.prompt != PROMPT_NONE) {
            prompt_keys(p + i, n - i);   /* '/' opened the search prompt */
            return;
        }
        size_t used = 1;
        if (p[i] == 0x1b && i + 2 < n && (p[i + 1] == '[' || p[i + 1] == 'O')) {
            used = 3;
            if (p[i + 2] >= '0' && p[i + 2] <= '9') {
                while (i + used < n && p[i + used] != '~')
                    used++;
                if (i + used < n)
                    used++;
            }
        }
        cm_key_one(p + i, used);
        i += used;
    }
}
