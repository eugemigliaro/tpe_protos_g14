#ifndef HELLO_H_g14
#define HELLO_H_g14

#include <stdbool.h>
#include <stdint.h>

#include "selector.h"

/* Métodos de autenticación (RFC1928). */
#define METHOD_NO_AUTH       0x00
#define METHOD_USERPASS      0x02
#define METHOD_NO_ACCEPTABLE 0xFF

/**
 * Parser incremental de la negociación de métodos (RFC1928):
 *   VER(1=0x05) NMETHODS(1) METHODS(NMETHODS)
 *
 * Se alimenta byte a byte para tolerar lecturas parciales.
 */
struct hello_parser {
    enum hello_state {
        HELLO_P_VERSION,
        HELLO_P_NMETHODS,
        HELLO_P_METHODS,
        HELLO_P_DONE,
        HELLO_P_ERROR,
    } state;
    uint8_t remaining;     /* métodos que faltan leer */
    bool    has_userpass;  /* el cliente ofrece user/pass */
    bool    has_noauth;    /* el cliente ofrece "sin auth" */
    uint8_t selected;      /* método elegido (se recuerda para el write) */
};

/* Callbacks del estado HELLO para la máquina de estados. */
void     hello_on_arrival(const unsigned state, struct selector_key *key);
unsigned hello_on_read_ready(struct selector_key *key);
unsigned hello_on_write_ready(struct selector_key *key);

#endif
