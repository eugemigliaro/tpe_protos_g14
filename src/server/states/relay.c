/**
 * relay.c - estado COPY: relay bidireccional cliente <-> origin.
 *
 * read_buffer  transporta cliente -> origin.
 * write_buffer transporta origin  -> cliente.
 *
 * Cada fd lee hacia el buffer que "se aleja" de él y escribe desde el buffer
 * que "viene hacia" él. Se manejan lecturas/escrituras parciales y el cierre
 * de medio canal: al recibir EOF de un lado se hace SHUT_WR del otro cuando se
 * vacía el buffer en tránsito. La conexión termina cuando ambos sentidos
 * quedaron cerrados y sin datos pendientes.
 */
#include <errno.h>
#include <sys/socket.h>

#include "access_log.h"
#include "metrics.h"
#include "socks5.h"
#include "states/states_common.h"

/* Recalcula los intereses de ambos fds según el estado de los buffers. */
static void
relay_compute_interests(fd_selector sel, struct socks5 *s)
{
    fd_interest client = OP_NOOP;
    fd_interest origin = OP_NOOP;

    if (!s->client_closed && buffer_can_write(&s->read_buffer)) {
        client |= OP_READ;
    }
    if (buffer_can_read(&s->write_buffer)) {
        client |= OP_WRITE;
    }
    if (!s->origin_closed && buffer_can_write(&s->write_buffer)) {
        origin |= OP_READ;
    }
    if (buffer_can_read(&s->read_buffer)) {
        origin |= OP_WRITE;
    }
    selector_set_interest(sel, s->client_fd, client);
    selector_set_interest(sel, s->origin_fd, origin);
}

/* Ambos sentidos cerrados y sin datos en tránsito. */
static bool
relay_finished(struct socks5 *s)
{
    const bool c2o = s->client_closed && !buffer_can_read(&s->read_buffer);
    const bool o2c = s->origin_closed && !buffer_can_read(&s->write_buffer);
    return c2o && o2c;
}

void
relay_on_arrival(const unsigned state, struct selector_key *key)
{
    (void)state;
    struct socks5 *s = ATTACHMENT(key);
    /* RF8: registrar acceso al entrar en COPY (conexión establecida con éxito). */
    access_log_entry(s->username[0] ? s->username : NULL,
                     s->origin_str[0] ? s->origin_str : NULL, "OK");
    selector_set_interest(key->s, s->client_fd, OP_READ);
    selector_set_interest(key->s, s->origin_fd, OP_READ);
}

unsigned
relay_on_read_ready(struct selector_key *key)
{
    struct socks5 *s = ATTACHMENT(key);
    const bool from_client = (key->fd == s->client_fd);
    buffer *b = from_client ? &s->read_buffer : &s->write_buffer;
    const int peer = from_client ? s->origin_fd : s->client_fd;

    /* Acumular todo lo disponible en el socket (loop hasta EAGAIN). */
    bool got_eof = false;
    for (;;) {
        size_t space;
        uint8_t *ptr = buffer_write_ptr(b, &space);
        if (space == 0) {
            break; /* buffer lleno */
        }
        const ssize_t n = recv(key->fd, ptr, space, 0);
        if (n > 0) {
            buffer_write_adv(b, n);
            /* RF6: contabilizar bytes según dirección del flujo. */
            if (from_client) {
                metrics_add_bytes_recv((size_t)n);
            } else {
                metrics_add_bytes_sent((size_t)n);
            }
        } else if (n == 0) {
            got_eof = true;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* no hay más datos por ahora */
            }
            return ERROR;
        }
    }

    /* Intentar enviar inmediatamente al peer sin volver al selector. */
    if (buffer_can_read(b)) {
        size_t count;
        uint8_t *ptr = buffer_read_ptr(b, &count);
        const ssize_t w = send(peer, ptr, count, MSG_NOSIGNAL);
        if (w > 0) {
            buffer_read_adv(b, w);
        } else if (w == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return ERROR;
        }
    }

    if (got_eof) {
        if (from_client) {
            s->client_closed = true;
        } else {
            s->origin_closed = true;
        }
        shutdown(key->fd, SHUT_RD);
        if (!buffer_can_read(b)) {
            shutdown(peer, SHUT_WR);
        }
    }

    if (relay_finished(s)) {
        return DONE;
    }
    relay_compute_interests(key->s, s);
    return COPY;
}

unsigned
relay_on_write_ready(struct selector_key *key)
{
    struct socks5 *s = ATTACHMENT(key);
    const bool to_client = (key->fd == s->client_fd);
    buffer *b = to_client ? &s->write_buffer : &s->read_buffer;
    const bool source_closed = to_client ? s->origin_closed : s->client_closed;

    size_t count;
    uint8_t *ptr = buffer_read_ptr(b, &count);
    const ssize_t n = send(key->fd, ptr, count, MSG_NOSIGNAL);
    if (n > 0) {
        buffer_read_adv(b, n);
        if (!buffer_can_read(b) && source_closed) {
            shutdown(key->fd, SHUT_WR); /* drenado y origen cerrado: medio cierre */
        }
    } else if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return ERROR;
    }

    if (relay_finished(s)) {
        return DONE;
    }
    relay_compute_interests(key->s, s);
    return COPY;
}
