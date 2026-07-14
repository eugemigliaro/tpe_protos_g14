/**
 * request.c - estados REQUEST / RESOLVE / CONNECTING / REQUEST_WRITE.
 *
 *   cliente -> VER(0x05) CMD RSV(0x00) ATYP DST.ADDR DST.PORT
 *   servidor -> VER(0x05) REP RSV(0x00) ATYP BND.ADDR BND.PORT
 *
 * Solo se soporta CONNECT. La resolución de FQDN corre en un thread aparte que
 * únicamente resuelve y despierta al thread principal (selector_notify_block).
 * Ante múltiples IPs se intenta cada una hasta lograr conexión (RF4) y se
 * reportan todos los reply codes según la causa de fallo (RF5).
 */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "socks5.h"
#include "states/states_common.h"

/* --- forward declarations --- */
static unsigned request_connect(struct selector_key *key);

/* Jobs DNS en vuelo. Solo el event loop modifica la lista; los workers
 * publican resultado/completion bajo este mutex. */
static pthread_mutex_t dns_jobs_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct socks5  *dns_jobs       = NULL;
static unsigned        dns_jobs_count = 0;

static void
dns_job_remove_locked(struct socks5 *s)
{
    struct socks5 **cur = &dns_jobs;
    while (*cur != NULL && *cur != s) {
        cur = &(*cur)->dns_next;
    }
    assert(*cur == s);
    assert(dns_jobs_count > 0);
    *cur = s->dns_next;
    s->dns_next = NULL;
    dns_jobs_count--;
}

/* Intenta encolar la completion. Si selector_notify_block() no puede reservar
 * su nodo, el flag queda levantado para que el event loop reintente. */
static void
dns_job_notify(struct socks5 *s)
{
    const selector_status st = selector_notify_block(s->dns_selector,
                                                       s->dns_client_fd);
    pthread_mutex_lock(&dns_jobs_mutex);
    if (s->dns_active) {
        s->dns_notification_failed = st != SELECTOR_SUCCESS;
    }
    pthread_mutex_unlock(&dns_jobs_mutex);
}

/* Formatea "host:puerto" en s->origin_str para el access log. */
static void
format_origin_str(struct socks5 *s)
{
    struct request_parser *p = &s->parser.request;
    const uint16_t port = (uint16_t)((p->port[0] << 8) | p->port[1]);
    switch (p->atyp) {
        case SOCKS_ATYP_IPV4: {
            char ipstr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, p->addr, ipstr, sizeof(ipstr));
            snprintf(s->origin_str, sizeof(s->origin_str), "%s:%u", ipstr, port);
            break;
        }
        case SOCKS_ATYP_IPV6: {
            char ipstr[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, p->addr, ipstr, sizeof(ipstr));
            snprintf(s->origin_str, sizeof(s->origin_str), "[%s]:%u", ipstr, port);
            break;
        }
        default: /* FQDN */
            snprintf(s->origin_str, sizeof(s->origin_str), "%s:%u",
                     (const char *)p->addr, port);
            break;
    }
}
static unsigned request_connect_fail(struct selector_key *key, int err);
static unsigned request_reply(struct selector_key *key);

static enum socks_reply
reply_from_errno(int e)
{
    switch (e) {
        case 0:            return REPLY_SUCCEEDED;
        case ECONNREFUSED: return REPLY_CONNECTION_REFUSED;
        case ENETUNREACH:  return REPLY_NETWORK_UNREACHABLE;
        case EHOSTUNREACH: return REPLY_HOST_UNREACHABLE;
        case ETIMEDOUT:    return REPLY_TTL_EXPIRED;
        default:           return REPLY_GENERAL_FAILURE;
    }
}

/* Escribe el reply SOCKS5 en write_buffer usando s->reply y la dirección local
 * del socket origin (BND) si la conexión fue exitosa. */
