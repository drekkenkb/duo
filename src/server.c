/* The session server: owns the windows, keeps running while nobody is
 * attached, and renders for whichever client is attached. */
#include "conf.h"
#include "config.h"
#include "duo.h"
#include "proto.h"
#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MAX_CONNS 8

Session S;

typedef struct {
    int fd;
    Buf in, out;
} Conn;

static Conn conns[MAX_CONNS];
static int  attached = -1;
static int  want_detach;
static int  prefix_pending;
static int  sig_pipe[2];

long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

void set_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S.message, sizeof S.message, fmt, ap);
    va_end(ap);
    S.message_until = now_ms() + MESSAGE_MS;
    S.dirty = 1;
}

static void on_signal(int sig)
{
    int saved = errno;
    unsigned char c = (unsigned char)sig;
    if (write(sig_pipe[1], &c, 1) < 0) {
        /* full pipe: a wakeup is already queued */
    }
    errno = saved;
}

/* ---- windows exiting -------------------------------------------------- */

static void win_exited(int i)
{
    Window *w = S.win[i];
    char name[NAME_LEN];
    snprintf(name, sizeof name, "%s", win_label(w));
    int number = i + 1;

    w->alive = 0;
    w->pid = -1;
    win_close_fd(w);
    session_close_window(i);
    if (!S.quit)
        set_message("window %d (%s) closed", number, name);
}

static void reap(void)
{
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        for (int i = 0; i < S.nwins; i++)
            if (S.win[i]->pid == pid) {
                win_exited(i);
                break;
            }
}

/* ---- pane focus by direction ------------------------------------------ */

static void focus_direction(int dx, int dy)
{
    Rect r[MAX_PANES];
    layout_rects(S.layout, S.npanes, S.rows, S.cols, r);
    Rect f = r[S.focus];
    int best = -1, best_d = 1 << 30;
    for (int p = 0; p < S.npanes; p++) {
        if (p == S.focus)
            continue;
        Rect c = r[p];
        int ok = (dx < 0 && c.x + c.w <= f.x) || (dx > 0 && c.x >= f.x + f.w) ||
                 (dy < 0 && c.y + c.h <= f.y) || (dy > 0 && c.y >= f.y + f.h);
        if (!ok)
            continue;
        int d = abs(c.x - f.x) + abs(c.y - f.y);
        if (d < best_d) {
            best_d = d;
            best = p;
        }
    }
    if (best >= 0)
        session_focus_pane(best);
}

/* ---- prompts and overlays --------------------------------------------- */

static void prompt_open(int kind, const char *prefill)
{
    S.prompt = kind;
    snprintf(S.prompt_buf, sizeof S.prompt_buf, "%s", prefill ? prefill : "");
    S.prompt_len = (int)strlen(S.prompt_buf);
    S.dirty = 1;
}

static void prompt_commit(void)
{
    int kind = S.prompt;
    S.prompt = PROMPT_NONE;
    S.dirty = 1;
    if (kind == PROMPT_RENAME) {
        Window *w = focused_window();
        if (w) {
            snprintf(w->name, sizeof w->name, "%.*s", (int)sizeof w->name - 1, S.prompt_buf);
            set_message(w->name[0] ? "renamed to %s" : "name cleared", w->name);
        }
    } else if (kind == PROMPT_SEARCH) {
        cm_search(S.prompt_buf, S.cm.dir ? S.cm.dir : -1);
    }
}

