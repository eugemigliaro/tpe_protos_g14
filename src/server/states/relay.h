#ifndef RELAY_H_g14
#define RELAY_H_g14

#include "selector.h"

/**
 * COPY: relay bidireccional cliente <-> origin con lecturas/escrituras
 * parciales y cierres de medio canal (SHUT_WR al recibir EOF de un lado).
 */
void     relay_on_arrival(const unsigned state, struct selector_key *key);
unsigned relay_on_read_ready(struct selector_key *key);
unsigned relay_on_write_ready(struct selector_key *key);

#endif
