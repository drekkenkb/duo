/* Optional session file: which windows a new session opens, what they are
 * called, what runs in them, and how they are arranged.
 *
 *   layout columns          tabs | columns | rows | grid
 *   panes 2                 how many panes are open at the start
 *   focus 1                 which window has focus (1-based)
 *   window logs -- journalctl -f
 *   window shell            a plain shell, no command
 *
 * Read only when a session starts; attaching to a running one ignores it.
 */
#ifndef DUO_CONF_H
#define DUO_CONF_H

#include <stddef.h>

#include "session.h"

#define CONF_CMD_LEN 512

typedef struct {
    char name[NAME_LEN];
    char cmd[CONF_CMD_LEN];
} ConfWindow;

typedef struct {
    int        layout;      /* LAYOUT_*, -1 if the file didn't say */
    int        panes;       /* 0 if the file didn't say */
    int        focus;       /* 1-based, 0 if the file didn't say */
    int        nwindows;
    ConfWindow win[MAX_WINDOWS];
} SessionConf;

/* Finds the file for `session`: ~/.config/duo/<session>.conf, else
 * ~/.config/duo/default.conf. Returns 1 if one exists, 0 if not. */
int conf_find(const char *session, char *path, size_t n);

/* Parses `path`. Returns 0, or -1 with a one-line reason in `err`. */
int conf_load(const char *path, SessionConf *c, char *err, size_t errn);

#endif
