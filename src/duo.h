#ifndef DUO_H
#define DUO_H

#include <stddef.h>

/* Socket for session `name`, under /tmp/duo-<uid>/. Creates and checks the
 * directory. Returns 0 on success, -1 with a message printed. */
int sock_dir(char *out, size_t n);
int sock_path(const char *name, char *out, size_t n);

/* Connects to a session socket; -1 with errno set if nobody is listening. */
int sock_connect(const char *path);

/* Starts a detached server for `path` with shells sized rows x cols.
 * Returns once the socket is listening, so a connect() right after works. */
int server_start(const char *path, int rows, int cols, const char *conf_path);

/* Runs the attached client until detach. Returns the process exit status. */
int client_attach(int fd);

#endif
