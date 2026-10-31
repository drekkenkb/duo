#include "mouse.h"

#include <stdio.h>

size_t mouse_parse(const char *p, size_t n, MouseEvent *ev)
{
    if (n < 3 || p[0] != 0x1b || p[1] != '[')
        return 0;

    if (p[2] == 'M') {
        if (n < 6)
            return 0;
        int b = (unsigned char)p[3] - 32;
        ev->x = (unsigned char)p[4] - 33;
        ev->y = (unsigned char)p[5] - 33;
        ev->release = (b & 3) == 3 && !(b & (MOUSE_MOTION | MOUSE_WHEEL));
        ev->b = b;
        return (ev->x < 0 || ev->y < 0 || b < 0) ? 0 : 6;
    }

    if (p[2] != '<')
        return 0;
    int v[3] = { 0, 0, 0 }, k = 0;
    for (size_t i = 3; i < n; i++) {
        char c = p[i];
        if (c >= '0' && c <= '9') {
            if (v[k] < 100000)
                v[k] = v[k] * 10 + (c - '0');
        } else if (c == ';' && k < 2) {
            k++;
        } else if ((c == 'M' || c == 'm') && k == 2) {
            if (v[1] < 1 || v[2] < 1)
                return 0;
            ev->b = v[0];
            ev->x = v[1] - 1;
            ev->y = v[2] - 1;
            ev->release = c == 'm';
            return i + 1;
        } else {
            return 0;
        }
    }
    return 0;
}

size_t mouse_encode(const MouseEvent *ev, int mode, int sgr, char *out)
{
    int b = ev->b;
    int motion = b & MOUSE_MOTION;
    int held = (b & 3) != 3;

    if (mode == 0)
        return 0;
    if (motion && (mode == 9 || mode == 1000 || (mode == 1002 && !held)))
        return 0;
    if (mode == 9) {
        if (ev->release)
            return 0;
        b &= 3 | MOUSE_WHEEL;   /* X10 reports carry no modifiers */
    }

    if (sgr)
        return (size_t)snprintf(out, 32, "\033[<%d;%d;%d%c", b, ev->x + 1, ev->y + 1,
                                ev->release ? 'm' : 'M');

    if (ev->release) {
        if (b & MOUSE_WHEEL)
            return 0;           /* wheels have no release in this encoding */
        b = (b & ~3) | 3;
    }
    if (ev->x + 33 > 255 || ev->y + 33 > 255)
        return 0;
    out[0] = 0x1b;
    out[1] = '[';
    out[2] = 'M';
    out[3] = (char)(b + 32);
    out[4] = (char)(ev->x + 33);
    out[5] = (char)(ev->y + 33);
    return 6;
}
