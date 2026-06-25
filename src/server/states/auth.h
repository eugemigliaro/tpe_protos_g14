#ifndef AUTH_H_g14
#define AUTH_H_g14

#include <stdint.h>

#include "selector.h"

#define AUTH_VERSION    0x01  /* versión del subprotocolo user/pass (RFC1929) */
#define AUTH_STATUS_OK  0x00
#define AUTH_STATUS_FAIL 0x01
#define AUTH_MAX_FIELD  255   /* ULEN/PLEN son de 1 byte */

/**
 * Parser incremental de autenticación user/pass (RFC1929):
 *   VER(1=0x01) ULEN(1) UNAME(ULEN) PLEN(1) PASSWD(PLEN)
 */
struct auth_parser {
    enum auth_state {
        AUTH_P_VERSION,
        AUTH_P_ULEN,
        AUTH_P_UNAME,
        AUTH_P_PLEN,
        AUTH_P_PASSWD,
        AUTH_P_DONE,
        AUTH_P_ERROR,
    } state;
    uint8_t ulen;
    uint8_t plen;
    uint8_t idx;
    bool    ok;            /* resultado de la validación (recordado para el write) */
    char    uname[AUTH_MAX_FIELD + 1];
    char    passwd[AUTH_MAX_FIELD + 1];
};

void     auth_on_arrival(const unsigned state, struct selector_key *key);
unsigned auth_on_read_ready(struct selector_key *key);
unsigned auth_on_write_ready(struct selector_key *key);

#endif
