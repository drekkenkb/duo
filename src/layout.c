#include "session.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define MIN_PANE_W 8
#define MIN_PANE_H 2

static const char *names[] = { "tabs", "columns", "rows", "grid" };

const char *layout_name(int layout)
{
    return names[layout % LAYOUT_COUNT];
}

/* Splits `total` into `n` parts with single-cell dividers between them,
 * handing the remainder to the leftmost parts. */
static int split_sizes(int total, int n, int *size, int *start)
{
    int usable = total - (n - 1);
    if (n < 1 || usable < n)
        return -1;
    int base = usable / n, extra = usable % n, at = 0;
    for (int i = 0; i < n; i++) {
        size[i] = base + (i < extra);
        start[i] = at;
        at += size[i] + 1;
    }
    return 0;
}

int layout_rects(int layout, int npanes, int rows, int cols, Rect *out)
{
    int h = MAX(rows - 1, 1); /* the status bar owns the last line */

    if (npanes < 2)
        layout = LAYOUT_TABS;

    int size[MAX_PANES], start[MAX_PANES];

    if (layout == LAYOUT_COLS) {
        if (split_sizes(cols, npanes, size, start) < 0 || size[npanes - 1] < MIN_PANE_W)
            layout = LAYOUT_TABS;
        else
            for (int i = 0; i < npanes; i++)
                out[i] = (Rect){ start[i], 0, size[i], h };
    } else if (layout == LAYOUT_ROWS) {
        if (split_sizes(h, npanes, size, start) < 0 || size[npanes - 1] < MIN_PANE_H)
            layout = LAYOUT_TABS;
        else
            for (int i = 0; i < npanes; i++)
                out[i] = (Rect){ 0, start[i], cols, size[i] };
    } else if (layout == LAYOUT_GRID) {
        int ncols = 1;
        while (ncols * ncols < npanes)
            ncols++;
        int nrows = (npanes + ncols - 1) / ncols;
        int csize[MAX_PANES], cstart[MAX_PANES], rsize[MAX_PANES], rstart[MAX_PANES];
        if (split_sizes(cols, ncols, csize, cstart) < 0 ||
            split_sizes(h, nrows, rsize, rstart) < 0 ||
            csize[ncols - 1] < MIN_PANE_W || rsize[nrows - 1] < MIN_PANE_H) {
            layout = LAYOUT_TABS;
        } else {
            for (int i = 0; i < npanes; i++) {
                int r = i / ncols, c = i % ncols;
                int span = (r == nrows - 1 && npanes % ncols) ? npanes % ncols : ncols;
                /* the last row stretches its panes across the full width */
                int w = csize[c], x = cstart[c];
                if (span != ncols) {
                    int s2[MAX_PANES], st2[MAX_PANES];
                    if (split_sizes(cols, span, s2, st2) == 0) {
                        w = s2[c];
                        x = st2[c];
                    }
                }
                out[i] = (Rect){ x, rstart[r], w, rsize[r] };
            }
        }
    }

    if (layout == LAYOUT_TABS)
        for (int i = 0; i < npanes; i++)
            out[i] = (Rect){ 0, 0, cols, h };

    return layout;
}

int pane_rect(int pane, Rect *r)
{
    Rect all[MAX_PANES];
    int lay = layout_rects(S.layout, S.npanes, S.rows, S.cols, all);
    if (pane < 0 || pane >= S.npanes)
        return LAYOUT_TABS;
    *r = all[pane];
    return lay;
}
