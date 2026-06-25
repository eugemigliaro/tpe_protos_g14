#ifndef STATES_COMMON_H_g14
#define STATES_COMMON_H_g14

#include <stdint.h>

#include "selector.h"
#include "buffer.h"

/* macOS no define MSG_NOSIGNAL; ignoramos SIGPIPE en main como respaldo. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/**
 * Lee del socket de `key` hacia el buffer `b` (escritura).
 * Retorna lo mismo que recv(2): >0 bytes leídos, 0 EOF, -1 error (ver errno).
 */
ssize_t socks_recv(struct selector_key *key, buffer *b);

/**
 * Envía el contenido pendiente del buffer `b` al socket de `key`.
 * Retorna: 1 = se envió todo, 0 = quedó pendiente (parcial/EAGAIN), -1 = error.
 */
int socks_send(struct selector_key *key, buffer *b);

#endif