static void
request_build_reply(struct socks5 *s)
{
    buffer *b = &s->write_buffer;
    buffer_write(b, 0x05);
    buffer_write(b, (uint8_t)s->reply);
    buffer_write(b, 0x00);

    struct sockaddr_storage bnd;
    socklen_t bnd_len = sizeof(bnd);
    const bool have_bnd = s->reply == REPLY_SUCCEEDED && s->origin_fd != -1 &&
        getsockname(s->origin_fd, (struct sockaddr *)&bnd, &bnd_len) == 0;

    if (have_bnd && bnd.ss_family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&bnd;
        buffer_write(b, SOCKS_ATYP_IPV6);
        const uint8_t *ip = (const uint8_t *)&a->sin6_addr;
        for (int i = 0; i < 16; i++) {
            buffer_write(b, ip[i]);
        }
        buffer_write(b, ((const uint8_t *)&a->sin6_port)[0]);
        buffer_write(b, ((const uint8_t *)&a->sin6_port)[1]);
    } else if (have_bnd && bnd.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&bnd;
        buffer_write(b, SOCKS_ATYP_IPV4);
        const uint8_t *ip = (const uint8_t *)&a->sin_addr;
        for (int i = 0; i < 4; i++) {
            buffer_write(b, ip[i]);
        }
        buffer_write(b, ((const uint8_t *)&a->sin_port)[0]);
        buffer_write(b, ((const uint8_t *)&a->sin_port)[1]);
    } else {
        /* fallo o sin info: ATYP IPv4 0.0.0.0:0 */
        buffer_write(b, SOCKS_ATYP_IPV4);
        for (int i = 0; i < 6; i++) {
            buffer_write(b, 0x00);
        }
    }
}

/* Toma la próxima IP de la lista de resolución hacia origin_addr (RF4). */
static bool
request_pick_next(struct socks5 *s)
{
    if (s->origin_resolution_cur == NULL) {
        return false;
    }
    memcpy(&s->origin_addr, s->origin_resolution_cur->ai_addr,
           s->origin_resolution_cur->ai_addrlen);
    s->origin_addr_len       = s->origin_resolution_cur->ai_addrlen;
    s->origin_resolution_cur = s->origin_resolution_cur->ai_next;
    return true;
}

/* --- parser del request --- */

static void
request_feed(struct request_parser *p, uint8_t b)
{
    switch (p->state) {
        case REQ_P_VERSION:
            p->state = (b == 0x05) ? REQ_P_CMD : REQ_P_ERROR;
            break;
        case REQ_P_CMD:
            p->cmd = b;
            p->state = REQ_P_RSV;
            break;
        case REQ_P_RSV:
            p->state = (b == 0x00) ? REQ_P_ATYP : REQ_P_ERROR;
            break;
        case REQ_P_ATYP:
            p->atyp = b;
            p->idx  = 0;
            switch (b) {
                case SOCKS_ATYP_IPV4: p->addr_len = 4;  p->state = REQ_P_DADDR; break;
                case SOCKS_ATYP_IPV6: p->addr_len = 16; p->state = REQ_P_DADDR; break;
                case SOCKS_ATYP_FQDN: p->state = REQ_P_DADDR_LEN; break;
                default:              p->state = REQ_P_ERROR; break;
            }
            break;
        case REQ_P_DADDR_LEN:
            p->addr_len = b;
            p->idx = 0;
            p->state = (b == 0) ? REQ_P_ERROR : REQ_P_DADDR;
            break;
        case REQ_P_DADDR:
            p->addr[p->idx++] = b;
            if (p->idx == p->addr_len) {
                if (p->atyp == SOCKS_ATYP_FQDN) {
                    p->addr[p->idx] = '\0';
                }
                p->idx = 0;
                p->state = REQ_P_DPORT;
            }
            break;
        case REQ_P_DPORT:
            p->port[p->idx++] = b;
            if (p->idx == 2) {
                p->state = REQ_P_DONE;
            }
            break;
        default:
            break;
    }
}

void
request_on_arrival(const unsigned state, struct selector_key *key)
{
    (void)state;
    struct request_parser *p = &ATTACHMENT(key)->parser.request;
    p->state = REQ_P_VERSION;
    p->idx   = 0;
}

unsigned
request_on_read_ready(struct selector_key *key)
{
    struct socks5 *s = ATTACHMENT(key);
    struct request_parser *p = &s->parser.request;

    const ssize_t n = socks_recv(key, &s->read_buffer);
    if (n == 0) {
        return ERROR;
    }
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? REQUEST_READ : ERROR;
    }
    while (buffer_can_read(&s->read_buffer) &&
           p->state != REQ_P_DONE && p->state != REQ_P_ERROR) {
        request_feed(p, buffer_read(&s->read_buffer));
    }
    if (p->state == REQ_P_ERROR) {
        s->reply = REPLY_GENERAL_FAILURE;
        return request_reply(key);
    }
    if (p->state != REQ_P_DONE) {
        return REQUEST_READ;
    }

    format_origin_str(s);

    if (p->cmd != SOCKS_CMD_CONNECT) {
        s->reply = REPLY_COMMAND_NOT_SUPPORTED;
        return request_reply(key);
    }

    switch (p->atyp) {
        case SOCKS_ATYP_IPV4: {
            struct sockaddr_in *a = (struct sockaddr_in *)&s->origin_addr;
            memset(a, 0, sizeof(*a));
            a->sin_family = AF_INET;
            memcpy(&a->sin_addr, p->addr, 4);
            memcpy(&a->sin_port, p->port, 2);
            s->origin_addr_len = sizeof(*a);
            return request_connect(key);
        }
        case SOCKS_ATYP_IPV6: {
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)&s->origin_addr;
            memset(a, 0, sizeof(*a));
            a->sin6_family = AF_INET6;
            memcpy(&a->sin6_addr, p->addr, 16);
            memcpy(&a->sin6_port, p->port, 2);
            s->origin_addr_len = sizeof(*a);
            return request_connect(key);
        }
        default: /* FQDN */
            return REQUEST_RESOLV;
    }
}

