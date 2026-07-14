/**
 * monitor.c - canal de monitoreo MNG/1 (RF7).
 *
 * Pool de conexiones + máquina de estados por conexión:
 *   MNG_AUTH_READ → MNG_AUTH_WRITE → MNG_CMD_READ ⇆ MNG_RESP_WRITE → MNG_DONE
 *
 * El I/O es completamente no bloqueante; se multiplexa en el mismo selector
 * que el tráfico SOCKS5. No hay fd "origin": una sola referencia por conn.
 */
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "access_log.h"
#include "metrics.h"
#include "mng_proto.h"
#include "monitor.h"
#include "socks5.h"

/* macOS no define MSG_NOSIGNAL; ignoramos SIGPIPE en main como respaldo. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* -------------------------------------------------------------------------
 * Pool y configuración
 * ---------------------------------------------------------------------- */

static struct mng_conn     *pool      = NULL;
static unsigned             pool_size = 0;
static const struct socks5args *mng_cfg = NULL;
static struct mng_conn     *active_list  = NULL;
static unsigned             active_count = 0;
static time_t               idle_timeout = SOCKS5_IDLE_TIMEOUT;

void
mng_init(const struct socks5args *args)
{
    mng_cfg = args;
}

static void
active_add(struct mng_conn *c)
{
    c->active_prev = NULL;
    c->active_next = active_list;
    if (active_list != NULL) {
        active_list->active_prev = c;
    }
    active_list = c;
    c->active = true;
    active_count++;
}

static void
active_remove(struct mng_conn *c)
{
    if (c->active_prev != NULL) {
        c->active_prev->active_next = c->active_next;
    } else {
        active_list = c->active_next;
    }
    if (c->active_next != NULL) {
        c->active_next->active_prev = c->active_prev;
    }
    c->active_prev = c->active_next = NULL;
    c->active = false;
    active_count--;
}

unsigned
mng_active_connections(void)
{
    return active_count;
}

static struct mng_conn *
mng_new(int fd)
{
    struct mng_conn *c;
    if (pool != NULL) {
        c = pool;
        pool = pool->pool_next;
        pool_size--;
    } else {
        c = malloc(sizeof(*c));
        if (c == NULL) {
            return NULL;
        }
    }
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->last_activity = time(NULL);
    buffer_init(&c->read_buffer,  MNG_READ_SIZE,  c->raw_read);
    buffer_init(&c->write_buffer, MNG_WRITE_SIZE, c->raw_write);
    return c;
}

static void
mng_destroy(struct mng_conn *c)
{
    if (c == NULL) {
        return;
    }
    if (c->active) {
        active_remove(c);
    }
    if (pool_size < MNG_POOL_MAX) {
        c->pool_next = pool;
        pool         = c;
        pool_size++;
    } else {
        free(c);
    }
}

void
mng_pool_destroy(void)
{
    while (pool != NULL) {
        struct mng_conn *next = pool->pool_next;
        free(pool);
        pool = next;
    }
    pool_size = 0;
}

/* -------------------------------------------------------------------------
 * Parsers internos
 * ---------------------------------------------------------------------- */

static void
auth_feed(struct mng_auth_parser *p, uint8_t b)
{
    switch (p->state) {
        case MNG_AUTH_P_VER:
            p->state = (b == MNG_VERSION) ? MNG_AUTH_P_ULEN : MNG_AUTH_P_ERROR;
            break;
        case MNG_AUTH_P_ULEN:
            p->ulen  = b;
            p->idx   = 0;
            p->state = (b == 0) ? MNG_AUTH_P_PLEN : MNG_AUTH_P_UNAME;
            break;
        case MNG_AUTH_P_UNAME:
            p->uname[p->idx++] = (char)b;
            if (p->idx == p->ulen) {
                p->uname[p->idx] = '\0';
                p->state = MNG_AUTH_P_PLEN;
            }
            break;
        case MNG_AUTH_P_PLEN:
            p->plen = b;
            p->idx  = 0;
            if (b == 0) {
                p->passwd[0] = '\0';
                p->state = MNG_AUTH_P_DONE;
            } else {
                p->state = MNG_AUTH_P_PASSWD;
            }
            break;
        case MNG_AUTH_P_PASSWD:
            p->passwd[p->idx++] = (char)b;
            if (p->idx == p->plen) {
                p->passwd[p->idx] = '\0';
                p->state = MNG_AUTH_P_DONE;
            }
            break;
        default:
            break;
    }
}

