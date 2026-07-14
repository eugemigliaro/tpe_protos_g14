/**
 * server.c - punto de entrada del servidor SOCKS5.
 *
 * Levanta un socket pasivo no bloqueante y multiplexa todo (accept, lecturas y
 * escrituras) en un único thread mediante el selector de la cátedra. La lógica
 * por conexión vive en socks5.c / states/.
 *
 * También implementa el apagado controlado, el timeout de inactividad y la
 * elevación del límite de descriptores.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "access_log.h"
#include "args.h"
#include "metrics.h"
#include "monitor.h"
#include "selector.h"
#include "socks5.h"
#include "version.h"

#define MAX_PENDING_CONN 20
#define SELECTOR_TIMEOUT 10
#define SWEEP_INTERVAL   10  /* cada cuántos segundos barrer timeouts */

/* Handler del socket pasivo SOCKS5 (accept). */
static const struct fd_handler passive_handler = {
    .handle_read = socks5_passive_accept,
};

/* Handler del socket pasivo de monitoreo (accept). */
static const struct fd_handler mng_passive_handler = {
    .handle_read = mng_passive_accept,
};

/* Contador de señales de apagado: 0 normal, 1 apagado controlado, >=2 forzar. */
static volatile sig_atomic_t shutdown_signals = 0;

static void
shutdown_handler(const int signal)
{
    (void)signal;
    shutdown_signals++;
}

/* Sube el límite de descriptores al máximo permitido (ayuda a RF1). */
static void
raise_fd_limit(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

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

    /* Apagado controlado ante SIGINT/SIGTERM. Sin SA_RESTART a propósito: así
     * la señal interrumpe el pselect del selector (EINTR) y el loop reacciona. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    raise_fd_limit();
    metrics_init();
    if (access_log_open(args.log_path) != 0) {
        fprintf(stderr, "advertencia: no se pudo abrir el access log '%s'\n",
                args.log_path);
    }
    socks5_init(&args);

    int socks_fd = create_passive_socket(args.socks_addr, args.socks_port);
    if (socks_fd < 0) {
        fprintf(stderr, "no se pudo abrir el socket SOCKS5 en %s:%u: %s\n",
                args.socks_addr, args.socks_port, strerror(errno));
        return 1;
    }

    int mng_fd = create_passive_socket(args.mng_addr, args.mng_port);
    if (mng_fd < 0) {
        fprintf(stderr, "no se pudo abrir el socket de monitoreo en %s:%u: %s\n",
                args.mng_addr, args.mng_port, strerror(errno));
        close(socks_fd);
        return 1;
    }
    mng_init(&args);

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
        fprintf(stderr, "no se pudo registrar el socket pasivo SOCKS5\n");
        ret = 1;
        goto finally;
    }
    if (selector_register(selector, mng_fd, &mng_passive_handler, OP_READ,
                          NULL) != SELECTOR_SUCCESS) {
        fprintf(stderr, "no se pudo registrar el socket pasivo de monitoreo\n");
        ret = 1;
        goto finally;
    }

    fprintf(stderr, "%s escuchando SOCKS5 en %s:%u\n", version_string(),
            args.socks_addr, args.socks_port);
    fprintf(stderr, "monitoreo en %s:%u\n", args.mng_addr, args.mng_port);

    bool   closing   = false;
    time_t last_sweep = time(NULL);

    for (;;) {
        if (selector_select(selector) != SELECTOR_SUCCESS) {
            fprintf(stderr, "error en el selector: %s\n", strerror(errno));
            ret = 1;
            break;
        }

        /* Fallback ante SELECTOR_ENOMEM en selector_notify_block(): los jobs
         * completados conservan su resultado y se reintentan desde el thread
         * seguro del event loop. */
        request_resolv_retry_notifications();

        /* Barrido de timeouts, acotado a una vez cada SWEEP_INTERVAL segundos
         * para no recorrer la lista de conexiones en cada evento de I/O. */
        const time_t now = time(NULL);
        if (now - last_sweep >= SWEEP_INTERVAL) {
            socks5_sweep_timeouts(selector, now);
            mng_sweep_timeouts(selector, now);
            last_sweep = now;
        }

        /* Segunda señal: apagado forzado. */
        if (shutdown_signals >= 2) {
            fprintf(stderr, "apagado forzado (%u SOCKS y %u monitoreo "
                    "activas descartadas)\n", socks5_active_connections(),
                    mng_active_connections());
            break;
        }
        /* Primera señal: dejar de aceptar y esperar a las conexiones activas. */
        if (shutdown_signals >= 1 && !closing) {
            closing = true;
            selector_unregister_fd(selector, socks_fd);
            close(socks_fd);
            socks_fd = -1;
            selector_unregister_fd(selector, mng_fd);
            close(mng_fd);
            mng_fd = -1;
            mng_begin_shutdown(selector);
            fprintf(stderr, "apagado controlado: no acepto mas conexiones, "
                    "esperando %u SOCKS y %u monitoreo "
                    "(senal de nuevo para forzar)\n",
                    socks5_active_connections(), mng_active_connections());
        }
        if (closing && socks5_active_connections() == 0 &&
            mng_active_connections() == 0) {
            fprintf(stderr, "todas las conexiones terminaron, apagando\n");
            break;
        }
    }

finally: {
    const unsigned pending_dns = request_resolv_pending_jobs();
    if (pending_dns > 0) {
        fprintf(stderr, "esperando %u resoluciones DNS antes de liberar recursos\n",
                pending_dns);
    }
    /* getaddrinfo() no es cancelable de forma portable. El selector y las
     * conexiones deben seguir vivos hasta que todos los workers terminen. */
    request_resolv_wait_all();
    if (selector != NULL) {
        mng_force_close_all(selector);
        selector_destroy(selector);
    }
    selector_close();
    socks5_pool_destroy();
    mng_pool_destroy();
    access_log_close();
    if (socks_fd >= 0) {
        close(socks_fd);
    }
    if (mng_fd >= 0) {
        close(mng_fd);
    }
}
    return ret;
}
