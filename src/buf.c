#include "buf.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void buf_append(Buf *b, const void *p, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n)
            cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d)
            abort();
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void buf_appendf(Buf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        buf_append(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

void buf_consume(Buf *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

void buf_free(Buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

int buf_flush(Buf *b, int fd)
{
    while (b->len) {
        ssize_t n = write(fd, b->data, b->len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1;
        }
        buf_consume(b, (size_t)n);
    }
    return 0;
}
