/* duo — a small terminal multiplexer: windows and splits in one ssh
 * session, surviving disconnects.
 *
 *   duo            attach to the session, starting it if needed
 *   duo -s NAME    use a named session instead of "default"
 *   duo -l         list running sessions
 *   duo -k         end the session and all its windows
 */
#include "duo.h"
#include "proto.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int sock_dir(char *out, size_t n)
{
    snprintf(out, n, "/tmp/duo-%u", (unsigned)getuid());
    if (mkdir(out, 0700) < 0 && errno != EEXIST) {
        fprintf(stderr, "duo: cannot create %s: %s\n", out, strerror(errno));
        return -1;
    }
    /* Refuse a directory someone else could have planted or can read. */
    struct stat st;
    if (lstat(out, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077)) {
        fprintf(stderr, "duo: %s is not a private directory owned by you\n", out);
        return -1;
    }
    return 0;
}

int sock_path(const char *name, char *out, size_t n)
{
    char dir[64];
    if (sock_dir(dir, sizeof dir) < 0)
        return -1;
    if (strchr(name, '/') || !*name || strlen(dir) + strlen(name) + 2 > 100) {
        fprintf(stderr, "duo: bad session name '%s'\n", name);
        return -1;
    }
    snprintf(out, n, "%s/%s", dir, name);
    return 0;
}

int sock_connect(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static void usage(FILE *f)
{
    fprintf(f,
        "usage: duo [-s NAME] [-l | -k]\n"
        "\n"
        "  (no option)  attach to the session, starting it if it isn't running\n"
        "  -s NAME      use session NAME instead of \"default\"\n"
        "  -l           list running sessions\n"
        "  -k           end the session, closing all its windows\n"
        "  -h           this help\n"
        "\n"
        "Inside: Ctrl-a c new window, Ctrl-a S split, Ctrl-a ? help,\n"
        "Ctrl-a d detaches.\n");
}

static int list_sessions(void)
{
    char dir[64], path[512];
    if (sock_dir(dir, sizeof dir) < 0)
        return 1;
    DIR *d = opendir(dir);
    if (!d)
        return 1;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        int fd = sock_connect(path);
        if (fd >= 0) {
            printf("%s\n", e->d_name);
            close(fd);
            found = 1;
        } else if (errno == ECONNREFUSED) {
            unlink(path); /* left behind by a crashed server */
        }
    }
    closedir(d);
    if (!found)
        printf("no sessions\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *name = "default";
    int kill_it = 0, list = 0, opt;

    while ((opt = getopt(argc, argv, "s:lkh")) != -1) {
        switch (opt) {
        case 's': name = optarg; break;
        case 'l': list = 1; break;
        case 'k': kill_it = 1; break;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 2;
        }
    }
    if (optind != argc) {
        usage(stderr);
        return 2;
    }
    if (list)
        return list_sessions();

    char path[128];
    if (sock_path(name, path, sizeof path) < 0)
        return 1;

    if (kill_it) {
        int fd = sock_connect(path);
        if (fd < 0) {
            fprintf(stderr, "duo: no session '%s'\n", name);
            return 1;
        }
        Buf b = { 0 };
        msg_put(&b, MSG_KILL, NULL, 0);
        if (write(fd, b.data, b.len) < 0)
            perror("duo: write");
        buf_free(&b);
        close(fd);
        return 0;
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "duo: needs a terminal (with ssh, use: ssh -t host duo)\n");
        return 1;
    }
    const char *inside = getenv("DUO_SESSION");
    if (inside && strcmp(inside, path) == 0) {
        fprintf(stderr, "duo: already inside session '%s'\n", name);
        return 1;
    }

    int fd = sock_connect(path);
    if (fd < 0) {
        if (errno != ENOENT && errno != ECONNREFUSED) {
            fprintf(stderr, "duo: connect %s: %s\n", path, strerror(errno));
            return 1;
        }
        struct winsize ws = { 0 };
        ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
        if (server_start(path, ws.ws_row ? ws.ws_row : 24, ws.ws_col ? ws.ws_col : 80) < 0)
            return 1;
        fd = sock_connect(path);
        if (fd < 0) {
            fprintf(stderr, "duo: session did not start: %s\n", strerror(errno));
            return 1;
        }
    }
    return client_attach(fd);
}
