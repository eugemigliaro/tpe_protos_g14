#ifndef METRICS_H_g14
#define METRICS_H_g14

#include <stddef.h>
#include <stdint.h>

/**
 * metrics.h - contadores de métricas del proxy (RF6).
 *
 * Todas las variables son globales estáticas en metrics.c. No hay
 * sincronización porque todo el código que las toca corre en el único
 * thread del event loop (excepción: getaddrinfo no toca métricas).
 */

struct metrics_snapshot {
    uint64_t hist_connections;  /* total de conexiones SOCKS5 desde el inicio */
    uint32_t curr_connections;  /* conexiones SOCKS5 activas en este momento   */
    uint64_t bytes_sent;        /* bytes enviados hacia los clientes SOCKS5     */
    uint64_t bytes_recv;        /* bytes recibidos de los clientes SOCKS5       */
};

void                    metrics_init(void);
void                    metrics_conn_open(void);
void                    metrics_conn_close(void);
void                    metrics_add_bytes_sent(size_t n);
void                    metrics_add_bytes_recv(size_t n);
struct metrics_snapshot metrics_get(void);

#endif
