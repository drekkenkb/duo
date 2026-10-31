/* Mouse reports: parsing what the outer terminal sends, encoding what a
 * window's program asked for. */
#ifndef DUO_MOUSE_H
#define DUO_MOUSE_H

#include <stddef.h>

/* Button byte bits, as in xterm's reports. */
#define MOUSE_MOTION 32
#define MOUSE_WHEEL  64

typedef struct {
    int b;          /* button byte: low 2 bits button, +4/8/16 modifiers, +32 motion, +64 wheel */
    int x, y;       /* 0-based */
    int release;
} MouseEvent;

/* If p starts with a mouse report (SGR "ESC [ < b;x;y M/m" or X10
 * "ESC [ M bxy"), fills *ev and returns its length; otherwise 0. */
size_t mouse_parse(const char *p, size_t n, MouseEvent *ev);

/* Encodes ev for a program that enabled tracking `mode` (9, 1000, 1002 or
 * 1003) with SGR encoding if `sgr`. Returns the length written to out (at
 * least 32 bytes), or 0 if that program shouldn't get this event. */
size_t mouse_encode(const MouseEvent *ev, int mode, int sgr, char *out);

#endif
