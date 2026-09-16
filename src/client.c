/* The attached client: raw terminal in, rendered screen out. All the real
 * work happens in the server, so a dropped ssh connection loses nothing. */
#include "duo.h"
#include "proto.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* Outer terminal: switch to its alternate screen so detaching restores the
 * shell you attached from, and disable autowrap so drawing the bottom-right
 * cell can't scroll. */
static const char ENTER_SEQ[] = "\033[?1049h\033[?7l\033[H\033[2J";
static const char LEAVE_SEQ[] =
    "\033[0m\033[?7h\033[?25h\033[?1l\033>\033[?2004l\033[?1049l";

static int sig_pipe[2] = { -1, -1 };

static void on_signal(int sig)
{
    int saved = errno;
    unsigned char c = (unsigned char)sig;
    if (write(sig_pipe[1], &c, 1) < 0) {
        /* pipe full: a wakeup is already pending */
    }
    errno = saved;
}

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                struct pollfd pfd = { fd, POLLOUT, 0 };
                poll(&pfd, 1, -1);
                continue;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int send_buf(int fd, Buf *b)
{
    int r = write_all(fd, b->data, b->len);
    b->len = 0;
    return r;
}

static void term_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

int client_attach(int fd)
{
    struct termios saved, raw;
    if (tcgetattr(STDIN_FILENO, &saved) < 0) {
        perror("duo: tcgetattr");
        return 1;
    }
    raw = saved;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (pipe(sig_pipe) < 0) {
        perror("duo: pipe");
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGWINCH, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    write_all(STDOUT_FILENO, ENTER_SEQ, sizeof ENTER_SEQ - 1);

    Buf out = { 0 }, in = { 0 };
    int rows, cols;
    term_size(&rows, &cols);
    msg_put_size(&out, MSG_HELLO, rows, cols);
    send_buf(fd, &out);

    const char *reason = "lost connection to the session";
    char reason_buf[256];
    int status = 1;
    int done = 0;
    char tmp[65536];

    while (!done) {
        struct pollfd pfd[3] = {
            { STDIN_FILENO, POLLIN, 0 },
            { fd, POLLIN, 0 },
            { sig_pipe[0], POLLIN, 0 },
        };
        if (poll(pfd, 3, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (pfd[2].revents & POLLIN) {
            unsigned char s;
            if (read(sig_pipe[0], &s, 1) == 1 && s != SIGWINCH) {
                reason = "terminated";
                done = 1;
            } else {
                term_size(&rows, &cols);
                msg_put_size(&out, MSG_RESIZE, rows, cols);
                if (send_buf(fd, &out) < 0)
                    break;
            }
        }

        if (!done && (pfd[0].revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(STDIN_FILENO, tmp, 4096);
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                reason = "terminal closed";
                break;
            }
            if (n > 0) {
                msg_put(&out, MSG_INPUT, tmp, (uint32_t)n);
                if (send_buf(fd, &out) < 0)
                    break;
            }
        }

        if (!done && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(fd, tmp, sizeof tmp);
            if (n <= 0) {
                if (n < 0 && errno == EINTR)
                    continue;
                break;
            }
            buf_append(&in, tmp, (size_t)n);

            int type;
            const char *payload;
            uint32_t len;
            long used;
            while (!done && (used = msg_peek(&in, &type, &payload, &len)) > 0) {
                switch (type) {
                case MSG_OUTPUT:
                    if (write_all(STDOUT_FILENO, payload, len) < 0) {
                        reason = "terminal closed";
                        done = 1;
                    }
                    break;
                case MSG_DETACH:
                    snprintf(reason_buf, sizeof reason_buf, "%.*s",
                             (int)(len < 200 ? len : 200), payload);
                    reason = reason_buf;
                    status = 0;
                    done = 1;
                    break;
                case MSG_EXIT:
                    reason = "session ended";
                    status = 0;
                    done = 1;
                    break;
                }
                buf_consume(&in, (size_t)used);
            }
            if (used < 0)
                break;
        }
    }

    write_all(STDOUT_FILENO, LEAVE_SEQ, sizeof LEAVE_SEQ - 1);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    fprintf(stderr, "[duo: %s]\n", reason);
    buf_free(&in);
    buf_free(&out);
    close(fd);
    return status;
}
