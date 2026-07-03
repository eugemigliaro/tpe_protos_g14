/**
 * client.c - cliente de monitoreo MNG/1 (RNF5).
 *
 * I/O bloqueante (permitido para el cliente). Flujo: connect → handshake
 * de auth → enviar CMD → leer STATUS+data → imprimir → CLOSE → salir.
 *
 * Uso:
 *   ./bin/client [-L addr] [-P puerto] -A admin:pass COMANDO [args]
 *
 * Comandos:
 *   stats                  métricas del proxy
 *   users                  lista de usuarios SOCKS5
 *   add-user NOMBRE PASS   agrega un usuario
 *   del-user NOMBRE        elimina un usuario
 *   set-timeout SEG        cambia el idle timeout en runtime
 *   log                    últimas entradas del access log
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mng_proto.h"
#include "version.h"

/* -------------------------------------------------------------------------
 * I/O helpers
 * ---------------------------------------------------------------------- */

/* Lee exactamente n bytes de fd; retorna 0 ok, -1 error/EOF. */
static int
read_all(int fd, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        const ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0) {
            return -1;
        }
        done += (size_t)r;
    }
    return 0;
}

/* Escribe exactamente n bytes a fd; retorna 0 ok, -1 error. */
static int
write_all(int fd, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        const ssize_t w = write(fd, (const char *)buf + done, n - done);
        if (w <= 0) {
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

/* Decodifica uint64 big-endian. */
static uint64_t
rd_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* Decodifica uint32 big-endian. */
static uint32_t
rd_u32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* Decodifica uint16 big-endian. */
static uint16_t
rd_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* -------------------------------------------------------------------------
 * Conexión TCP (bloqueante)
 * ---------------------------------------------------------------------- */

static int
tcp_connect(const char *addr, unsigned short port)
{
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *res;
    if (getaddrinfo(addr, port_str, &hints, &res) != 0) {
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* -------------------------------------------------------------------------
 * Handshake de autenticación
 * ---------------------------------------------------------------------- */

static int
do_handshake(int fd, const char *name, const char *pass)
{
    const uint8_t ulen = (uint8_t)strlen(name);
    const uint8_t plen = (uint8_t)strlen(pass);

    /* VER(1) ULEN(1) UNAME(ULEN) PLEN(1) PASSWD(PLEN) */
    uint8_t buf[3 + 255 + 255];
    int n = 0;
    buf[n++] = MNG_VERSION;
    buf[n++] = ulen;
    memcpy(buf + n, name, ulen);
    n += ulen;
    buf[n++] = plen;
    memcpy(buf + n, pass, plen);
    n += plen;

    if (write_all(fd, buf, (size_t)n) < 0) {
        fprintf(stderr, "error enviando handshake\n");
        return -1;
    }

    uint8_t resp[2];
    if (read_all(fd, resp, 2) < 0) {
        fprintf(stderr, "error leyendo respuesta de autenticación\n");
        return -1;
    }
    if (resp[0] != MNG_VERSION) {
        fprintf(stderr, "versión de protocolo no soportada: 0x%02x\n", resp[0]);
        return -1;
    }
    switch (resp[1]) {
        case MNG_AUTH_OK:
            return 0;
        case MNG_AUTH_FAIL:
            fprintf(stderr, "autenticación fallida: credenciales inválidas\n");
            break;
        case MNG_AUTH_VER_UNSUPPORTED:
            fprintf(stderr, "versión de protocolo no soportada por el servidor\n");
            break;
        default:
            fprintf(stderr, "respuesta de auth desconocida: 0x%02x\n", resp[1]);
            break;
    }
    return -1;
}

/* Envía CLOSE y lee el STATUS (ignora el resultado — es cleanup). */
static void
send_close(int fd)
{
    const uint8_t req = MNG_CMD_CLOSE;
    if (write_all(fd, &req, 1) < 0) {
        return;
    }
    uint8_t status;
    read_all(fd, &status, 1);
}

/* -------------------------------------------------------------------------
 * Comandos
 * ---------------------------------------------------------------------- */

static int
cmd_stats(int fd)
{
    const uint8_t req = MNG_CMD_GET_STATS;
    if (write_all(fd, &req, 1) < 0) {
        return -1;
    }

    /* STATUS(1) HIST(8) CURR(4) BYTES_SENT(8) BYTES_RECV(8) = 29 bytes */
    uint8_t resp[1 + 8 + 4 + 8 + 8];
    if (read_all(fd, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "error leyendo estadísticas\n");
        return -1;
    }
    if (resp[0] != MNG_STATUS_OK) {
        fprintf(stderr, "error del servidor: 0x%02x\n", resp[0]);
        return -1;
    }

    const uint64_t hist = rd_u64(resp + 1);
    const uint32_t curr = rd_u32(resp + 9);
    const uint64_t sent = rd_u64(resp + 13);
    const uint64_t recv = rd_u64(resp + 21);

    printf("Conexiones históricas: %llu\n", (unsigned long long)hist);
    printf("Conexiones activas:    %u\n",   curr);
    printf("Bytes enviados:        %llu\n", (unsigned long long)sent);
    printf("Bytes recibidos:       %llu\n", (unsigned long long)recv);
    return 0;
}

static int
cmd_users(int fd)
{
    const uint8_t req = MNG_CMD_LIST_USERS;
    if (write_all(fd, &req, 1) < 0) {
        return -1;
    }

    uint8_t status;
    if (read_all(fd, &status, 1) < 0) {
        return -1;
    }
    if (status != MNG_STATUS_OK) {
        fprintf(stderr, "error del servidor: 0x%02x\n", status);
        return -1;
    }

    uint8_t count;
    if (read_all(fd, &count, 1) < 0) {
        return -1;
    }
    printf("Usuarios (%u):\n", count);
    for (uint8_t i = 0; i < count; i++) {
        uint8_t ulen;
        if (read_all(fd, &ulen, 1) < 0) {
            return -1;
        }
        char name[256];
        if (ulen > 0 && read_all(fd, name, ulen) < 0) {
            return -1;
        }
        name[ulen] = '\0';
        printf("  %s\n", name);
    }
    return 0;
}

static int
cmd_add_user(int fd, const char *name, const char *pass)
{
    const uint8_t ulen = (uint8_t)strlen(name);
    const uint8_t plen = (uint8_t)strlen(pass);

    uint8_t buf[1 + 1 + 255 + 1 + 255];
    int n = 0;
    buf[n++] = MNG_CMD_ADD_USER;
    buf[n++] = ulen;
    memcpy(buf + n, name, ulen);
    n += ulen;
    buf[n++] = plen;
    memcpy(buf + n, pass, plen);
    n += plen;

    if (write_all(fd, buf, (size_t)n) < 0) {
        return -1;
    }

    uint8_t status;
    if (read_all(fd, &status, 1) < 0) {
        return -1;
    }
    if (status != MNG_STATUS_OK) {
        if (status == MNG_STATUS_FULL) {
            fprintf(stderr, "error: tabla de usuarios llena\n");
        } else if (status == MNG_STATUS_NOT_FOUND) {
            fprintf(stderr, "error: el usuario ya existe\n");
        } else {
            fprintf(stderr, "error del servidor: 0x%02x\n", status);
        }
        return -1;
    }
    printf("OK\n");
    return 0;
}

static int
cmd_del_user(int fd, const char *name)
{
    const uint8_t ulen = (uint8_t)strlen(name);

    uint8_t buf[1 + 1 + 255];
    int n = 0;
    buf[n++] = MNG_CMD_DEL_USER;
    buf[n++] = ulen;
    memcpy(buf + n, name, ulen);
    n += ulen;

    if (write_all(fd, buf, (size_t)n) < 0) {
        return -1;
    }

    uint8_t status;
    if (read_all(fd, &status, 1) < 0) {
        return -1;
    }
    if (status != MNG_STATUS_OK) {
        if (status == MNG_STATUS_NOT_FOUND) {
            fprintf(stderr, "error: usuario no encontrado\n");
        } else {
            fprintf(stderr, "error del servidor: 0x%02x\n", status);
        }
        return -1;
    }
    printf("OK\n");
    return 0;
}

static int
cmd_set_timeout(int fd, uint32_t seconds)
{
    uint8_t buf[5];
    buf[0] = MNG_CMD_SET_TIMEOUT;
    buf[1] = (uint8_t)((seconds >> 24) & 0xFF);
    buf[2] = (uint8_t)((seconds >> 16) & 0xFF);
    buf[3] = (uint8_t)((seconds >>  8) & 0xFF);
    buf[4] = (uint8_t)((seconds >>  0) & 0xFF);

    if (write_all(fd, buf, sizeof(buf)) < 0) {
        return -1;
    }

    uint8_t status;
    if (read_all(fd, &status, 1) < 0) {
        return -1;
    }
    if (status != MNG_STATUS_OK) {
        fprintf(stderr, "error del servidor: 0x%02x\n", status);
        return -1;
    }
    printf("OK\n");
    return 0;
}

static int
cmd_log(int fd)
{
    const uint8_t req = MNG_CMD_GET_LOG;
    if (write_all(fd, &req, 1) < 0) {
        return -1;
    }

    uint8_t status;
    if (read_all(fd, &status, 1) < 0) {
        return -1;
    }
    if (status != MNG_STATUS_OK) {
        fprintf(stderr, "error del servidor: 0x%02x\n", status);
        return -1;
    }

    uint8_t cnt_buf[2];
    if (read_all(fd, cnt_buf, 2) < 0) {
        return -1;
    }
    const uint16_t count = rd_u16(cnt_buf);

    printf("Entradas del log (%u):\n", count);
    for (uint16_t i = 0; i < count; i++) {
        uint8_t elen_buf[2];
        if (read_all(fd, elen_buf, 2) < 0) {
            return -1;
        }
        uint16_t elen = rd_u16(elen_buf);
        if (elen > 511) {
            elen = 511;
        }
        char entry[512];
        if (elen > 0 && read_all(fd, entry, elen) < 0) {
            return -1;
        }
        entry[elen] = '\0';
        printf("  %s\n", entry);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Uso y main
 * ---------------------------------------------------------------------- */

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-L addr] [-P puerto] -A admin:pass COMANDO [args]\n"
        "\n"
        "Opciones:\n"
        "  -L ADDR    dirección del servicio de monitoreo (default 127.0.0.1)\n"
        "  -P PUERTO  puerto del servicio de monitoreo (default 8080)\n"
        "  -A CRED    credencial en formato usuario:contraseña\n"
        "  -v         imprime versión y termina\n"
        "  -h         imprime esta ayuda y termina\n"
        "\n"
        "Comandos:\n"
        "  stats                  muestra métricas del proxy\n"
        "  users                  lista los usuarios SOCKS5\n"
        "  add-user NOMBRE PASS   agrega un usuario\n"
        "  del-user NOMBRE        elimina un usuario\n"
        "  set-timeout SEG        cambia el idle timeout (0 = sin timeout)\n"
        "  log                    muestra las últimas entradas del access log\n",
        prog);
}

/* Divide "nombre:contraseña" en sus partes. Retorna 0 ok, -1 error. */
static int
parse_admin_cred(const char *src,
                 char *name, size_t name_sz,
                 char *pass, size_t pass_sz)
{
    const char *colon = strchr(src, ':');
    if (colon == NULL) {
        return -1;
    }
    const size_t nlen = (size_t)(colon - src);
    const size_t plen = strlen(colon + 1);
    if (nlen == 0 || nlen >= name_sz || plen >= pass_sz) {
        return -1;
    }
    memcpy(name, src, nlen);
    name[nlen] = '\0';
    memcpy(pass, colon + 1, plen);
    pass[plen] = '\0';
    return 0;
}

int
main(int argc, char *argv[])
{
    const char    *addr       = "127.0.0.1";
    unsigned short port       = 8080;
    char           admin_name[256] = "";
    char           admin_pass[256] = "";

    int opt;
    while ((opt = getopt(argc, argv, "hA:L:P:v")) != -1) {
        switch (opt) {
            case 'L':
                addr = optarg;
                break;
            case 'P':
                port = (unsigned short)atoi(optarg);
                break;
            case 'A':
                if (parse_admin_cred(optarg,
                                     admin_name, sizeof(admin_name),
                                     admin_pass, sizeof(admin_pass)) < 0) {
                    fprintf(stderr, "formato de -A: admin:contraseña\n");
                    return 1;
                }
                break;
            case 'v':
                printf("client %s\n", version_string());
                return 0;
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (admin_name[0] == '\0') {
        fprintf(stderr, "se requiere -A admin:contraseña\n");
        usage(argv[0]);
        return 1;
    }
    if (optind >= argc) {
        fprintf(stderr, "se requiere un comando\n");
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[optind];

    const int fd = tcp_connect(addr, port);
    if (fd < 0) {
        fprintf(stderr, "no se pudo conectar a %s:%u: %s\n",
                addr, port, strerror(errno));
        return 1;
    }

    if (do_handshake(fd, admin_name, admin_pass) < 0) {
        close(fd);
        return 1;
    }

    int ret = 0;

    if (strcmp(cmd, "stats") == 0) {
        ret = cmd_stats(fd);

    } else if (strcmp(cmd, "users") == 0) {
        ret = cmd_users(fd);

    } else if (strcmp(cmd, "add-user") == 0) {
        if (optind + 2 >= argc) {
            fprintf(stderr, "uso: add-user NOMBRE CONTRASEÑA\n");
            ret = 1;
        } else {
            ret = cmd_add_user(fd, argv[optind + 1], argv[optind + 2]);
        }

    } else if (strcmp(cmd, "del-user") == 0) {
        if (optind + 1 >= argc) {
            fprintf(stderr, "uso: del-user NOMBRE\n");
            ret = 1;
        } else {
            ret = cmd_del_user(fd, argv[optind + 1]);
        }

    } else if (strcmp(cmd, "set-timeout") == 0) {
        if (optind + 1 >= argc) {
            fprintf(stderr, "uso: set-timeout SEGUNDOS\n");
            ret = 1;
        } else {
            ret = cmd_set_timeout(fd, (uint32_t)atoi(argv[optind + 1]));
        }

    } else if (strcmp(cmd, "log") == 0) {
        ret = cmd_log(fd);

    } else {
        fprintf(stderr, "comando desconocido: %s\n", cmd);
        usage(argv[0]);
        ret = 1;
    }

    send_close(fd);
    close(fd);
    return ret;
}
