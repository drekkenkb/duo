/* Windows (one shell each) and the rules tying them to panes. */
#include "config.h"
#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define WIN_INBUF_MAX (1u << 20)

const char *win_label(const Window *w)
{
    return w->name[0] ? w->name : w->auto_name;
}

Window *win_new(int number, int rows, int cols)
{
    rows = MAX(rows, 1);
    cols = MAX(cols, 1);
    Window *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->vt = vt_new(rows, cols);
    if (!w->vt) {
        free(w);
        return NULL;
    }
    w->fd = -1;
    w->pid = -1;
    snprintf(w->auto_name, sizeof w->auto_name, "shell");

    struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols };
    int fd;
    pid_t pid = forkpty(&fd, NULL, NULL, &ws);
    if (pid < 0) {
        vt_feed(w->vt, "\r\n[duo: could not start a shell]\r\n", 34);
        return w;
    }
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_DFL;
        for (int s = 1; s < NSIG; s++)
            sigaction(s, &sa, NULL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);

        char num[8];
        snprintf(num, sizeof num, "%d", number);
        setenv("TERM", INNER_TERM, 1);
        setenv("DUO_SESSION", S.path ? S.path : "", 1);
        setenv("DUO_WINDOW", num, 1);

        const char *shell = getenv("SHELL");
        if (!shell || !*shell) {
            struct passwd *pw = getpwuid(getuid());
            shell = (pw && pw->pw_shell && *pw->pw_shell) ? pw->pw_shell : "/bin/sh";
        }
        const char *base = strrchr(shell, '/');
        execl(shell, base ? base + 1 : shell, (char *)NULL);
        _exit(127);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    w->fd = fd;
    w->pid = pid;
    w->alive = 1;
    return w;
}

void win_close_fd(Window *w)
{
    if (w->fd >= 0)
        close(w->fd);
    w->fd = -1;
    buf_free(&w->in);
}

void win_free(Window *w)
{
    if (!w)
        return;
    free(w->pending_cmd);
    if (w->pid > 0)
        kill(-w->pid, SIGHUP);
    win_close_fd(w);
    vt_free(w->vt);
    free(w);
}

void win_resize(Window *w, int rows, int cols)
{
    rows = MAX(rows, 1);
    cols = MAX(cols, 1);
    if (w->vt->rows == rows && w->vt->cols == cols)
        return;
    vt_resize(w->vt, rows, cols);
    if (w->fd >= 0) {
        struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols };
        ioctl(w->fd, TIOCSWINSZ, &ws);
    }
}

void win_write(Window *w, const char *p, size_t n)
{
    if (!n || w->fd < 0 || w->in.len + n > WIN_INBUF_MAX)
        return;
    buf_append(&w->in, p, n);
    buf_flush(&w->in, w->fd);
}

int win_visible_pane(int w)
{
    for (int p = 0; p < S.npanes; p++)
        if (S.pane[p].win == w)
            return p;
    return -1;
}

Window *focused_window(void)
{
    int w = S.pane[S.focus].win;
    return (w >= 0 && w < S.nwins) ? S.win[w] : NULL;
}

/* Types a session file's command into the window once the shell is ready
 * for it. Typed in while the pty is still in cooked mode with echo on (the
 * shell starting up), the kernel echoes it straight away and the line editor
 * shows it again after the prompt, so it appears twice. Line editors (zsh's
 * ZLE, readline) switch canonical mode off while they read, so wait for that.
 * Shells without one never switch, so fall back to the output going quiet
 * for a moment, or a hard limit if the shell prints nothing at all.
 * Returns 1 while a command is still waiting. */
