/**
 * mng_proto.h - constantes del protocolo de monitoreo MNG/1.
 *
 * Compartido entre server (monitor.c) y client (client.c).
 * Ver doc/SPEC.md para la especificación completa del protocolo.
 */
#ifndef MNG_PROTO_H_g14
#define MNG_PROTO_H_g14

/* Versión del protocolo */
#define MNG_VERSION              0x01

/* Status codes en la respuesta de autenticación */
#define MNG_AUTH_OK              0x00
#define MNG_AUTH_FAIL            0x01
#define MNG_AUTH_VER_UNSUPPORTED 0xFF

/* Status codes en la respuesta de comandos */
#define MNG_STATUS_OK            0x00
#define MNG_STATUS_NOT_FOUND     0x01   /* no encontrado, o ya existe (ADD_USER) */
#define MNG_STATUS_FULL          0x02   /* tabla de usuarios llena */
#define MNG_STATUS_BAD_ARGS      0x03   /* argumentos incorrectos / CMD desconocido */
#define MNG_STATUS_ERROR         0xFF   /* error interno del servidor */

/* Comandos (client → server) */
#define MNG_CMD_ADD_USER         0x01
#define MNG_CMD_DEL_USER         0x02
#define MNG_CMD_LIST_USERS       0x03
#define MNG_CMD_GET_STATS        0x04
#define MNG_CMD_SET_TIMEOUT      0x05
#define MNG_CMD_GET_LOG          0x06
#define MNG_CMD_CLOSE            0x07

#endif
