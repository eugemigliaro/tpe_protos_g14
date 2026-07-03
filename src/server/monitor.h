/**
 * monitor.h - canal de monitoreo MNG/1 (RF7).
 *
 * Escucha en un puerto TCP dedicado (distinto al de SOCKS5). Cada conexión
 * pasa por un handshake de autenticación y luego acepta comandos en un loop
 * hasta que el cliente envía CLOSE o cierra el socket. Todo el I/O es no
 * bloqueante; se multiplexa en el mismo selector que el tráfico SOCKS5.
 */
#ifndef MONITOR_H_g14
#define MONITOR_H_g14

#include <stdbool.h>
#include <stdint.h>

#include "args.h"
#include "buffer.h"
#include "selector.h"
#include "stm.h"

/* Tamaño del pool de conexiones de monitoreo. */
#define MNG_POOL_MAX   16
/* Buffer de lectura: suficiente para cualquier comando + args (max ~514 B). */
#define MNG_READ_SIZE  2048
/* Buffer de escritura: suficiente para GET_LOG peor caso (~51 KB). */
#define MNG_WRITE_SIZE 65536

/* Estados de la máquina de estados del canal de monitoreo. */
enum mng_state {
    MNG_AUTH_READ  = 0,
    MNG_AUTH_WRITE,
    MNG_CMD_READ,
    MNG_RESP_WRITE,
    MNG_DONE,
    MNG_ERROR,
};

/* --- parsers internos (definidos aquí para que queden en el struct) --- */

enum mng_auth_p_state {
    MNG_AUTH_P_VER = 0,
    MNG_AUTH_P_ULEN,
    MNG_AUTH_P_UNAME,
    MNG_AUTH_P_PLEN,
    MNG_AUTH_P_PASSWD,
    MNG_AUTH_P_DONE,
    MNG_AUTH_P_ERROR,
};

struct mng_auth_parser {
    enum mng_auth_p_state state;
    uint8_t ulen;
    char    uname[256];
    uint8_t plen;
    char    passwd[256];
    uint8_t idx;
};

enum mng_cmd_p_state {
    MNG_CMD_P_CMD = 0,
    MNG_CMD_P_ULEN,
    MNG_CMD_P_UNAME,
    MNG_CMD_P_PLEN,
    MNG_CMD_P_PASSWD,
    MNG_CMD_P_TIMEOUT,
    MNG_CMD_P_DONE,
    MNG_CMD_P_ERROR,
};

struct mng_cmd_parser {
    enum mng_cmd_p_state state;
    uint8_t  cmd;
    uint8_t  ulen;
    char     uname[256];
    uint8_t  plen;
    char     passwd[256];
    uint32_t timeout;   /* SET_TIMEOUT: acumulado big-endian */
    uint8_t  idx;       /* índice genérico para strings y timeout */
};

/** Estado por conexión de monitoreo. */
struct mng_conn {
    int fd;

    struct state_machine stm;

    bool authenticated;      /* true tras handshake exitoso */
    bool close_after_write;  /* true si CLOSE fue el último comando procesado */

    struct mng_auth_parser auth_parser;
    struct mng_cmd_parser  cmd_parser;

    buffer  read_buffer;
    buffer  write_buffer;
    uint8_t raw_read [MNG_READ_SIZE];
    uint8_t raw_write[MNG_WRITE_SIZE];

    struct mng_conn *next;  /* free-list del pool */
};

/** Acceso al estado adjunto desde callbacks del selector. */
#define MNG_ATTACHMENT(key) ((struct mng_conn *)((key)->data))

/**
 * Inicializa el subsistema de monitoreo con la configuración del servidor.
 * Debe llamarse antes de registrar el socket pasivo en el selector.
 */
void mng_init(const struct socks5args *args);

/** Libera el pool de conexiones de monitoreo. */
void mng_pool_destroy(void);

/**
 * Handler del socket pasivo de monitoreo: acepta una conexión entrante y la
 * registra en el selector. Se pasa como .handle_read del fd_handler pasivo.
 */
void mng_passive_accept(struct selector_key *key);

#endif