void prompt_keys(const char *p, size_t n)
{
    if (S.prompt == PROMPT_KILL || S.prompt == PROMPT_QUIT) {
        int kind = S.prompt;
        S.prompt = PROMPT_NONE;
        S.dirty = 1;
        if (p[0] == 'y' || p[0] == 'Y') {
            if (kind == PROMPT_QUIT)
                S.quit = 1;
            else
                session_close_window(S.pane[S.focus].win);
        }
        return;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '\r' || c == '\n') {
            prompt_commit();
            return;
        }
        if (c == 0x1b) {
            S.prompt = PROMPT_NONE;
            S.dirty = 1;
            return;
        }
        if (c == 0x7f || c == 0x08) {
            while (S.prompt_len > 0 &&
                   ((unsigned char)S.prompt_buf[--S.prompt_len] & 0xC0) == 0x80)
                ; /* step back over a whole character */
            S.prompt_buf[S.prompt_len] = 0;
        } else if (c == 0x15) {
            S.prompt_len = 0;
            S.prompt_buf[0] = 0;
        } else if (c >= 0x20 && S.prompt_len < (int)sizeof S.prompt_buf - 1) {
            S.prompt_buf[S.prompt_len++] = (char)c;
            S.prompt_buf[S.prompt_len] = 0;
        }
    }
    S.dirty = 1;
}

static void picker_keys(const char *p, size_t n)
{
    if (n >= 3 && p[0] == 0x1b && (p[1] == '[' || p[1] == 'O')) {
        if (p[2] == 'A')
            S.picker_sel--;
        else if (p[2] == 'B')
            S.picker_sel++;
    } else if (n >= 1) {
        switch (p[0]) {
        case 'k': S.picker_sel--; break;
        case 'j': S.picker_sel++; break;
        case '\r': case '\n':
            S.picker = 0;
            session_select_window(S.picker_sel);
            S.dirty = 1;
            return;
        case 0x1b: case 'q':
            S.picker = 0;
            S.dirty = 1;
            return;
        default:
            if (p[0] >= '1' && p[0] <= '9') {
                S.picker = 0;
                session_select_window(p[0] - '1');
                S.dirty = 1;
                return;
            }
            break;
        }
    }
    S.picker_sel = MAX(0, MIN(S.picker_sel, S.nwins - 1));
    S.dirty = 1;
}

static void list_windows_message(void)
{
    char text[160];
    size_t at = 0;
    for (int i = 0; i < S.nwins && at + 8 < sizeof text; i++)
        at += (size_t)snprintf(text + at, sizeof text - at, "%s%d:%s",
                               i ? "  " : "", i + 1, win_label(S.win[i]));
    set_message("%s", text);
}

/* A split layout needs something to split: with a single pane open, make the
 * second one rather than just relabelling the layout. */
static void set_layout(int lay)
{
    if (lay != LAYOUT_TABS && S.npanes < 2 && S.nwins > 1) {
        int keep = S.focus;
        session_add_pane();
        S.focus = MIN(keep, S.npanes - 1);
    }
    S.layout = lay;
    session_apply_layout();
    set_message("layout: %s", layout_name(lay));
}

/* ---- keys ------------------------------------------------------------- */

/* Runs one command after the prefix. Returns 1 if the rest of the chunk
 * should be dropped. */
