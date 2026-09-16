#include "conf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *layout_words[] = { "tabs", "columns", "rows", "grid" };

int conf_find(const char *session, char *path, size_t n)
{
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char dir[256];
    if (xdg && *xdg)
        snprintf(dir, sizeof dir, "%s/duo", xdg);
    else if (home && *home)
        snprintf(dir, sizeof dir, "%s/.config/duo", home);
    else
        return 0;

    snprintf(path, n, "%s/%s.conf", dir, session);
    if (access(path, R_OK) == 0)
        return 1;
    snprintf(path, n, "%s/default.conf", dir);
    return access(path, R_OK) == 0;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
        *--end = 0;
    return s;
}

/* Splits off a leading word, leaving `line` at what follows. */
static char *word(char **line)
{
    char *s = trim(*line);
    char *start = s;
    while (*s && *s != ' ' && *s != '\t')
        s++;
    if (*s)
        *s++ = 0;
    *line = s;
    return start;
}

int conf_load(const char *path, SessionConf *c, char *err, size_t errn)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errn, "cannot read %s", path);
        return -1;
    }
    memset(c, 0, sizeof *c);
    c->layout = -1;

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        /* A '#' starts a comment, unless it is inside a window's command. */
        char *sep = strstr(line, "--");
        char *hash = strchr(line, '#');
        if (hash && (!sep || hash < sep))
            *hash = 0;

        char *rest = line;
        char *key = word(&rest);
        if (!*key)
            continue;

        if (strcmp(key, "layout") == 0) {
            char *v = word(&rest);
            c->layout = -1;
            for (int i = 0; i < LAYOUT_COUNT; i++)
                if (strcmp(v, layout_words[i]) == 0)
                    c->layout = i;
            if (c->layout < 0) {
                snprintf(err, errn, "line %d: layout must be tabs, columns, rows or grid",
                         lineno);
                goto fail;
            }
        } else if (strcmp(key, "panes") == 0 || strcmp(key, "focus") == 0) {
            char *v = word(&rest);
            int n = atoi(v);
            int max = (key[0] == 'p') ? MAX_PANES : MAX_WINDOWS;
            if (n < 1 || n > max) {
                snprintf(err, errn, "line %d: %s must be between 1 and %d", lineno, key, max);
                goto fail;
            }
            if (key[0] == 'p')
                c->panes = n;
            else
                c->focus = n;
        } else if (strcmp(key, "window") == 0) {
            if (c->nwindows >= MAX_WINDOWS) {
                snprintf(err, errn, "line %d: more than %d windows", lineno, MAX_WINDOWS);
                goto fail;
            }
            ConfWindow *w = &c->win[c->nwindows++];
            char *tail = trim(rest);
            if (strncmp(tail, "--", 2) != 0 && *tail) {
                char *name = word(&tail);
                snprintf(w->name, sizeof w->name, "%s", name);
                tail = trim(tail);
            }
            if (strncmp(tail, "--", 2) == 0) {
                tail = trim(tail + 2);
                if (!*tail) {
                    snprintf(err, errn, "line %d: -- with no command after it", lineno);
                    goto fail;
                }
                snprintf(w->cmd, sizeof w->cmd, "%s", tail);
            }
        } else {
            snprintf(err, errn, "line %d: don't know '%s' (layout, panes, focus, window)",
                     lineno, key);
            goto fail;
        }
    }
    fclose(f);

    if (c->nwindows == 0) {
        snprintf(err, errn, "%s has no window lines", path);
        return -1;
    }
    if (c->focus > c->nwindows) {
        snprintf(err, errn, "focus %d, but the file only opens %d window%s",
                 c->focus, c->nwindows, c->nwindows == 1 ? "" : "s");
        return -1;
    }
    if (c->panes > c->nwindows) {
        snprintf(err, errn, "panes %d, but the file only opens %d window%s",
                 c->panes, c->nwindows, c->nwindows == 1 ? "" : "s");
        return -1;
    }
    return 0;

fail:
    fclose(f);
    return -1;
}