static void
cmd_feed(struct mng_cmd_parser *p, uint8_t b)
{
    switch (p->state) {
        case MNG_CMD_P_CMD:
            p->cmd = b;
            switch (b) {
                case MNG_CMD_ADD_USER:
                case MNG_CMD_DEL_USER:
                    p->state = MNG_CMD_P_ULEN;
                    break;
                case MNG_CMD_SET_TIMEOUT:
                    p->timeout = 0;
                    p->idx     = 0;
                    p->state   = MNG_CMD_P_TIMEOUT;
                    break;
                case MNG_CMD_LIST_USERS:
                case MNG_CMD_GET_STATS:
                case MNG_CMD_GET_LOG:
                case MNG_CMD_CLOSE:
                    p->state = MNG_CMD_P_DONE;
                    break;
                default:
                    p->state = MNG_CMD_P_ERROR;
                    break;
            }
            break;
        case MNG_CMD_P_ULEN:
            p->ulen = b;
            p->idx  = 0;
            if (b == 0) {
                p->uname[0] = '\0';
                p->state = (p->cmd == MNG_CMD_ADD_USER) ? MNG_CMD_P_PLEN
                                                         : MNG_CMD_P_DONE;
            } else {
                p->state = MNG_CMD_P_UNAME;
            }
            break;
        case MNG_CMD_P_UNAME:
            p->uname[p->idx++] = (char)b;
            if (p->idx == p->ulen) {
                p->uname[p->idx] = '\0';
                p->state = (p->cmd == MNG_CMD_ADD_USER) ? MNG_CMD_P_PLEN
                                                         : MNG_CMD_P_DONE;
            }
            break;
        case MNG_CMD_P_PLEN:
            p->plen = b;
            p->idx  = 0;
            if (b == 0) {
                p->passwd[0] = '\0';
                p->state = MNG_CMD_P_DONE;
            } else {
                p->state = MNG_CMD_P_PASSWD;
            }
            break;
        case MNG_CMD_P_PASSWD:
            p->passwd[p->idx++] = (char)b;
            if (p->idx == p->plen) {
                p->passwd[p->idx] = '\0';
                p->state = MNG_CMD_P_DONE;
            }
            break;
        case MNG_CMD_P_TIMEOUT:
            p->timeout = (p->timeout << 8) | b;
            if (++p->idx == 4) {
                p->state = MNG_CMD_P_DONE;
            }
            break;
        default:
            break;
    }
}

/* -------------------------------------------------------------------------
 * Construcción de respuestas
 * ---------------------------------------------------------------------- */

static void
write_u64(buffer *b, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        buffer_write(b, (uint8_t)((v >> (i * 8)) & 0xFF));
    }
}

static void
write_u32(buffer *b, uint32_t v)
{
    for (int i = 3; i >= 0; i--) {
        buffer_write(b, (uint8_t)((v >> (i * 8)) & 0xFF));
    }
}

static void
write_u16(buffer *b, uint16_t v)
{
    buffer_write(b, (uint8_t)((v >> 8) & 0xFF));
    buffer_write(b, (uint8_t)(v & 0xFF));
}

