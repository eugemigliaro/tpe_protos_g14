#ifndef REQUEST_H_g14
#define REQUEST_H_g14

#include <stdint.h>

#include "selector.h"

#define SOCKS_CMD_CONNECT  0x01
#define SOCKS_ATYP_IPV4    0x01
#define SOCKS_ATYP_FQDN    0x03
#define SOCKS_ATYP_IPV6    0x04
#define SOCKS_FQDN_MAX     255

/**
 * Parser incremental del request SOCKS5 (RFC1928):
 *   VER(1=0x05) CMD(1) RSV(1=0x00) ATYP(1) DST.ADDR(var) DST.PORT(2)
 */
struct request_parser {
    enum request_state {
        REQ_P_VERSION,
        REQ_P_CMD,
        REQ_P_RSV,
        REQ_P_ATYP,
        REQ_P_DADDR_LEN,  /* solo FQDN: byte de longitud */
        REQ_P_DADDR,
        REQ_P_DPORT,
        REQ_P_DONE,
        REQ_P_ERROR,
    } state;
    uint8_t cmd;
    uint8_t atyp;
    uint8_t addr_len;            /* bytes de DST.ADDR a leer */
    uint8_t idx;
    uint8_t addr[SOCKS_FQDN_MAX + 1]; /* IPv4(4) / IPv6(16) / FQDN(+NUL) */
    uint8_t port[2];             /* network byte order */
};

/* REQUEST: parseo del pedido. */
void     request_on_arrival(const unsigned state, struct selector_key *key);
unsigned request_on_read_ready(struct selector_key *key);

/* RESOLVE: resolución de nombres en thread aparte. */
void     request_resolv_on_arrival(const unsigned state, struct selector_key *key);
unsigned request_resolv_on_block_ready(struct selector_key *key);

/** Reintenta notificaciones DNS que no pudieron encolarse en el selector. */
void request_resolv_retry_notifications(void);

/** Cantidad de workers DNS todavía pendientes de consumo/join. */
unsigned request_resolv_pending_jobs(void);

/** Espera y descarta todos los workers DNS pendientes antes del shutdown. */
void request_resolv_wait_all(void);

/* CONNECTING: confirmación del connect no bloqueante al origin. */
unsigned request_connecting_on_write_ready(struct selector_key *key);

/* REQUEST_WRITE: envío del reply al cliente. */
unsigned request_write_on_write_ready(struct selector_key *key);

#endif
