#ifndef SOCKS5_H_g14
#define SOCKS5_H_g14

#include <netdb.h>
#include <sys/socket.h>
#include <time.h>

#include "args.h"
#include "selector.h"  /* antes que stm.h: define struct selector_key */
#include "buffer.h"
#include "stm.h"

#include "states/hello.h"
#include "states/auth.h"
#include "states/request.h"
#include "states/relay.h"

/* Tamaño de los buffers de I/O por sentido (bytes). */
#define SOCKS5_BUFFER_SIZE 4096

/* Segundos de inactividad tras los cuales una conexión se cierra por timeout. */
#define SOCKS5_IDLE_TIMEOUT 60

/* Estados de la máquina; el orden debe coincidir con la tabla en socks5.c. */
enum socks_state {
    HELLO_READ = 0,
    HELLO_WRITE,
    AUTH_READ,
    AUTH_WRITE,
    REQUEST_READ,
    REQUEST_RESOLV,
    REQUEST_CONNECTING,
    REQUEST_WRITE,
    COPY,
    DONE,
    ERROR,
};

/* Reply codes de SOCKS5 (RFC1928 §6). */
enum socks_reply {
    REPLY_SUCCEEDED            = 0x00,
    REPLY_GENERAL_FAILURE      = 0x01,
    REPLY_NOT_ALLOWED          = 0x02,
    REPLY_NETWORK_UNREACHABLE  = 0x03,
    REPLY_HOST_UNREACHABLE     = 0x04,
    REPLY_CONNECTION_REFUSED   = 0x05,
    REPLY_TTL_EXPIRED          = 0x06,
    REPLY_COMMAND_NOT_SUPPORTED = 0x07,
    REPLY_ADDRESS_NOT_SUPPORTED = 0x08,
};

/** Estado por conexión: una instancia por cliente del proxy. */
struct socks5 {
    /* --- cliente --- */
    struct sockaddr_storage client_addr;
    socklen_t               client_addr_len;
    int                     client_fd;

    /* --- origin (servidor destino) --- */
    struct sockaddr_storage origin_addr;
    socklen_t               origin_addr_len;
    int                     origin_fd;

    /* Resolución de nombres (FQDN): lista e iterador para RF4. */
    struct addrinfo *origin_resolution;
    struct addrinfo *origin_resolution_cur;

    /* Reply a enviar al cliente al cerrar la negociación. */
    enum socks_reply reply;

    /* Cierres de medio canal en el relay. */
    bool client_closed;  /* el cliente ya mandó EOF (no más datos hacia origin) */
    bool origin_closed;  /* el origin ya mandó EOF (no más datos hacia cliente) */

    /* Máquina de estados. */
    struct state_machine stm;

    /* Parsers de cada etapa (excluyentes en el tiempo). */
    union {
        struct hello_parser   hello;
        struct auth_parser    auth;
        struct request_parser request;
    } parser;

    /* Buffers: read_buffer = bytes que vienen del cliente (van al origin);
       write_buffer = bytes que van hacia el cliente (vienen del origin). */
    buffer read_buffer;
    buffer write_buffer;
    uint8_t raw_read[SOCKS5_BUFFER_SIZE];
    uint8_t raw_write[SOCKS5_BUFFER_SIZE];

    /* Conteo de referencias (un fd cliente + un fd origin comparten esta data). */
    unsigned references;
    /* Free-list del pool de conexiones. */
    struct socks5 *next;

    /* --- Fase 2: timeout de inactividad y listado de conexiones activas --- */
    time_t last_activity;         /* último evento de I/O; base del timeout */
    bool   resolving;             /* true mientras corre el thread de getaddrinfo */
    struct socks5 *active_prev;   /* lista doblemente enlazada de conexiones activas */
    struct socks5 *active_next;
};

/** Acceso al estado adjunto desde los callbacks del selector. */
#define ATTACHMENT(key) ((struct socks5 *)((key)->data))

/** Inicializa el subsistema SOCKS5 con la configuración del servidor. */
void socks5_init(const struct socks5args *args);

/** Handler de accept del socket pasivo SOCKS5. */
void socks5_passive_accept(struct selector_key *key);

/**
 * Registra el fd del origin en el selector con el handler compartido y suma
 * una referencia al estado. Lo usa el estado REQUEST al iniciar el connect.
 */
selector_status socks5_register_origin(struct selector_key *key,
                                       struct socks5 *s);

/** Libera el pool de conexiones reutilizables. */
void socks5_pool_destroy(void);

/** Cantidad de conexiones activas (para el apagado controlado — RF9). */
unsigned socks5_active_connections(void);

/** Cierra las conexiones inactivas por más de SOCKS5_IDLE_TIMEOUT segundos. */
void socks5_sweep_timeouts(fd_selector s, time_t now);

/** Configuración global (solo lectura) para validar usuarios. */
const struct socks5args *socks5_config(void);

#endif
