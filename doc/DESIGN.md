# Documento de diseño

Arquitectura de la aplicación y máquina de estados. Ver `../tpe/apuntes-clases-tp.md` §9–§12.

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
