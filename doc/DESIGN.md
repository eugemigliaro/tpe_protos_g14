# Documento de diseño

Arquitectura de la aplicación y de sus máquinas de estados.

## Arquitectura general

Event loop **no bloqueante en un solo thread**: un socket pasivo SOCKS5 y, por cada cliente, dos
file descriptors (cliente y origin) multiplexados con el `selector` de la cátedra. Cada conexión
tiene su propio estado (`struct socks5`) y una máquina de estados (`stm`). La resolución de nombres
es la única tarea que se descarga a un thread aparte (lo permite la consigna).

Separación de responsabilidades por archivo:

| Archivo | Responsabilidad |
|---------|-----------------|
| `server.c` | Levanta el socket pasivo y corre el event loop. |
| `socks5.c` | Ciclo de vida de la conexión, pool, handlers del selector, tabla de estados. |
| `states/hello.c` | Negociación de método (RFC1928). |
| `states/auth.c` | Autenticación usuario/contraseña (RFC1929). |
| `states/request.c` | Parseo del request, resolución, connect e iteración de IPs, reply codes. |
| `states/relay.c` | Copia bidireccional con parciales y medio cierre. |
| `states/states_common.c` | Helpers de recv/send sobre `buffer`. |

## Máquina de estados SOCKS5

```
HELLO_READ ──► HELLO_WRITE ──┬──► AUTH_READ ──► AUTH_WRITE ──► REQUEST_READ
                             └──(sin auth)──────────────────►  REQUEST_READ
                             └──(0xFF)──► DONE

REQUEST_READ ──(IPv4/IPv6)──────────────► (connect)
             └─(FQDN)─► REQUEST_RESOLV ──► (connect)

(connect) ──► REQUEST_CONNECTING ──► REQUEST_WRITE ──(reply ok)──► COPY ──► DONE
                                                   └─(reply err)──► DONE
```

| Estado | Evento | Acción / transición |
|--------|--------|---------------------|
| `HELLO_READ` | read | parsea VER/NMETHODS/METHODS; elige método → `HELLO_WRITE` |
| `HELLO_WRITE` | write | envía método; → `AUTH_READ` / `REQUEST_READ` / `DONE` (0xFF) |
| `AUTH_READ` | read | valida user/pass contra `args`; → `AUTH_WRITE` |
| `AUTH_WRITE` | write | envía status; ok → `REQUEST_READ`, falla → `DONE` |
| `REQUEST_READ` | read | parsea CMD/ATYP/DST; IP → connect, FQDN → `REQUEST_RESOLV` |
| `REQUEST_RESOLV` | block | `getaddrinfo` en thread; al volver → connect |
| `REQUEST_CONNECTING` | write | `SO_ERROR`; ok → reply, falla → próxima IP (RF4) |
| `REQUEST_WRITE` | write | envía reply al cliente; ok → `COPY`, error → `DONE` |
| `COPY` | read/write | relay bidireccional; ambos sentidos cerrados → `DONE` |

## Estado por conexión

`struct socks5` (ver `socks5.h`): fds de cliente y origin, `sockaddr` de cada uno, lista de
resolución (`getaddrinfo`) con iterador para RF4, reply a enviar, flags de medio cierre, máquina de
estados, parsers de cada etapa (en `union`, son excluyentes en el tiempo) y dos buffers:

- `read_buffer`: bytes que vienen del **cliente** (van al origin).
- `write_buffer`: bytes que van hacia el **cliente** (vienen del origin).

Las estructuras se reciclan en un pool para evitar `malloc`/`free` por conexión. Un contador de
referencias libera la estructura recién cuando se cierran sus dos fds.

## Resolución de nombres (RF3/RF4) y robustez

Para FQDN se lanza un thread que solo corre `getaddrinfo` y despierta al thread principal con
`selector_notify_block`. El resultado es una lista de IPs (IPv4/IPv6); `REQUEST_CONNECTING` prueba
una a una hasta que alguna conecte (RF4). Cada causa de fallo se mapea a su reply code SOCKS5 (RF5):
`ECONNREFUSED`→0x05, `ENETUNREACH`→0x03, `EHOSTUNREACH`→0x04, `ETIMEDOUT`→0x06, resto→0x01.

## Lecturas/escrituras parciales (C4)

Todo el I/O usa el `buffer` de la cátedra: se lee/escribe lo que el kernel permita y lo pendiente
queda para el próximo evento. En `COPY` los intereses de cada fd se recalculan según haya datos por
leer/escribir y se hace `SHUT_WR` del par cuando un lado manda EOF y se vacía su buffer en tránsito.

---

## Canal de monitoreo (MNG/1)

### Propósito y transporte

El servidor escucha en un segundo puerto TCP (por defecto `127.0.0.1:8080`, flag `-P`) para
conexiones de administración. El protocolo binario **MNG/1** (especificado en `doc/SPEC.md`) opera
sobre este canal con I/O **no bloqueante** multiplexada en el mismo event loop que SOCKS5: no hay
thread ni socket bloqueante adicional. El socket pasivo de monitoreo se registra en el selector
exactamente igual que el SOCKS5.

### Límite, tracking y pool de conexiones (`struct mng_conn`)

`monitor.c` admite hasta `MNG_MAX_CONNECTIONS = 16` sesiones simultáneas y mantiene una lista
doblemente enlazada para contarlas, aplicar timeouts y apagarlas ordenadamente. Al cerrar una
sesión, conserva hasta `MNG_POOL_MAX = 16` structs reutilizables en un free-list. Campos principales:

