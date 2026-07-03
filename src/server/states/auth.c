/**
 * auth.c - estado AUTH: autenticación usuario/contraseña (RFC1929).
 *
 *   cliente -> VER(0x01) ULEN UNAME PLEN PASSWD
 *   servidor -> VER(0x01) STATUS   (0x00 ok, !=0 fallo)
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "socks5.h"
#include "states/states_common.h"

static void
auth_feed(struct auth_parser *p, uint8_t b)
{
    switch (p->state) {
        case AUTH_P_VERSION:
            p->state = (b == AUTH_VERSION) ? AUTH_P_ULEN : AUTH_P_ERROR;
            break;
        case AUTH_P_ULEN:
            p->ulen = b;
            p->idx  = 0;
            p->state = (b == 0) ? AUTH_P_PLEN : AUTH_P_UNAME;
            break;
        case AUTH_P_UNAME:
            p->uname[p->idx++] = (char)b;
            if (p->idx == p->ulen) {
                p->uname[p->idx] = '\0';
                p->state = AUTH_P_PLEN;
            }
            break;
        case AUTH_P_PLEN:
            p->plen = b;
            p->idx  = 0;
            if (b == 0) {
                p->passwd[0] = '\0';
                p->state = AUTH_P_DONE;
            } else {
                p->state = AUTH_P_PASSWD;
            }
            break;
        case AUTH_P_PASSWD:
            p->passwd[p->idx++] = (char)b;
            if (p->idx == p->plen) {
                p->passwd[p->idx] = '\0';
                p->state = AUTH_P_DONE;
            }
            break;
        default:
            break;
    }
}

void
auth_on_arrival(const unsigned state, struct selector_key *key)
{
    (void)state;
    struct auth_parser *p = &ATTACHMENT(key)->parser.auth;
    p->state = AUTH_P_VERSION;
    p->idx   = 0;
    p->ok    = false;
}

unsigned
auth_on_read_ready(struct selector_key *key)
{
    struct socks5 *s = ATTACHMENT(key);
    struct auth_parser *p = &s->parser.auth;

    const ssize_t n = socks_recv(key, &s->read_buffer);
    if (n == 0) {
        return ERROR;
    }
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? AUTH_READ : ERROR;
    }
    while (buffer_can_read(&s->read_buffer) &&
           p->state != AUTH_P_DONE && p->state != AUTH_P_ERROR) {
        auth_feed(p, buffer_read(&s->read_buffer));
    }
    if (p->state == AUTH_P_ERROR) {
        return ERROR;
    }
    if (p->state != AUTH_P_DONE) {
        return AUTH_READ;
    }

    p->ok = socks5_validate_user(p->uname, p->passwd);
    if (p->ok) {
        snprintf(s->username, sizeof(s->username), "%s", p->uname);
    }
    buffer_write(&s->write_buffer, AUTH_VERSION);
    buffer_write(&s->write_buffer, p->ok ? AUTH_STATUS_OK : AUTH_STATUS_FAIL);
    selector_set_interest_key(key, OP_WRITE);
    return AUTH_WRITE;
}

unsigned
auth_on_write_ready(struct selector_key *key)
{
    struct socks5 *s = ATTACHMENT(key);

    const int st = socks_send(key, &s->write_buffer);
    if (st == -1) {
        return ERROR;
    }
    if (st == 0) {
        return AUTH_WRITE;
    }
    if (!s->parser.auth.ok) {
        return DONE; /* credenciales inválidas: cerrar */
    }
    selector_set_interest_key(key, OP_READ);
    return REQUEST_READ;
}
