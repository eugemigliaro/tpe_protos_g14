#ifndef SOCKS5_H_g14
#define SOCKS5_H_g14

#include <netdb.h>
#include <pthread.h>
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

    /* Conteo de referencias: fd cliente, fd origin y job DNS comparten data. */
    unsigned references;
    /* Free-list del pool de conexiones. */
    struct socks5 *next;

    /* Timeout de inactividad y listado de conexiones activas. */
    time_t last_activity;         /* último evento de I/O; base del timeout */
    bool   resolving;             /* true mientras corre el thread de getaddrinfo */
    struct socks5 *active_prev;   /* lista doblemente enlazada de conexiones activas */
    struct socks5 *active_next;

    /* Trabajo DNS. El thread solo accede a estos campos; la referencia tomada
     * al iniciarlo mantiene viva la estructura hasta que el event loop lo
     * consume o el shutdown hace join. Los flags se protegen con el mutex de
     * jobs definido en request.c. */
    pthread_t       dns_thread;
    fd_selector     dns_selector;
    int             dns_client_fd;
    char            dns_host[SOCKS_FQDN_MAX + 1];
    char            dns_service[6];
    struct addrinfo *dns_result;
    int             dns_gai_status;
    bool            dns_active;
    bool            dns_thread_started;
    bool            dns_completed;
    bool            dns_notification_failed;
    struct socks5  *dns_next;

    /* Datos para métricas y access log. */
    char username[256];   /* usuario autenticado (RFC1929); vacío si sin auth */
    char origin_str[270]; /* "host:puerto" del destino formateado en REQUEST_READ */
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

/** Adquiere/libera una referencia explícita al estado de conexión. */
void socks5_retain(struct socks5 *s);
void socks5_release(struct socks5 *s);

/** Libera el pool de conexiones reutilizables. */
void socks5_pool_destroy(void);

/** Cantidad de conexiones activas (para el apagado controlado — RF9). */
unsigned socks5_active_connections(void);

/** Cierra las conexiones inactivas por más de SOCKS5_IDLE_TIMEOUT segundos. */
void socks5_sweep_timeouts(fd_selector s, time_t now);

/** Configuración global (solo lectura) para validar usuarios. */
const struct socks5args *socks5_config(void);

/**
 * Gestión de usuarios en runtime (para el canal de monitoreo — RF7).
 * Todas operan sobre la copia mutable local de socks5.c, no sobre config.
 */
bool socks5_validate_user(const char *name, const char *pass);
bool socks5_has_users(void);

/**
 * Cambia el tiempo de inactividad máximo en runtime (SET_TIMEOUT).
 * Un valor de 0 deshabilita el barrido de timeouts.
 */
void socks5_set_timeout(uint32_t seconds);

/** Agrega usuario. Retorna 0=ok, -1=sin espacio, -2=ya existe. */
int  socks5_add_user(const char *name, const char *pass);

/** Elimina usuario. Retorna 0=ok, -1=no encontrado. */
int  socks5_del_user(const char *name);

/**
 * Copia los nombres de usuarios activos en names[][256].
 * max debe ser >= MAX_USERS. Retorna la cantidad de usuarios.
 */
int  socks5_list_users(char names[][256], int max);

#endif