static int prefix_command(const char *p, size_t n)
{
    unsigned char k = (unsigned char)p[0];

    if (k == 0x1b && n >= 3 && (p[1] == '[' || p[1] == 'O')) {
        switch (p[2]) {
        case 'A': focus_direction(0, -1); break;
        case 'B': focus_direction(0, 1); break;
        case 'C': focus_direction(1, 0); break;
        case 'D': focus_direction(-1, 0); break;
        }
        return 1;
    }
    if (k == PREFIX_KEY) {
        session_select_window(S.last_win);
        return 1;
    }
    if (k == PREFIX_LETTER) {
        char c = PREFIX_KEY;
        Window *w = focused_window();
        if (w)
            win_write(w, &c, 1);
        return 0;
    }

    switch (k) {
    case 'c': session_new_window(); break;
    case 'k': S.prompt = PROMPT_KILL; break;
    case 'A': {
        Window *w = focused_window();
        prompt_open(PROMPT_RENAME, w ? w->name : "");
        break;
    }
    case '"':
        S.picker = 1;
        S.picker_sel = S.pane[S.focus].win;
        break;
    case 'w': list_windows_message(); break;
    case 'n': session_next_window(1); break;
    case 'p': session_next_window(-1); break;
    case '0': session_select_window(9); break;
    case '\t': case 'o':
        session_focus_pane((S.focus + 1) % S.npanes);
        break;
    case 'S': session_add_pane(); break;
    case 'X': session_close_pane(); break;
    case 't': set_layout(LAYOUT_TABS); break;
    case 'v': case '|': set_layout(LAYOUT_COLS); break;
    case 's': case '-': set_layout(LAYOUT_ROWS); break;
    case 'g': set_layout(LAYOUT_GRID); break;
    case ' ': set_layout((S.layout + 1) % LAYOUT_COUNT); break;
    case '[': cm_enter(); break;
    case ']': cm_paste(); break;
    case 'd': want_detach = 1; break;
    case 'r': S.full_redraw = 1; break;
    case '?': S.help = 1; break;
    case 'Q': S.prompt = PROMPT_QUIT; break;
    case 0x1b: break;
    default:
        if (k >= '1' && k <= '9') {
            int w = k - '1';
            if (w < S.nwins)
                session_select_window(w);
            else
                set_message("no window %d (C-a c makes one)", w + 1);
        } else {
            set_message("no binding for that key (" PREFIX_NAME " ? for help)");
        }
        break;
    }
    S.dirty = 1;
    return 1;
}

static void handle_input(const char *p, size_t n)
{
    if (S.help) {
        S.help = 0;
        S.dirty = 1;
        return;
    }
    if (S.picker) {
        picker_keys(p, n);
        return;
    }
    if (S.prompt != PROMPT_NONE) {
        prompt_keys(p, n);
        return;
    }
    if (S.cm.active && !prefix_pending) {
        if (n == 1 && (unsigned char)p[0] == PREFIX_KEY) {
            prefix_pending = 1;
            S.dirty = 1;
            return;
        }
        cm_keys(p, n);
        return;
    }

    size_t i = 0;
    while (i < n) {
        if (prefix_pending) {
            prefix_pending = 0;
            S.dirty = 1;
            if (prefix_command(p + i, n - i))
                return;
            i++;
            continue;
        }
        const char *hit = memchr(p + i, PREFIX_KEY, n - i);
        size_t end = hit ? (size_t)(hit - p) : n;
        Window *w = focused_window();
        if (w && end > i)
            win_write(w, p + i, end - i);
        if (!hit)
            break;
        prefix_pending = 1;
        S.dirty = 1;
        i = end + 1;
    }
}

/* ---- connections ------------------------------------------------------ */

static void conn_close(int c)
{
    Conn *k = &conns[c];
    if (k->fd < 0)
        return;
    close(k->fd);
    k->fd = -1;
    buf_free(&k->in);
    buf_free(&k->out);
    if (c == attached) {
        attached = -1;
        prefix_pending = S.help = S.picker = 0;
        S.prompt = PROMPT_NONE;
        if (S.cm.active)
            cm_exit();
    }
}