/* --- RESOLVE: getaddrinfo en un thread dedicado --- */

static void *
request_resolv_thread(void *arg)
{
    struct socks5 *s = arg;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *result = NULL;
    const int gai_status = getaddrinfo(s->dns_host, s->dns_service,
                                       &hints, &result);

    pthread_mutex_lock(&dns_jobs_mutex);
    s->dns_result     = result;
    s->dns_gai_status = gai_status;
    s->dns_completed  = true;
    pthread_mutex_unlock(&dns_jobs_mutex);

    dns_job_notify(s);
    return NULL;
}

void
request_resolv_on_arrival(const unsigned state, struct selector_key *key)
{
    (void)state;
    struct socks5 *s = ATTACHMENT(key);
    struct request_parser *p = &s->parser.request;

    s->resolving = true; /* el barrido de timeouts no debe tocarla */
    selector_set_interest_key(key, OP_NOOP);
    assert(!s->dns_active);

    memcpy(s->dns_host, p->addr, p->addr_len);
    s->dns_host[p->addr_len] = '\0';
    const uint16_t port = (uint16_t)((p->port[0] << 8) | p->port[1]);
    snprintf(s->dns_service, sizeof(s->dns_service), "%u", port);
    s->dns_selector            = key->s;
    s->dns_client_fd           = key->fd;
    s->dns_result              = NULL;
    s->dns_gai_status          = EAI_SYSTEM;
    s->dns_completed           = false;
    s->dns_notification_failed = false;
    s->dns_thread_started      = false;
    s->dns_active              = true;

    /* El job mantiene viva la conexión aunque se desregistren sus fds. */
    socks5_retain(s);

    pthread_mutex_lock(&dns_jobs_mutex);
    s->dns_next = dns_jobs;
    dns_jobs = s;
    dns_jobs_count++;

    const int create_status = pthread_create(&s->dns_thread, NULL,
                                              request_resolv_thread, s);
    if (create_status == 0) {
        s->dns_thread_started = true;
    } else {
        /* Se procesa como una resolución fallida mediante el mismo camino. */
        s->dns_completed = true;
    }
    pthread_mutex_unlock(&dns_jobs_mutex);

    if (create_status != 0) {
        dns_job_notify(s);
    }
}

unsigned
request_resolv_on_block_ready(struct selector_key *key)
{
    struct socks5 *s = ATTACHMENT(key);

    pthread_mutex_lock(&dns_jobs_mutex);
    if (!s->dns_active || !s->dns_completed) {
        pthread_mutex_unlock(&dns_jobs_mutex);
        return REQUEST_RESOLV;
    }
    const bool thread_started = s->dns_thread_started;
    pthread_mutex_unlock(&dns_jobs_mutex);

    /* La notificación se envía al final del worker: este join no espera al
     * getaddrinfo(), solo termina de sincronizar y recupera sus recursos. */
    if (thread_started) {
        pthread_join(s->dns_thread, NULL);
    }

    pthread_mutex_lock(&dns_jobs_mutex);
    struct addrinfo *result = s->dns_result;
    const int gai_status = s->dns_gai_status;
    dns_job_remove_locked(s);
    s->dns_result              = NULL;
    s->dns_active              = false;
    s->dns_thread_started      = false;
    s->dns_completed           = false;
    s->dns_notification_failed = false;
    pthread_mutex_unlock(&dns_jobs_mutex);

    s->resolving = false; /* el thread de resolución ya terminó */
    s->origin_resolution = result;
    socks5_release(s); /* referencia propia del job DNS */

    if (gai_status != 0 || s->origin_resolution == NULL) {
        s->reply = REPLY_HOST_UNREACHABLE;
        return request_reply(key);
    }
    s->origin_resolution_cur = s->origin_resolution;
    request_pick_next(s);
    return request_connect(key);
}