static void
exec_command(struct mng_conn *c)
{
    struct mng_cmd_parser *p = &c->cmd_parser;
    buffer *b = &c->write_buffer;

    buffer_reset(b);

    switch (p->cmd) {

        case MNG_CMD_ADD_USER: {
            const int r = socks5_add_user(p->uname, p->passwd);
            if (r == 0)       buffer_write(b, MNG_STATUS_OK);
            else if (r == -1) buffer_write(b, MNG_STATUS_FULL);
            else              buffer_write(b, MNG_STATUS_NOT_FOUND);
            break;
        }

        case MNG_CMD_DEL_USER: {
            const int r = socks5_del_user(p->uname);
            buffer_write(b, r == 0 ? MNG_STATUS_OK : MNG_STATUS_NOT_FOUND);
            break;
        }

        case MNG_CMD_LIST_USERS: {
            char names[MAX_USERS][256];
            const int count = socks5_list_users(names, MAX_USERS);
            buffer_write(b, MNG_STATUS_OK);
            buffer_write(b, (uint8_t)count);
            for (int i = 0; i < count; i++) {
                const uint8_t ulen = (uint8_t)strlen(names[i]);
                buffer_write(b, ulen);
                for (uint8_t j = 0; j < ulen; j++) {
                    buffer_write(b, (uint8_t)names[i][j]);
                }
            }
            break;
        }

        case MNG_CMD_GET_STATS: {
            struct metrics_snapshot m = metrics_get();
            buffer_write(b, MNG_STATUS_OK);
            write_u64(b, m.hist_connections);
            write_u32(b, m.curr_connections);
            write_u64(b, m.bytes_sent);
            write_u64(b, m.bytes_recv);
            break;
        }

        case MNG_CMD_SET_TIMEOUT: {
            socks5_set_timeout(p->timeout);
            idle_timeout = (time_t)p->timeout;
            buffer_write(b, MNG_STATUS_OK);
            break;
        }

        case MNG_CMD_GET_LOG: {
            const char *entries[LOG_RECENT_MAX];
            const int count = access_log_get_recent(entries, LOG_RECENT_MAX);
            buffer_write(b, MNG_STATUS_OK);
            write_u16(b, (uint16_t)count);
            for (int i = 0; i < count; i++) {
                const uint16_t elen = (uint16_t)strlen(entries[i]);
                write_u16(b, elen);
                for (uint16_t j = 0; j < elen; j++) {
                    buffer_write(b, (uint8_t)entries[i][j]);
                }
            }
            break;
        }

        case MNG_CMD_CLOSE:
            buffer_write(b, MNG_STATUS_OK);
            c->close_after_write = true;
            break;

        default:
            buffer_write(b, MNG_STATUS_BAD_ARGS);
            break;
    }
}

/* -------------------------------------------------------------------------
 * Máquina de estados — declaraciones adelantadas
 * ---------------------------------------------------------------------- */

static void     mng_auth_read_on_arrival(const unsigned state,
                                         struct selector_key *key);
static unsigned mng_auth_read_on_read_ready(struct selector_key *key);
static unsigned mng_auth_write_on_write_ready(struct selector_key *key);
static void     mng_cmd_read_on_arrival(const unsigned state,
                                        struct selector_key *key);
static unsigned mng_cmd_read_on_read_ready(struct selector_key *key);
static unsigned mng_resp_write_on_write_ready(struct selector_key *key);

static const struct state_definition mng_state_defs[] = {
    [MNG_AUTH_READ] = {
        .state         = MNG_AUTH_READ,
        .on_arrival    = mng_auth_read_on_arrival,
        .on_read_ready = mng_auth_read_on_read_ready,
    },
    [MNG_AUTH_WRITE] = {
        .state          = MNG_AUTH_WRITE,
        .on_write_ready = mng_auth_write_on_write_ready,
    },
    [MNG_CMD_READ] = {
        .state         = MNG_CMD_READ,
        .on_arrival    = mng_cmd_read_on_arrival,
        .on_read_ready = mng_cmd_read_on_read_ready,
    },
    [MNG_RESP_WRITE] = {
        .state          = MNG_RESP_WRITE,
        .on_write_ready = mng_resp_write_on_write_ready,
    },
    [MNG_DONE]  = { .state = MNG_DONE  },
    [MNG_ERROR] = { .state = MNG_ERROR },
};

/* --- MNG_AUTH_READ --- */

static void
mng_auth_read_on_arrival(const unsigned state, struct selector_key *key)
{
    (void)state;
    struct mng_conn *c = MNG_ATTACHMENT(key);
    memset(&c->auth_parser, 0, sizeof(c->auth_parser));
    c->auth_parser.state = MNG_AUTH_P_VER;
    selector_set_interest_key(key, OP_READ);
}