static void conn_send_and_close(int c, int type, const char *text)
{
    Conn *k = &conns[c];
    msg_put(&k->out, type, text, (uint32_t)strlen(text));
    int flags = fcntl(k->fd, F_GETFL);
    fcntl(k->fd, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval tv = { 1, 0 };
    setsockopt(k->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    buf_flush(&k->out, k->fd);
    conn_close(c);
}

static void attach_conn(int c, int rows, int cols)
{
    if (attached >= 0 && attached != c)
        conn_send_and_close(attached, MSG_DETACH, "detached: attached from another terminal");
    attached = c;
    S.rows = rows;
    S.cols = cols;
    prefix_pending = S.help = S.picker = 0;
    S.prompt = PROMPT_NONE;
    render_reset();
    session_apply_layout();
    win_refresh_names();
}

static void conn_read(int c)
{
    Conn *k = &conns[c];
    char tmp[65536];
    ssize_t n = read(k->fd, tmp, sizeof tmp);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (n <= 0) {
        conn_close(c);
        return;
    }
    buf_append(&k->in, tmp, (size_t)n);

    for (;;) {
        int type, rows, cols;
        const char *payload;
        uint32_t len;
        long used = msg_peek(&k->in, &type, &payload, &len);
        if (used < 0) {
            conn_close(c);
            return;
        }
        if (used == 0)
            return;

        switch (type) {
        case MSG_HELLO:
            if (msg_get_size(payload, len, &rows, &cols) == 0)
                attach_conn(c, rows, cols);
            break;
        case MSG_RESIZE:
            if (c == attached && msg_get_size(payload, len, &rows, &cols) == 0) {
                S.rows = rows;
                S.cols = cols;
                session_apply_layout();
            }
            break;
        case MSG_INPUT:
            if (c == attached && len)
                handle_input(payload, len);
            break;
        case MSG_KILL:
            S.quit = 1;
            break;
        }
        buf_consume(&k->in, (size_t)used);

        if (want_detach) {
            want_detach = 0;
            if (attached >= 0)
                conn_send_and_close(attached, MSG_DETACH, "detached");
        }
        if (k->fd < 0 || S.quit)
            return;
    }
}

static void accept_conn(int lfd)
{
    int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0)
        return;
    struct ucred cr;
    socklen_t l = sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) < 0 || cr.uid != getuid()) {
        close(fd);
        return;
    }
    for (int c = 0; c < MAX_CONNS; c++) {
        if (conns[c].fd < 0) {
            memset(&conns[c], 0, sizeof conns[c]);
            conns[c].fd = fd;
            return;
        }
    }
    close(fd);
}

/* ---- main loop -------------------------------------------------------- */

