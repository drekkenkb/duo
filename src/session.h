/* The session model: windows (a shell each), panes (viewports onto windows),
 * and the modes layered on top of them. One session per server process. */
#ifndef DUO_SESSION_H
#define DUO_SESSION_H

#include <stdint.h>
#include <sys/types.h>

#include "buf.h"
#include "vterm.h"

#define MAX_WINDOWS 16
#define MAX_PANES   8
#define NAME_LEN    24

typedef struct { int x, y, w, h; } Rect;

typedef struct {
    VTerm  *vt;
    int     fd;             /* pty master, -1 once closed */
    pid_t   pid;            /* -1 once reaped */
    int     alive;
    Buf     in;             /* keystrokes waiting for the pty */
    char    auto_name[NAME_LEN];  /* what's running, from /proc */
    char    name[NAME_LEN];       /* set with C-a A; empty means use auto_name */
    int     activity;       /* output arrived while off screen */
    int     seen;           /* has been on screen at least once */
    char   *pending_cmd;    /* typed in once the shell shows signs of life */
} Window;

/* A pane shows exactly one window, and a window appears in at most one pane,
 * so a window only ever has one size to be resized to. */
typedef struct { int win; } Pane;

enum { LAYOUT_TABS, LAYOUT_COLS, LAYOUT_ROWS, LAYOUT_GRID, LAYOUT_COUNT };

/* Copy mode positions are in history coordinates: line 0 is the oldest
 * scrollback line, line sb_count is the top of the live screen. */
typedef struct {
    int      active;
    int      line, col;
    int      view_top;
    int      sel, sel_line, sel_col;    /* selection anchor */
    char     query[64];
    int      dir;                       /* -1 searching up, +1 down */
    int      hl_line, hl_col, hl_len;   /* last match, highlighted */
    uint64_t pushed;                    /* keeps the view pinned as output arrives */
    int      count;                     /* sb_count when pushed was sampled */
} CopyMode;

enum { PROMPT_NONE, PROMPT_RENAME, PROMPT_SEARCH, PROMPT_KILL, PROMPT_QUIT };

typedef struct {
    const char *path;

    Window  *win[MAX_WINDOWS];
    int      nwins;
    Pane     pane[MAX_PANES];
    int      npanes, focus;
    int      last_win;              /* C-a C-a goes back here */
    int      layout;

    int      rows, cols;            /* the attached client's size */

    CopyMode cm;
    Buf      clip;                  /* copy buffer, pasted with C-a ] */
    int      clip_osc;              /* send it to the terminal's clipboard too */

    int      prompt;                /* PROMPT_* */
    char     prompt_buf[128];
    int      prompt_len;
    int      prefix;                /* prefix pressed, waiting for a command key */
    int      picker, picker_sel;    /* window list overlay */
    int      help;

    char     message[160];
    long long message_until;

    int      dirty, full_redraw;
    int      quit, ended;
} Session;

extern Session S;

long long now_ms(void);
void set_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* layout.c: fills out[] with npanes rectangles, returns the layout actually
 * used (falls back to tabs when the terminal is too small to split). */
int  layout_rects(int layout, int npanes, int rows, int cols, Rect *out);
int  pane_rect(int pane, Rect *r);       /* rectangle of one pane right now */
const char *layout_name(int layout);

/* window.c */
Window *win_new(int number, int rows, int cols);
void    win_free(Window *w);
void    win_resize(Window *w, int rows, int cols);
void    win_write(Window *w, const char *p, size_t n);
void    win_feed(int i, const char *p, size_t n);
void    win_read(int i);
void    win_close_fd(Window *w);
const char *win_label(const Window *w);
void    win_refresh_names(void);

int  session_new_window(void);
void session_close_window(int i);
void session_select_window(int w);
void session_next_window(int delta);
void session_add_pane(void);
void session_close_pane(void);
void session_focus_pane(int p);
void session_apply_layout(void);
int  win_visible_pane(int w);
Window *focused_window(void);

void prompt_keys(const char *p, size_t n);   /* server.c */

/* copy.c */
void cm_enter(void);
void cm_exit(void);
void cm_keys(const char *p, size_t n);
void cm_search(const char *query, int dir);
void cm_copy_selection(void);
void cm_paste(void);
void cm_follow_output(void);
void cm_line_text(const VTerm *vt, int line, char *out, size_t n);

/* render.c */
void render(Buf *out);
void render_reset(void);   /* forget what the client's terminal was told */

#endif
