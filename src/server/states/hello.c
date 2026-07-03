/**
 * hello.c - estado HELLO: negociación de método de autenticación (RFC1928).
 *
 *   cliente -> VER(0x05) NMETHODS METHODS...
 *   servidor -> VER(0x05) METHOD
 *
 * Política: si hay usuarios configurados se exige user/pass (RFC1929); si no
 * hay ninguno, se acepta "sin autenticación".
 */
#include <errno.h>

#include "socks5.h"
#include "states/states_common.h"

static void
hello_feed(struct hello_parser *p, uint8_t b)
{
    switch (p->state) {
        case HELLO_P_VERSION:
            p->state = (b == 0x05) ? HELLO_P_NMETHODS : HELLO_P_ERROR;
            break;
        case HELLO_P_NMETHODS:
            p->remaining = b;
            p->state = (b == 0) ? HELLO_P_DONE : HELLO_P_METHODS;
            break;
        case HELLO_P_METHODS:
            if (b == METHOD_NO_AUTH) {
                p->has_noauth = true;
            } else if (b == METHOD_USERPASS) {
                p->has_userpass = true;
            }
            if (--p->remaining == 0) {
                p->state = HELLO_P_DONE;
            }
            break;
        default:
            break;
    }
}

void
hello_on_arrival(const unsigned state, struct selector_key *key)
{
    (void)state;
    struct hello_parser *p = &ATTACHMENT(key)->parser.hello;
    p->state        = HELLO_P_VERSION;
    p->remaining    = 0;
    p->has_userpass = false;
    p->has_noauth   = false;
    p->selected     = METHOD_NO_ACCEPTABLE;
}

unsigned
hello_on_read_ready(struct selector_key *key)
{
    struct socks5 *s = ATTACHMENT(key);
    struct hello_parser *p = &s->parser.hello;

    const ssize_t n = socks_recv(key, &s->read_buffer);
    if (n == 0) {
        return ERROR;
    }
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? HELLO_READ : ERROR;
    }
    while (buffer_can_read(&s->read_buffer) &&
           p->state != HELLO_P_DONE && p->state != HELLO_P_ERROR) {
        hello_feed(p, buffer_read(&s->read_buffer));
    }
    if (p->state == HELLO_P_ERROR) {
        return ERROR;
    }
    if (p->state != HELLO_P_DONE) {
        return HELLO_READ;
    }

    if (p->has_userpass) {
        p->selected = METHOD_USERPASS;
    } else if (p->has_noauth && !socks5_has_users()) {
        p->selected = METHOD_NO_AUTH;
    } else {
        p->selected = METHOD_NO_ACCEPTABLE;
    }

    buffer_write(&s->write_buffer, 0x05);
    buffer_write(&s->write_buffer, p->selected);
    selector_set_interest_key(key, OP_WRITE);
    return HELLO_WRITE;
}

unsigned
hello_on_write_ready(struct selector_key *key)
{
    struct socks5 *s = ATTACHMENT(key);

    const int st = socks_send(key, &s->write_buffer);
    if (st == -1) {
        return ERROR;
    }
    if (st == 0) {
        return HELLO_WRITE;
    }
    switch (s->parser.hello.selected) {
        case METHOD_USERPASS:
            selector_set_interest_key(key, OP_READ);
            return AUTH_READ;
        case METHOD_NO_AUTH:
            selector_set_interest_key(key, OP_READ);
            return REQUEST_READ;
        default:
            return DONE; /* 0xFF: no hay método aceptable */
    }
}
