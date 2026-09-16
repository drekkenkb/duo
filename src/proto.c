#include "proto.h"

void msg_put(Buf *out, int type, const void *payload, uint32_t len)
{
    unsigned char h[MSG_HDR] = {
        (unsigned char)type,
        len & 0xff, (len >> 8) & 0xff, (len >> 16) & 0xff, (len >> 24) & 0xff,
    };
    buf_append(out, h, sizeof h);
    if (len)
        buf_append(out, payload, len);
}

void msg_put_size(Buf *out, int type, int rows, int cols)
{
    unsigned char p[4] = {
        rows & 0xff, (rows >> 8) & 0xff, cols & 0xff, (cols >> 8) & 0xff,
    };
    msg_put(out, type, p, sizeof p);
}

long msg_peek(const Buf *in, int *type, const char **payload, uint32_t *len)
{
    if (in->len < MSG_HDR)
        return 0;
    const unsigned char *h = (const unsigned char *)in->data;
    uint32_t n = (uint32_t)h[1] | (uint32_t)h[2] << 8 | (uint32_t)h[3] << 16 |
                 (uint32_t)h[4] << 24;
    if (n > MSG_MAX_LEN)
        return -1;
    if (in->len < MSG_HDR + (size_t)n)
        return 0;
    *type = h[0];
    *payload = in->data + MSG_HDR;
    *len = n;
    return (long)(MSG_HDR + n);
}

int msg_get_size(const char *payload, uint32_t len, int *rows, int *cols)
{
    if (len != 4)
        return -1;
    const unsigned char *p = (const unsigned char *)payload;
    *rows = p[0] | p[1] << 8;
    *cols = p[2] | p[3] << 8;
    return (*rows > 0 && *cols > 0) ? 0 : -1;
}