int win_send_pending(Window *w)
{
    if (!w->pending_cmd)
        return 0;
    if (w->fd < 0) {
        free(w->pending_cmd);
        w->pending_cmd = NULL;
        return 0;
    }
    long long now = now_ms();
    struct termios t;
    int editor = tcgetattr(w->fd, &t) == 0 && !(t.c_lflag & ICANON);
    int quiet = w->last_output && now - w->last_output >= PENDING_QUIET_MS;
    int stale = now - w->pending_since >= PENDING_MAX_MS;
    if (!editor && !quiet && !stale)
        return 1;
    char *cmd = w->pending_cmd;
    w->pending_cmd = NULL;
    win_write(w, cmd, strlen(cmd));
    win_write(w, "\n", 1);
    free(cmd);
    return 0;
}

void win_feed(int i, const char *p, size_t n)
{
    Window *w = S.win[i];
    vt_feed(w->vt, p, n);
    w->last_output = now_ms();
    win_send_pending(w);
    if (w->vt->reply_len) {
        win_write(w, w->vt->reply, w->vt->reply_len);
        w->vt->reply_len = 0;
    }
    if (win_visible_pane(i) >= 0) {
        S.dirty = 1;
        if (S.cm.active && win_visible_pane(i) == S.focus)
            cm_follow_output();
    } else if (!w->activity && w->seen) {
        w->activity = 1;
        S.dirty = 1; /* the status bar marks it */
    }
}

