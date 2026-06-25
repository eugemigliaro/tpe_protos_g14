#include <errno.h>
#include <sys/socket.h>

#include "states/states_common.h"

ssize_t
socks_recv(struct selector_key *key, buffer *b)
{
    size_t space;
    uint8_t *ptr = buffer_write_ptr(b, &space);
    if (space == 0) {
        return -1; /* sin lugar: el llamador no debería pedir leer */
    }
    const ssize_t n = recv(key->fd, ptr, space, 0);
    if (n > 0) {
        buffer_write_adv(b, n);
    }
    return n;
}

int
socks_send(struct selector_key *key, buffer *b)
{
    size_t count;
    uint8_t *ptr = buffer_read_ptr(b, &count);
    if (count == 0) {
        return 1; /* nada que enviar */
    }
    const ssize_t n = send(key->fd, ptr, count, MSG_NOSIGNAL);
    if (n == -1) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
    buffer_read_adv(b, n);
    return buffer_can_read(b) ? 0 : 1;
}