static unsigned
mng_auth_read_on_read_ready(struct selector_key *key)
{
    struct mng_conn        *c = MNG_ATTACHMENT(key);
    struct mng_auth_parser *p = &c->auth_parser;

    size_t   space;
    uint8_t *ptr = buffer_write_ptr(&c->read_buffer, &space);
    if (space == 0) {
        return MNG_ERROR;
    }
    const ssize_t n = recv(key->fd, ptr, space, 0);
    if (n == 0) {
        return MNG_DONE;
    }
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? MNG_AUTH_READ
                                                         : MNG_ERROR;
    }
    buffer_write_adv(&c->read_buffer, n);

    while (buffer_can_read(&c->read_buffer) &&
           p->state != MNG_AUTH_P_DONE && p->state != MNG_AUTH_P_ERROR) {
        auth_feed(p, buffer_read(&c->read_buffer));
    }

    if (p->state == MNG_AUTH_P_ERROR) {
        buffer_write(&c->write_buffer, MNG_VERSION);
        buffer_write(&c->write_buffer, MNG_AUTH_VER_UNSUPPORTED);
        c->authenticated = false;
        selector_set_interest_key(key, OP_WRITE);
        return MNG_AUTH_WRITE;
    }
    if (p->state != MNG_AUTH_P_DONE) {
        return MNG_AUTH_READ;
    }

    bool ok = false;
    if (mng_cfg != NULL && mng_cfg->admin.name != NULL &&
        mng_cfg->admin.pass != NULL &&
        strcmp(p->uname,  mng_cfg->admin.name) == 0 &&
        strcmp(p->passwd, mng_cfg->admin.pass) == 0) {
        ok = true;
    }

    buffer_write(&c->write_buffer, MNG_VERSION);
    buffer_write(&c->write_buffer, ok ? MNG_AUTH_OK : MNG_AUTH_FAIL);
    c->authenticated = ok;
    selector_set_interest_key(key, OP_WRITE);
    return MNG_AUTH_WRITE;
}

/* --- MNG_AUTH_WRITE --- */

static unsigned
mng_auth_write_on_write_ready(struct selector_key *key)
{
    struct mng_conn *c = MNG_ATTACHMENT(key);

    size_t   count;
    uint8_t *ptr = buffer_read_ptr(&c->write_buffer, &count);
    const ssize_t n = send(key->fd, ptr, count, MSG_NOSIGNAL);
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? MNG_AUTH_WRITE
                                                         : MNG_ERROR;
    }
    buffer_read_adv(&c->write_buffer, n);

    if (buffer_can_read(&c->write_buffer)) {
        return MNG_AUTH_WRITE;
    }
    if (c->shutting_down) {
        return MNG_DONE;
    }
    if (!c->authenticated) {
        return MNG_DONE;
    }
    selector_set_interest_key(key, OP_READ);
    return MNG_CMD_READ;
}

/* --- MNG_CMD_READ --- */

static void
mng_cmd_read_on_arrival(const unsigned state, struct selector_key *key)
{
    (void)state;
    struct mng_conn *c = MNG_ATTACHMENT(key);
    memset(&c->cmd_parser, 0, sizeof(c->cmd_parser));
    c->cmd_parser.state  = MNG_CMD_P_CMD;
    c->close_after_write = false;
    buffer_reset(&c->read_buffer);
    buffer_reset(&c->write_buffer);
    selector_set_interest_key(key, OP_READ);
}

static unsigned
mng_cmd_read_on_read_ready(struct selector_key *key)
{
    struct mng_conn       *c = MNG_ATTACHMENT(key);
    struct mng_cmd_parser *p = &c->cmd_parser;

    size_t   space;
    uint8_t *ptr = buffer_write_ptr(&c->read_buffer, &space);
    if (space == 0) {
        return MNG_ERROR;
    }
    const ssize_t n = recv(key->fd, ptr, space, 0);
    if (n == 0) {
        return MNG_DONE;
    }
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? MNG_CMD_READ
                                                         : MNG_ERROR;
    }
    buffer_write_adv(&c->read_buffer, n);

    while (buffer_can_read(&c->read_buffer) &&
           p->state != MNG_CMD_P_DONE && p->state != MNG_CMD_P_ERROR) {
        cmd_feed(p, buffer_read(&c->read_buffer));
    }

    if (p->state == MNG_CMD_P_ERROR) {
        buffer_reset(&c->write_buffer);
        buffer_write(&c->write_buffer, MNG_STATUS_BAD_ARGS);
        selector_set_interest_key(key, OP_WRITE);
        return MNG_RESP_WRITE;
    }
    if (p->state != MNG_CMD_P_DONE) {
        return MNG_CMD_READ;
    }

    exec_command(c);
    selector_set_interest_key(key, OP_WRITE);
    return MNG_RESP_WRITE;
}

/* --- MNG_RESP_WRITE --- */

