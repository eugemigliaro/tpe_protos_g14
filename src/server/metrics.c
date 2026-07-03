/**
 * metrics.c - contadores de métricas en RAM (RF6).
 *
 * No hay sincronización: todo el código que actualiza estos contadores
 * corre en el único thread del event loop.
 */
#include "metrics.h"

static uint64_t hist_connections = 0;
static uint32_t curr_connections = 0;
static uint64_t bytes_sent       = 0;
static uint64_t bytes_recv       = 0;

void
metrics_init(void)
{
    hist_connections = 0;
    curr_connections = 0;
    bytes_sent       = 0;
    bytes_recv       = 0;
}

void
metrics_conn_open(void)
{
    hist_connections++;
    curr_connections++;
}

void
metrics_conn_close(void)
{
    if (curr_connections > 0) {
        curr_connections--;
    }
}

void
metrics_add_bytes_sent(size_t n)
{
    bytes_sent += n;
}

void
metrics_add_bytes_recv(size_t n)
{
    bytes_recv += n;
}

struct metrics_snapshot
metrics_get(void)
{
    return (struct metrics_snapshot){
        .hist_connections = hist_connections,
        .curr_connections = curr_connections,
        .bytes_sent       = bytes_sent,
        .bytes_recv       = bytes_recv,
    };
}
