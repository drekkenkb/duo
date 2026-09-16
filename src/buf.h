/* Growable byte buffer, used for socket and pty queues. */
#ifndef DUO_BUF_H
#define DUO_BUF_H

#include <stddef.h>

typedef struct {
    char   *data;
    size_t  len, cap;
} Buf;

void buf_append(Buf *b, const void *p, size_t n);
void buf_appendf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_consume(Buf *b, size_t n);
void buf_free(Buf *b);

/* Writes as much as the (non-blocking) fd accepts. Returns -1 on a hard error. */
int  buf_flush(Buf *b, int fd);

#endif