static unsigned
mng_resp_write_on_write_ready(struct selector_key *key)
{
    struct mng_conn *c = MNG_ATTACHMENT(key);

    size_t   count;
    uint8_t *ptr = buffer_read_ptr(&c->write_buffer, &count);
    const ssize_t n = send(key->fd, ptr, count, MSG_NOSIGNAL);
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? MNG_RESP_WRITE
                                                         : MNG_ERROR;
    }
    buffer_read_adv(&c->write_buffer, n);

    if (buffer_can_read(&c->write_buffer)) {
        return MNG_RESP_WRITE;
    }
    if (c->close_after_write || c->shutting_down) {
        return MNG_DONE;
    }
    selector_set_interest_key(key, OP_READ);
    return MNG_CMD_READ;
}

/* -------------------------------------------------------------------------
 * Handlers del selector y accept pasivo
 * ---------------------------------------------------------------------- */

static void
mng_done(struct selector_key *key)
{
    struct mng_conn *c  = MNG_ATTACHMENT(key);
    const int        fd = c->fd;
    selector_unregister_fd(key->s, fd);
    close(fd);
}

static void
mng_read(struct selector_key *key)
{
    struct mng_conn *c = MNG_ATTACHMENT(key);
    c->last_activity = time(NULL);
    const enum mng_state st =
        (enum mng_state)stm_handler_read(&c->stm, key);
    if (st == MNG_DONE || st == MNG_ERROR) {
        mng_done(key);
    }
}

static void
mng_write(struct selector_key *key)
{
    struct mng_conn *c = MNG_ATTACHMENT(key);
    c->last_activity = time(NULL);
    const enum mng_state st =
        (enum mng_state)stm_handler_write(&c->stm, key);
    if (st == MNG_DONE || st == MNG_ERROR) {
        mng_done(key);
    }
}

static void
mng_close(struct selector_key *key)
{
    mng_destroy(MNG_ATTACHMENT(key));
}

static const struct fd_handler mng_handler = {
    .handle_read  = mng_read,
    .handle_write = mng_write,
    .handle_close = mng_close,
};

static void
mng_kill(fd_selector s, struct mng_conn *c)
{
    const int fd = c->fd;
    selector_unregister_fd(s, fd);
    close(fd);
}

void
mng_sweep_timeouts(fd_selector s, time_t now)
{
    struct mng_conn *cur = active_list;
    while (cur != NULL) {
        struct mng_conn *next = cur->active_next;
        if (idle_timeout > 0 && now >= cur->last_activity &&
            now - cur->last_activity >= idle_timeout) {
            mng_kill(s, cur);
        }
        cur = next;
    }
}

void
mng_begin_shutdown(fd_selector s)
{
    struct mng_conn *cur = active_list;
    while (cur != NULL) {
        struct mng_conn *next = cur->active_next;
        const unsigned state = stm_state(&cur->stm);
        if ((state == MNG_AUTH_WRITE || state == MNG_RESP_WRITE) &&
            buffer_can_read(&cur->write_buffer)) {
            cur->shutting_down = true;
            if (selector_set_interest(s, cur->fd, OP_WRITE) !=
                SELECTOR_SUCCESS) {
                mng_kill(s, cur);
            }
        } else {
            mng_kill(s, cur);
        }
        cur = next;
    }
}

void
mng_force_close_all(fd_selector s)
{
    while (active_list != NULL) {
        mng_kill(s, active_list);
    }
}

void
mng_passive_accept(struct selector_key *key)
{
    struct sockaddr_storage addr;
    socklen_t               addr_len = sizeof(addr);
    struct mng_conn        *c        = NULL;

    const int fd = accept(key->fd, (struct sockaddr *)&addr, &addr_len);
    if (fd < 0) {
        return;
    }
    if (active_count >= MNG_MAX_CONNECTIONS) {
        close(fd);
        return;
    }
    if (selector_fd_set_nio(fd) < 0) {
        goto fail;
    }
    c = mng_new(fd);
    if (c == NULL) {
        goto fail;
    }

    c->stm.initial   = MNG_AUTH_READ;
    c->stm.max_state = MNG_ERROR;
    c->stm.states    = mng_state_defs;
    stm_init(&c->stm);

    if (selector_register(key->s, fd, &mng_handler, OP_READ, c) !=
        SELECTOR_SUCCESS) {
        goto fail;
    }
    active_add(c);
    return;

fail:
    if (fd >= 0) {
        close(fd);
    }
    mng_destroy(c);
}