void win_read(int i)
{
    Window *w = S.win[i];
    char buf[65536];
    /* Bounded, so one noisy window can't starve everything else. */
    for (int k = 0; k < 4 && w->fd >= 0; k++) {
        ssize_t n = read(w->fd, buf, sizeof buf);
        if (n > 0) {
            win_feed(i, buf, (size_t)n);
            if ((size_t)n < sizeof buf)
                break;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        win_close_fd(w); /* EIO: the shell is gone or going */
        break;
    }
}

void win_refresh_names(void)
{
    for (int i = 0; i < S.nwins; i++) {
        Window *w = S.win[i];
        char name[NAME_LEN] = "shell";
        pid_t pg = w->fd >= 0 ? tcgetpgrp(w->fd) : -1;
        if (pg > 0) {
            char path[64];
            snprintf(path, sizeof path, "/proc/%d/comm", (int)pg);
            FILE *f = fopen(path, "r");
            if (f) {
                if (fgets(name, sizeof name, f))
                    name[strcspn(name, "\n")] = 0;
                fclose(f);
            }
        } else if (!w->alive) {
            snprintf(name, sizeof name, "exited");
        }
        if (strcmp(name, w->auto_name) != 0) {
            snprintf(w->auto_name, sizeof w->auto_name, "%s", name);
            S.dirty = 1;
        }
    }
}

void session_apply_layout(void)
{
    Rect r[MAX_PANES];
    layout_rects(S.layout, S.npanes, S.rows, S.cols, r);
    for (int p = 0; p < S.npanes; p++) {
        int w = S.pane[p].win;
        if (w >= 0 && w < S.nwins)
            win_resize(S.win[w], r[p].h, r[p].w);
    }
    S.full_redraw = S.dirty = 1;
}

void session_focus_pane(int p)
{
    if (p < 0 || p >= S.npanes || p == S.focus)
        return;
    if (S.cm.active)
        cm_exit();
    S.focus = p;
    Window *w = focused_window();
    if (w)
        w->activity = 0;
    S.dirty = 1;
}

/* The first window not already on screen. */
static int first_hidden_window(void)
{
    for (int w = 0; w < S.nwins; w++)
        if (win_visible_pane(w) < 0)
            return w;
    return -1;
}

int session_new_window(void)
{
    if (S.nwins >= MAX_WINDOWS) {
        set_message("window limit reached (%d)", MAX_WINDOWS);
        return -1;
    }
    Rect r;
    pane_rect(S.focus, &r);
    if (r.w < 1 || r.h < 1) {
        r.w = S.cols > 0 ? S.cols : 80;
        r.h = S.rows > 1 ? S.rows - 1 : 24;
    }
    Window *w = win_new(S.nwins + 1, r.h, r.w);
    if (!w) {
        set_message("out of memory");
        return -1;
    }
    S.win[S.nwins++] = w;
    session_select_window(S.nwins - 1);
    return S.nwins - 1;
}

void session_select_window(int w)
{
    if (w < 0 || w >= S.nwins)
        return;
    int cur = S.pane[S.focus].win;
    if (w == cur)
        return;
    if (S.cm.active)
        cm_exit();

    int other = win_visible_pane(w);
    if (other >= 0)
        S.pane[other].win = cur;   /* swap, keeping one pane per window */
    S.last_win = cur;
    S.pane[S.focus].win = w;
    S.win[w]->activity = 0;
    session_apply_layout();
}

void session_next_window(int delta)
{
    if (S.nwins < 2)
        return;
    int cur = S.pane[S.focus].win;
    for (int step = 1; step <= S.nwins; step++) {
        int w = (cur + delta * step % S.nwins + S.nwins * 2) % S.nwins;
        if (w == cur)
            continue;
        session_select_window(w);
        return;
    }
}

void session_add_pane(void)
{
    if (S.npanes >= MAX_PANES) {
        set_message("pane limit reached (%d)", MAX_PANES);
        return;
    }
    int w = first_hidden_window();
    if (w < 0) {
        if (S.nwins >= MAX_WINDOWS) {
            set_message("no free window to show; close one first");
            return;
        }
        Window *nw = win_new(S.nwins + 1, 24, 80);
        if (!nw) {
            set_message("out of memory");
            return;
        }
        S.win[S.nwins++] = nw;
        w = S.nwins - 1;
    }
    S.pane[S.npanes].win = w;
    S.focus = S.npanes;
    S.npanes++;
    if (S.layout == LAYOUT_TABS)
        S.layout = (S.cols >= 2 * S.rows) ? LAYOUT_COLS : LAYOUT_ROWS;
    session_apply_layout();
}

void session_close_pane(void)
{
    if (S.npanes < 2) {
        set_message("only one pane; C-a k closes the window");
        return;
    }
    if (S.cm.active)
        cm_exit();
    for (int p = S.focus; p + 1 < S.npanes; p++)
        S.pane[p] = S.pane[p + 1];
    S.npanes--;
    if (S.focus >= S.npanes)
        S.focus = S.npanes - 1;
    if (S.npanes == 1)
        S.layout = LAYOUT_TABS;
    session_apply_layout();
}

void session_close_window(int i)
{
    if (i < 0 || i >= S.nwins)
        return;
    if (S.cm.active && win_visible_pane(i) == S.focus)
        cm_exit();

    win_free(S.win[i]);
    for (int k = i; k + 1 < S.nwins; k++)
        S.win[k] = S.win[k + 1];
    S.nwins--;

    if (S.nwins == 0) {
        S.ended = 1;
        S.quit = 1;
        return;
    }

    for (int p = 0; p < S.npanes; p++) {
        if (S.pane[p].win == i)
            S.pane[p].win = -1;
        else if (S.pane[p].win > i)
            S.pane[p].win--;
    }
    if (S.last_win == i)
        S.last_win = 0;
    else if (S.last_win > i)
        S.last_win--;

    /* Panes that showed it get another window, or go away. */
    for (int p = 0; p < S.npanes; p++) {
        if (S.pane[p].win >= 0)
            continue;
        int w = first_hidden_window();
        if (w >= 0) {
            S.pane[p].win = w;
        } else {
            for (int k = p; k + 1 < S.npanes; k++)
                S.pane[k] = S.pane[k + 1];
            S.npanes--;
            p--;
        }
    }
    if (S.npanes < 1) {
        S.npanes = 1;
        S.pane[0].win = 0;
    }
    if (S.focus >= S.npanes)
        S.focus = S.npanes - 1;
    if (S.npanes == 1)
        S.layout = LAYOUT_TABS;
    session_apply_layout();
}
