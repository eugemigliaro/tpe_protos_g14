/**
 * socks5.c - ciclo de vida de una conexión SOCKS5 y cableado con el selector.
 *
 * Cada cliente tiene una `struct socks5` con su máquina de estados (stm.c) y
 * sus buffers. El selector (selector.c) despacha los eventos de I/O hacia la
 * stm; los estados concretos viven en states/.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "socks5.h"

#define POOL_MAX 64  /* conexiones cacheadas para evitar malloc/free repetidos */

/* Configuración global del servidor (solo lectura). */
static const struct socks5args *config = NULL;

/* Free-list de structs socks5 reutilizables. */
static struct socks5 *pool      = NULL;
static unsigned       pool_size = 0;

/* --- declaración adelantada de la tabla de estados --- */
static const struct state_definition *socks5_describe_states(void);

void
socks5_init(const struct socks5args *args)
{
    config = args;
}

const struct socks5args *
socks5_config(void)
{
    return config;
}

/* --- pool de conexiones --- */

static struct socks5 *
socks5_new(int client_fd)
{
    struct socks5 *s;
    if (pool == NULL) {
        s = malloc(sizeof(*s));
        if (s == NULL) {
            return NULL;
        }
    } else {
        s = pool;
        pool = pool->next;
        pool_size--;
    }

    memset(s, 0, sizeof(*s));
    s->client_fd       = client_fd;
    s->origin_fd       = -1;
    s->client_addr_len = sizeof(s->client_addr);
    s->references      = 1;

    s->stm.initial   = HELLO_READ;
    s->stm.max_state = ERROR;
    s->stm.states    = socks5_describe_states();
    stm_init(&s->stm);

    buffer_init(&s->read_buffer,  SOCKS5_BUFFER_SIZE, s->raw_read);
    buffer_init(&s->write_buffer, SOCKS5_BUFFER_SIZE, s->raw_write);
    return s;
}

static void
socks5_destroy(struct socks5 *s)
{
    if (s == NULL) {
        return;
    }
    if (s->references > 1) {
        s->references--;
        return;
    }
    /* última referencia: liberar recursos y volver al pool */
    if (s->origin_resolution != NULL) {
        freeaddrinfo(s->origin_resolution);
        s->origin_resolution = NULL;
    }
    if (pool_size < POOL_MAX) {
        s->next = pool;
        pool    = s;
        pool_size++;
    } else {
        free(s);
    }
}

void
socks5_pool_destroy(void)
{
    while (pool != NULL) {
        struct socks5 *next = pool->next;
        free(pool);
        pool = next;
    }
    pool_size = 0;
}

/* --- handlers del selector (delegan en la stm) --- */

static void
socks5_done(struct selector_key *key)
{
    const int fds[] = { ATTACHMENT(key)->client_fd, ATTACHMENT(key)->origin_fd };
    for (unsigned i = 0; i < 2; i++) {
        if (fds[i] != -1) {
            selector_unregister_fd(key->s, fds[i]);
            close(fds[i]);
        }
    }
}

static void
socks5_read(struct selector_key *key)
{
    struct state_machine *stm = &ATTACHMENT(key)->stm;
    const enum socks_state st = stm_handler_read(stm, key);
    if (st == ERROR || st == DONE) {
        socks5_done(key);
    }
}

static void
socks5_write(struct selector_key *key)
{
    struct state_machine *stm = &ATTACHMENT(key)->stm;
    const enum socks_state st = stm_handler_write(stm, key);
    if (st == ERROR || st == DONE) {
        socks5_done(key);
    }
}

static void
socks5_block(struct selector_key *key)
{
    struct state_machine *stm = &ATTACHMENT(key)->stm;
    const enum socks_state st = stm_handler_block(stm, key);
    if (st == ERROR || st == DONE) {
        socks5_done(key);
    }
}

static void
socks5_close(struct selector_key *key)
{
    socks5_destroy(ATTACHMENT(key));
}

/* Handler compartido por el fd del cliente y el del origin. */
static const struct fd_handler socks5_handler = {
    .handle_read  = socks5_read,
    .handle_write = socks5_write,
    .handle_block = socks5_block,
    .handle_close = socks5_close,
};

selector_status
socks5_register_origin(struct selector_key *key, struct socks5 *s)
{
    const selector_status st = selector_register(key->s, s->origin_fd,
                                                 &socks5_handler, OP_WRITE, s);
    if (st == SELECTOR_SUCCESS) {
        s->references++;
    }
    return st;
}

/* --- accept del socket pasivo --- */

void
socks5_passive_accept(struct selector_key *key)
{
    struct sockaddr_storage client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    struct socks5 *state = NULL;

    const int client = accept(key->fd, (struct sockaddr *)&client_addr,
                              &client_addr_len);
    if (client == -1) {
        goto fail;
    }
    if (selector_fd_set_nio(client) == -1) {
        goto fail;
    }
    state = socks5_new(client);
    if (state == NULL) {
        goto fail;
    }
    memcpy(&state->client_addr, &client_addr, client_addr_len);
    state->client_addr_len = client_addr_len;

    if (selector_register(key->s, client, &socks5_handler, OP_READ,
                          state) != SELECTOR_SUCCESS) {
        goto fail;
    }
    return;

fail:
    if (client != -1) {
        close(client);
    }
    /* state recién creado, una sola referencia: liberar directamente */
    socks5_destroy(state);
}

/* --- tabla de estados de la máquina --- */

static const struct state_definition states[] = {
    {
        .state         = HELLO_READ,
        .on_arrival    = hello_on_arrival,
        .on_read_ready = hello_on_read_ready,
    }, {
        .state          = HELLO_WRITE,
        .on_write_ready = hello_on_write_ready,
    }, {
        .state         = AUTH_READ,
        .on_arrival    = auth_on_arrival,
        .on_read_ready = auth_on_read_ready,
    }, {
        .state          = AUTH_WRITE,
        .on_write_ready = auth_on_write_ready,
    }, {
        .state         = REQUEST_READ,
        .on_arrival    = request_on_arrival,
        .on_read_ready = request_on_read_ready,
    }, {
        .state          = REQUEST_RESOLV,
        .on_arrival     = request_resolv_on_arrival,
        .on_block_ready = request_resolv_on_block_ready,
    }, {
        .state          = REQUEST_CONNECTING,
        .on_write_ready = request_connecting_on_write_ready,
    }, {
        .state          = REQUEST_WRITE,
        .on_write_ready = request_write_on_write_ready,
    }, {
        .state          = COPY,
        .on_arrival     = relay_on_arrival,
        .on_read_ready  = relay_on_read_ready,
        .on_write_ready = relay_on_write_ready,
    }, {
        .state = DONE,
    }, {
        .state = ERROR,
    },
};

static const struct state_definition *
socks5_describe_states(void)
{
    return states;
}
