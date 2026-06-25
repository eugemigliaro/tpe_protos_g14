/**
 * server.c - punto de entrada del servidor SOCKS5.
 *
 * Levanta un socket pasivo no bloqueante y multiplexa todo (accept, lecturas y
 * escrituras) en un único thread mediante el selector de la cátedra. La lógica
 * por conexión vive en socks5.c / states/.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "args.h"
#include "selector.h"
#include "socks5.h"
#include "version.h"

#define MAX_PENDING_CONN 20
#define SELECTOR_TIMEOUT 10

/* Handler del socket pasivo: solo nos interesa el evento de lectura (accept). */
static const struct fd_handler passive_handler = {
    .handle_read = socks5_passive_accept,
};

/* Crea un socket pasivo IPv4 no bloqueante en addr:port. -1 ante error. */
static int
create_passive_socket(const char *addr, unsigned short port)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, MAX_PENDING_CONN) < 0 ||
        selector_fd_set_nio(fd) == -1) {
        close(fd);
        return -1;
    }
    return fd;
}

int
main(int argc, char *argv[])
{
    struct socks5args args;
    parse_args(argc, argv, &args);

    /* respaldo a MSG_NOSIGNAL: nunca morir por escribir a un peer cerrado */
    signal(SIGPIPE, SIG_IGN);

    socks5_init(&args);

    const int socks_fd = create_passive_socket(args.socks_addr, args.socks_port);
    if (socks_fd < 0) {
        fprintf(stderr, "no se pudo abrir el socket SOCKS5 en %s:%u: %s\n",
                args.socks_addr, args.socks_port, strerror(errno));
        return 1;
    }

    const struct selector_init conf = {
        .signal = SIGUSR1,
        .select_timeout = { .tv_sec = SELECTOR_TIMEOUT, .tv_nsec = 0 },
    };
    fd_selector selector = NULL;
    int ret = 0;

    if (selector_init(&conf) != SELECTOR_SUCCESS) {
        fprintf(stderr, "no se pudo inicializar el selector\n");
        ret = 1;
        goto finally;
    }
    selector = selector_new(1024);
    if (selector == NULL) {
        fprintf(stderr, "no se pudo crear el selector\n");
        ret = 1;
        goto finally;
    }
    if (selector_register(selector, socks_fd, &passive_handler, OP_READ,
                          NULL) != SELECTOR_SUCCESS) {
        fprintf(stderr, "no se pudo registrar el socket pasivo\n");
        ret = 1;
        goto finally;
    }

    fprintf(stderr, "%s escuchando SOCKS5 en %s:%u\n", version_string(),
            args.socks_addr, args.socks_port);

    for (;;) {
        if (selector_select(selector) != SELECTOR_SUCCESS) {
            fprintf(stderr, "error en el selector: %s\n", strerror(errno));
            ret = 1;
            break;
        }
    }

finally:
    if (selector != NULL) {
        selector_destroy(selector);
    }
    selector_close();
    socks5_pool_destroy();
    if (socks_fd >= 0) {
        close(socks_fd);
    }
    return ret;
}