void
request_resolv_retry_notifications(void)
{
    pthread_mutex_lock(&dns_jobs_mutex);
    for (struct socks5 *s = dns_jobs; s != NULL; s = s->dns_next) {
        if (s->dns_completed && s->dns_notification_failed) {
            const selector_status st = selector_notify_block(s->dns_selector,
                                                               s->dns_client_fd);
            s->dns_notification_failed = st != SELECTOR_SUCCESS;
        }
    }
    pthread_mutex_unlock(&dns_jobs_mutex);
}

unsigned
request_resolv_pending_jobs(void)
{
    pthread_mutex_lock(&dns_jobs_mutex);
    const unsigned count = dns_jobs_count;
    pthread_mutex_unlock(&dns_jobs_mutex);
    return count;
}

void
request_resolv_wait_all(void)
{
    for (;;) {
        pthread_mutex_lock(&dns_jobs_mutex);
        struct socks5 *s = dns_jobs;
        if (s == NULL) {
            pthread_mutex_unlock(&dns_jobs_mutex);
            break;
        }
        const bool thread_started = s->dns_thread_started;
        pthread_mutex_unlock(&dns_jobs_mutex);

        if (thread_started) {
            pthread_join(s->dns_thread, NULL);
        }

        pthread_mutex_lock(&dns_jobs_mutex);
        struct addrinfo *result = s->dns_result;
        dns_job_remove_locked(s);
        s->dns_result              = NULL;
        s->dns_active              = false;
        s->dns_thread_started      = false;
        s->dns_completed           = false;
        s->dns_notification_failed = false;
        pthread_mutex_unlock(&dns_jobs_mutex);

        if (result != NULL) {
            freeaddrinfo(result);
        }
        s->resolving = false;
        socks5_release(s); /* referencia propia del job DNS */
    }
}

/* --- CONNECT --- */

static unsigned
request_connect(struct selector_key *key)
{
    struct socks5 *s = ATTACHMENT(key);

    s->origin_fd = socket(s->origin_addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s->origin_fd == -1 || selector_fd_set_nio(s->origin_fd) == -1) {
        return request_connect_fail(key, errno);
    }

    const int r = connect(s->origin_fd, (struct sockaddr *)&s->origin_addr,
                          s->origin_addr_len);
    if (r == 0 || errno == EINPROGRESS) {
        if (socks5_register_origin(key, s) != SELECTOR_SUCCESS) {
            return request_connect_fail(key, EIO);
        }
        if (r == 0) {
            s->reply = REPLY_SUCCEEDED;
            return request_reply(key);
        }
        selector_set_interest(key->s, s->client_fd, OP_NOOP);
        selector_set_interest(key->s, s->origin_fd, OP_WRITE);
        return REQUEST_CONNECTING;
    }
    return request_connect_fail(key, errno);
}

static unsigned
request_connect_fail(struct selector_key *key, int err)
{
    struct socks5 *s = ATTACHMENT(key);
    if (s->origin_fd != -1) {
        close(s->origin_fd);
        s->origin_fd = -1;
    }
    if (request_pick_next(s)) {  /* RF4: probar la siguiente IP */
        return request_connect(key);
    }
    s->reply = reply_from_errno(err);
    return request_reply(key);
}

unsigned
request_connecting_on_write_ready(struct selector_key *key)
{
    struct socks5 *s = ATTACHMENT(key);

    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(key->fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1) {
        err = errno;
    }
    if (err == 0) {
        s->reply = REPLY_SUCCEEDED;
        return request_reply(key);
    }
    /* este intento falló: liberar el fd y probar la próxima IP (RF4) */
    selector_unregister_fd(key->s, key->fd); /* dispara handle_close: references-- */
    close(s->origin_fd);
    s->origin_fd = -1;
    if (request_pick_next(s)) {
        return request_connect(key);
    }
    s->reply = reply_from_errno(err);
    return request_reply(key);
}

/* Construye el reply y deja al cliente listo para recibirlo. */
static unsigned
request_reply(struct selector_key *key)
{
    struct socks5 *s = ATTACHMENT(key);
    request_build_reply(s);
    if (s->origin_fd != -1) {
        selector_set_interest(key->s, s->origin_fd, OP_NOOP);
    }
    selector_set_interest(key->s, s->client_fd, OP_WRITE);
    return REQUEST_WRITE;
}

unsigned
request_write_on_write_ready(struct selector_key *key)
{
    struct socks5 *s = ATTACHMENT(key);

    const int st = socks_send(key, &s->write_buffer);
    if (st == -1) {
        return ERROR;
    }
    if (st == 0) {
        return REQUEST_WRITE;
    }
    return (s->reply == REPLY_SUCCEEDED) ? COPY : DONE;
}
