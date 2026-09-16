/* Client <-> server messages over the unix socket.
 *
 * Every message is a 5-byte header (type, then payload length as a
 * little-endian uint32) followed by the payload.
 */
#ifndef DUO_PROTO_H
#define DUO_PROTO_H

#include <stdint.h>

#include "buf.h"

enum {
    /* client -> server */
    MSG_HELLO = 1,  /* payload: uint16 rows, uint16 cols */
    MSG_RESIZE,     /* payload: uint16 rows, uint16 cols */
    MSG_INPUT,      /* payload: raw keyboard bytes */
    MSG_KILL,       /* end the session */

    /* server -> client */
    MSG_OUTPUT = 32, /* payload: bytes for the terminal */
    MSG_DETACH,      /* payload: reason text; client exits */
    MSG_EXIT,        /* session ended; client exits */
};

#define MSG_HDR      5
#define MSG_MAX_LEN  (16u << 20)

void msg_put(Buf *out, int type, const void *payload, uint32_t len);
void msg_put_size(Buf *out, int type, int rows, int cols);

/* If `in` holds a complete message, stores its type, payload pointer and
 * length and returns the total bytes to consume. 0 = need more, -1 = bad. */
long msg_peek(const Buf *in, int *type, const char **payload, uint32_t *len);

int  msg_get_size(const char *payload, uint32_t len, int *rows, int *cols);

#endif