static int server_run(int lfd, const char *path, int rows, int cols,
                      const char *conf_path)
{
    S.path = path;
    S.rows = rows;
    S.cols = cols;
    S.layout = LAYOUT_TABS;
    for (int c = 0; c < MAX_CONNS; c++)
        conns[c].fd = -1;

    setlocale(LC_CTYPE, "");
    if (MB_CUR_MAX == 1)
        setlocale(LC_CTYPE, "C.UTF-8"); /* wcwidth needs a UTF-8 locale */

    if (pipe2(sig_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
        return 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    SessionConf conf;
    char conf_err[256];
    int have_conf = conf_path && *conf_path &&
                    conf_load(conf_path, &conf, conf_err, sizeof conf_err) == 0;
    int nwin = have_conf ? conf.nwindows : INITIAL_WINDOWS;

    for (int i = 0; i < nwin && i < MAX_WINDOWS; i++) {
        Window *w = win_new(i + 1, MAX(rows - 1, 1), cols);
        if (!w)
            return 1;
        if (have_conf) {
            if (conf.win[i].name[0])
                snprintf(w->name, sizeof w->name, "%s", conf.win[i].name);
            if (conf.win[i].cmd[0])
                w->pending_cmd = strdup(conf.win[i].cmd);
        }
        S.win[S.nwins++] = w;
    }
    S.npanes = 1;
    S.focus = 0;
    S.pane[0].win = (have_conf && conf.focus) ? conf.focus - 1 : 0;
    if (have_conf) {
        for (int p = 1; p < conf.panes && p < MAX_PANES; p++)
            session_add_pane();
        S.focus = 0;
        if (conf.layout >= 0)
            S.layout = conf.layout;
    }
    /* so C-a C-a toggles from the start */
    S.last_win = S.nwins > 1 ? (S.pane[0].win == 0 ? 1 : 0) : 0;
    session_apply_layout();

    long long names_at = 0;

    while (!S.quit) {
        struct pollfd pfd[2 + MAX_WINDOWS + MAX_CONNS];
        int nfd = 0;
        pfd[nfd++] = (struct pollfd){ lfd, POLLIN, 0 };
        pfd[nfd++] = (struct pollfd){ sig_pipe[0], POLLIN, 0 };
        int win_first = nfd;
        for (int i = 0; i < S.nwins; i++)
            pfd[nfd++] = (struct pollfd){ S.win[i]->fd,
                                          POLLIN | (S.win[i]->in.len ? POLLOUT : 0), 0 };
        int conn_first = nfd;
        for (int c = 0; c < MAX_CONNS; c++)
            pfd[nfd++] = (struct pollfd){ conns[c].fd,
                                          POLLIN | (conns[c].out.len ? POLLOUT : 0), 0 };

        int timeout = -1;
        if (attached >= 0) {
            timeout = 1000;
            if (S.message[0])
                timeout = (int)MAX(0, MIN(timeout, S.message_until - now_ms()));
        }

        if (poll(pfd, (nfds_t)nfd, timeout) < 0 && errno != EINTR)
            break;

        if (pfd[1].revents & POLLIN) {
            unsigned char s;
            while (read(sig_pipe[0], &s, 1) == 1) {
                if (s == SIGCHLD)
                    reap();
                else
                    S.quit = 1;
            }
        }

        /* Windows can close while we work through this, so match on the fd
         * rather than trusting the index. */
        for (int i = win_first; i < conn_first && !S.quit; i++) {
            if (pfd[i].fd < 0 || !pfd[i].revents)
                continue;
            int idx = -1;
            for (int k = 0; k < S.nwins; k++)
                if (S.win[k]->fd == pfd[i].fd)
                    idx = k;
            if (idx < 0)
                continue;
            if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))
                win_read(idx);
            if (S.win[idx] && S.win[idx]->fd == pfd[i].fd && (pfd[i].revents & POLLOUT))
                buf_flush(&S.win[idx]->in, S.win[idx]->fd);
        }

        if (pfd[0].revents & POLLIN)
            accept_conn(lfd);

        for (int c = 0; c < MAX_CONNS && !S.quit; c++) {
            Conn *k = &conns[c];
            if (k->fd < 0 || k->fd != pfd[conn_first + c].fd)
                continue;
            short re = pfd[conn_first + c].revents;
            if (re & POLLOUT)
                if (buf_flush(&k->out, k->fd) < 0) {
                    conn_close(c);
                    continue;
                }
            if (re & (POLLIN | POLLHUP | POLLERR))
                conn_read(c);
        }

        long long now = now_ms();
        if (S.message[0] && now >= S.message_until) {
            S.message[0] = 0;
            S.dirty = 1;
        }
        if (attached >= 0 && now - names_at >= 1000) {
            win_refresh_names();
            names_at = now;
        }

        /* Render only once the last frame has gone out, so frames coalesce
         * on a slow link instead of piling up. */
        if (!S.quit && attached >= 0 && S.dirty && conns[attached].out.len == 0) {
            Conn *k = &conns[attached];
            render(&k->out);
            if (buf_flush(&k->out, k->fd) < 0)
                conn_close(attached);
        }
    }

    for (int c = 0; c < MAX_CONNS; c++)
        if (conns[c].fd >= 0)
            conn_send_and_close(c, MSG_EXIT, S.ended ? "all windows closed" : "session ended");
    for (int i = 0; i < S.nwins; i++)
        win_free(S.win[i]);
    buf_free(&S.clip);
    close(lfd);
    unlink(path);
    return 0;
}

int server_start(const char *path, int rows, int cols, const char *conf_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("duo: socket");
        return -1;
    }
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    unlink(path); /* only reached when nothing is listening on it */

    mode_t old = umask(077);
    int r = bind(fd, (struct sockaddr *)&sa, sizeof sa);
    umask(old);
    if (r < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, "duo: cannot listen on %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("duo: fork");
        close(fd);
        return -1;
    }
    if (pid == 0) {
        /* Double fork: the server belongs to no terminal and no ssh session,
         * so a dropped connection's SIGHUP never reaches it. */
        setsid();
        if (fork() != 0)
            _exit(0);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
            if (null > STDERR_FILENO)
                close(null);
        }
        _exit(server_run(fd, path, rows, cols, conf_path));
    }
    waitpid(pid, NULL, 0);
    close(fd);
    return 0;
}