| Campo | Tipo | Descripción |
|-------|------|-------------|
| `fd` | `int` | Descriptor de la conexión de administración |
| `stm` | `struct state_machine` | Máquina de estados MNG (6 estados) |
| `authenticated` | `bool` | `true` tras handshake exitoso |
| `close_after_write` | `bool` | Si `true`, cierra la conexión al vaciar el write buffer |
| `shutting_down` | `bool` | Cierra al terminar una respuesta pendiente durante shutdown |
| `last_activity` | `time_t` | Última actividad para el timeout configurable |
| `read_buffer` | `buffer` | Entrada; tamaño `MNG_READ_SIZE = 2048` bytes |
| `write_buffer` | `buffer` | Salida; tamaño `MNG_WRITE_SIZE = 65536` bytes (64 KB) |
| `auth_parser`, `cmd_parser` | structs | Parsers del handshake y de comandos |
| `pool_next` | `struct mng_conn *` | Free-list del pool |
| `active_prev`, `active_next` | `struct mng_conn *` | Lista de sesiones activas |

El write buffer es 64 KB para acomodar el peor caso de `GET_LOG`:
1 + 2 + 100 × (2 + 511) = 51.301 bytes.

`close_after_write` se activa en `CMD_CLOSE`. Una autenticación fallida cierra al terminar su
respuesta mediante `authenticated == false`; durante el apagado, `shutting_down` permite vaciar una
respuesta ya preparada antes de cerrar.

### Máquina de estados del canal de monitoreo

```
MNG_AUTH_READ ──► MNG_AUTH_WRITE ──► MNG_CMD_READ ──► MNG_RESP_WRITE ──┐
                                           ▲                            │ (!close_after_write)
                                           └────────────────────────────┘
                                           │ (close_after_write o CMD_CLOSE)
                                           ▼
                                      MNG_DONE / MNG_ERROR
```

| Estado | Evento | Acción |
|--------|--------|--------|
| `MNG_AUTH_READ` | read | Parsea VER+ULEN+UNAME+PLEN+PASSWD; valida contra credencial admin |
| `MNG_AUTH_WRITE` | write | Envía VER(1)+STATUS(1); ok → `MNG_CMD_READ`, falla → `MNG_DONE` |
| `MNG_CMD_READ` | read | Parsea comando y parámetros con `cmd_feed()`; ejecuta con `exec_command()` |
| `MNG_RESP_WRITE` | write | Escribe respuesta; si `!close_after_write` vuelve a `MNG_CMD_READ` |
| `MNG_DONE` | — | Conexión terminada normalmente |
| `MNG_ERROR` | — | Terminación por error de I/O o protocolo |

La vuelta `MNG_RESP_WRITE → MNG_CMD_READ` permite **múltiples comandos por sesión** sin
desconectarse. Los buffers se resetean con `buffer_reset()` de forma segura porque en
`MNG_RESP_WRITE` el write buffer está completamente vaciado y en `MNG_CMD_READ` el read buffer
está completamente consumido.

### Parsers (byte a byte)

Ambos parsers son STM hand-rolled sin allocaciones; avanzan byte a byte sobre el `buffer` de entrada:

**`auth_feed()`** — STM interno:
```
VER → ULEN → UNAME[0..ulen-1] → PLEN → PASSWD[0..plen-1] → DONE / ERROR
```

**`cmd_feed()`** — STM interno, ramificado según el tipo de comando:
```
CMD ──(ADD_USER)──────────────► ULEN → UNAME → PLEN → PASSWD → DONE
    ──(DEL_USER)──────────────► ULEN → UNAME → DONE
    ──(SET_TIMEOUT)───────────► TIMEOUT[4 bytes big-endian] → DONE
    ──(GET_STATS / LIST_USERS / GET_LOG / CLOSE)──────────► DONE  (sin parámetros)
```

### Ejecución de comandos (`exec_command`)

| Comando | Byte | API del servidor | Respuesta wire |
|---------|------|-----------------|----------------|
| `ADD_USER` | 0x01 | `socks5_add_user()` | STATUS(1) |
| `DEL_USER` | 0x02 | `socks5_del_user()` | STATUS(1) |
| `LIST_USERS` | 0x03 | `socks5_list_users()` | STATUS(1)+COUNT(1)+[ULEN(1)+UNAME]* |
| `GET_STATS` | 0x04 | `metrics_get()` | STATUS(1)+HIST(8)+CURR(4)+SENT(8)+RECV(8) |
| `SET_TIMEOUT` | 0x05 | timeout compartido SOCKS5/MNG | STATUS(1) |
| `GET_LOG` | 0x06 | `access_log_get_recent()` | STATUS(1)+COUNT(2)+[ELEN(2)+ENTRY]* |
| `CLOSE` | 0x07 | — | STATUS(1); activa `close_after_write` |

Todos los campos de más de un byte se codifican en **big-endian** (network byte order).

### Métricas y access log

Las métricas se guardan en variables globales en `metrics.c`:
- `hist_connections` (`uint64_t`): conexiones totales históricas.
- `curr_connections` (`uint32_t`): conexiones activas en este momento.
- `bytes_sent` / `bytes_recv` (`uint64_t`): bytes transferidos en el relay.

El access log (`access_log.c`) escribe una línea por conexión exitosa al archivo `socks5_access.log`
(en el directorio de trabajo). Un búfer circular en RAM retiene las últimas 100 entradas para `GET_LOG`.

### Cliente de monitoreo (`src/client/client.c`)

Usa **I/O bloqueante** (permitido por RNF5). Sus primitivas `read_all()` / `write_all()` garantizan
lectura/escritura exacta de N bytes. Subcomandos: `stats`, `users`, `add-user`, `del-user`,
`set-timeout`, `log`. Flags: `-L <ip>` (default 127.0.0.1), `-P <puerto>` (default 8080),
`-A <user:pass>`, `-v`, `-h`.
