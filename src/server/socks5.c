/**
 * socks5.c - ciclo de vida de una conexión SOCKS5 y cableado con el selector.
 *
 * Cada cliente tiene una `struct socks5` con su máquina de estados (stm.c) y
 * sus buffers. El selector (selector.c) despacha los eventos de I/O hacia la
 * stm; los estados concretos viven en states/.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "metrics.h"
#include "socks5.h"

#define POOL_MAX 64  /* conexiones cacheadas para evitar malloc/free repetidos */

/* Configuración global del servidor (solo lectura). */
static const struct socks5args *config = NULL;

/* Timeout de inactividad en segundos (modificable por SET_TIMEOUT). */
static time_t idle_timeout = SOCKS5_IDLE_TIMEOUT;

/* Copia mutable de usuarios: inicializada desde config en socks5_init()
 * y modificable en runtime mediante socks5_add/del_user(). */
struct runtime_user {
    char name[256];
    char pass[256];
    bool in_use;
};
static struct runtime_user runtime_users[MAX_USERS];

/* Free-list de structs socks5 reutilizables. */
static struct socks5 *pool      = NULL;
static unsigned       pool_size = 0;

/* Lista de conexiones activas (para timeouts y apagado controlado). */
static struct socks5 *active_list  = NULL;
static unsigned       active_count = 0;

static void
active_add(struct socks5 *s)
{
    s->active_prev = NULL;
    s->active_next = active_list;
    if (active_list != NULL) {
        active_list->active_prev = s;
    }
    active_list = s;
    active_count++;
}

static void
active_remove(struct socks5 *s)
{
    if (s->active_prev != NULL) {
        s->active_prev->active_next = s->active_next;
    } else {
        active_list = s->active_next;
    }
    if (s->active_next != NULL) {
        s->active_next->active_prev = s->active_prev;
    }
    s->active_prev = s->active_next = NULL;
    active_count--;
}

unsigned
socks5_active_connections(void)
{
    return active_count;
}

/* --- declaración adelantada de la tabla de estados --- */
static const struct state_definition *socks5_describe_states(void);

void
socks5_init(const struct socks5args *args)
{
    config = args;
    memset(runtime_users, 0, sizeof(runtime_users));
    for (int i = 0; i < MAX_USERS; i++) {
        if (args->users[i].name != NULL) {
            strncpy(runtime_users[i].name, args->users[i].name,
                    sizeof(runtime_users[i].name) - 1);
            strncpy(runtime_users[i].pass, args->users[i].pass,
                    sizeof(runtime_users[i].pass) - 1);
            runtime_users[i].in_use = true;
        }
    }
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

    s->last_activity = time(NULL);
    s->resolving     = false;
    s->username[0]   = '\0';
    s->origin_str[0] = '\0';
    active_add(s);
    metrics_conn_open();
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
    metrics_conn_close();
    active_remove(s);
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

/* Cierra una conexión desde afuera de sus handlers (barrido de timeouts). */
static void
socks5_kill(fd_selector s, struct socks5 *cs)
{
    const int fds[] = { cs->client_fd, cs->origin_fd };
    for (unsigned i = 0; i < 2; i++) {
        if (fds[i] != -1) {
            selector_unregister_fd(s, fds[i]); /* dispara handle_close -> destroy */
            close(fds[i]);
        }
    }
}

void
socks5_sweep_timeouts(fd_selector s, time_t now)
{
    struct socks5 *cur = active_list;
    while (cur != NULL) {
        /* cur puede volver al pool al cerrarlo: guardamos el siguiente antes */
        struct socks5 *next = cur->active_next;
        if (idle_timeout > 0 &&
            !cur->resolving && now - cur->last_activity >= idle_timeout) {
            socks5_kill(s, cur);
        }
        cur = next;
    }
}

static void
socks5_read(struct selector_key *key)
{
    ATTACHMENT(key)->last_activity = time(NULL);
    struct state_machine *stm = &ATTACHMENT(key)->stm;
    const enum socks_state st = stm_handler_read(stm, key);
    if (st == ERROR || st == DONE) {
        socks5_done(key);
    }
}

static void
socks5_write(struct selector_key *key)
{
    ATTACHMENT(key)->last_activity = time(NULL);
    struct state_machine *stm = &ATTACHMENT(key)->stm;
    const enum socks_state st = stm_handler_write(stm, key);
    if (st == ERROR || st == DONE) {
        socks5_done(key);
    }
}

static void
socks5_block(struct selector_key *key)
{
    ATTACHMENT(key)->last_activity = time(NULL);
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

/* --- gestión de usuarios en runtime (RF7) --- */

bool
socks5_validate_user(const char *name, const char *pass)
{
    for (int i = 0; i < MAX_USERS; i++) {
        if (runtime_users[i].in_use &&
            strcmp(runtime_users[i].name, name) == 0 &&
            strcmp(runtime_users[i].pass, pass) == 0) {
            return true;
        }
    }
    return false;
}

bool
socks5_has_users(void)
{
    for (int i = 0; i < MAX_USERS; i++) {
        if (runtime_users[i].in_use) {
            return true;
        }
    }
    return false;
}

int
socks5_add_user(const char *name, const char *pass)
{
    for (int i = 0; i < MAX_USERS; i++) {
        if (runtime_users[i].in_use &&
            strcmp(runtime_users[i].name, name) == 0) {
            return -2; /* ya existe */
        }
    }
    for (int i = 0; i < MAX_USERS; i++) {
        if (!runtime_users[i].in_use) {
            strncpy(runtime_users[i].name, name,
                    sizeof(runtime_users[i].name) - 1);
            strncpy(runtime_users[i].pass, pass,
                    sizeof(runtime_users[i].pass) - 1);
            runtime_users[i].in_use = true;
            return 0;
        }
    }
    return -1; /* sin espacio */
}

int
socks5_del_user(const char *name)
{
    for (int i = 0; i < MAX_USERS; i++) {
        if (runtime_users[i].in_use &&
            strcmp(runtime_users[i].name, name) == 0) {
            memset(&runtime_users[i], 0, sizeof(runtime_users[i]));
            return 0;
        }
    }
    return -1; /* no encontrado */
}

int
socks5_list_users(char names[][256], int max)
{
    int count = 0;
    for (int i = 0; i < MAX_USERS && count < max; i++) {
        if (runtime_users[i].in_use) {
            strncpy(names[count], runtime_users[i].name, 255);
            names[count][255] = '\0';
            count++;
        }
    }
    return count;
}

void
socks5_set_timeout(uint32_t seconds)
{
    idle_timeout = (time_t)seconds;
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
