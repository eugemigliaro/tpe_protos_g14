# Informe — Trabajo Práctico Especial SOCKS5

**Materia:** Protocolos de Comunicación (72.07) — ITBA  
**Cuatrimestre:** 2026/1  
**Grupo:** 14  

---

## Índice

1. [Descripción de protocolos diseñados y aplicaciones desarrolladas](#1-descripción-de-protocolos-diseñados-y-aplicaciones-desarrolladas)
2. [Problemas encontrados](#2-problemas-encontrados)
3. [Limitaciones conocidas](#3-limitaciones-conocidas)
4. [Posibles extensiones](#4-posibles-extensiones)
5. [Conclusiones](#5-conclusiones)
6. [Ejemplos de prueba](#6-ejemplos-de-prueba)
7. [Guía de instalación](#7-guía-de-instalación)
8. [Instrucciones de configuración](#8-instrucciones-de-configuración)
9. [Ejemplos de configuración y monitoreo](#9-ejemplos-de-configuración-y-monitoreo)
10. [Documento de diseño](#10-documento-de-diseño)

---

## 1. Descripción de protocolos diseñados y aplicaciones desarrolladas

### 1.1. Servidor proxy SOCKS5

El servidor implementa el protocolo **SOCKS5 (RFC1928)** con autenticación usuario/contraseña
(**RFC1929**). Acepta conexiones TCP en el puerto configurado (por defecto 1080) y actúa como proxy
de nivel de transporte: el cliente negocia el método de autenticación, se autentica, envía un request
CONNECT indicando el destino, y el servidor establece la conexión al origin y comienza a relayar
tráfico bidireccional.

**Flujo de una conexión SOCKS5:**

```
Cliente → [TCP connect a 1080]
         [HELLO: VER=5, NMETHODS, METHODS]       → Servidor
         [HELLO reply: VER=5, METHOD=0x02]        ← Servidor
         [AUTH: VER=1, ULEN, UNAME, PLEN, PASSWD] → Servidor
         [AUTH reply: VER=1, STATUS=0x00]         ← Servidor
         [REQUEST: VER=5, CMD=1, ATYP, DST, PORT] → Servidor
         [REPLY: VER=5, REP, ..., BND_ADDR, PORT] ← Servidor
         [tráfico relay bidireccional]            ↔ Servidor ↔ Origin
```

Tipos de dirección soportados:
- `ATYP=0x01` (IPv4): connect directo.
- `ATYP=0x04` (IPv6): connect directo.
- `ATYP=0x03` (FQDN): resolución mediante un worker joinable que conserva una referencia propia,
  publica el resultado de forma sincronizada y permite iterar las IPs obtenidas (RF4).

Solo se soporta el comando `CONNECT` (0x01). `BIND` y `UDP ASSOCIATE` retornan reply `0x07`
(command not supported).

### 1.2. Protocolo de monitoreo MNG/1

MNG/1 es un protocolo binario propio diseñado para administrar el servidor en tiempo de ejecución.
Opera en un puerto TCP separado (por defecto 8080, escucha solo en `127.0.0.1`), con I/O no
bloqueante integrada al mismo event loop que SOCKS5.

**Handshake de autenticación:**

```
+-----+------+-------+------+--------+
| VER | ULEN | UNAME | PLEN | PASSWD |
+-----+------+-------+------+--------+
|  1  |  1   | ULEN  |  1   | PLEN   |  bytes
+-----+------+-------+------+--------+
```

El servidor responde `VER(1) + STATUS(1)`: `0x00` = ok, `0x01` = credenciales inválidas,
`0x02` = versión no soportada.

**Formato de comando:**

```
+-----+============+
| CMD | parámetros |
+-----+============+
|  1  |  variable  |  bytes
+-----+============+
```

Todos los campos multi-byte se codifican en big-endian. Los 7 comandos definidos son:

| CMD  | Nombre      | Parámetros                        | Respuesta                              |
|------|-------------|-----------------------------------|----------------------------------------|
| 0x01 | ADD_USER    | ULEN(1)+UNAME+PLEN(1)+PASSWD      | STATUS(1)                              |
| 0x02 | DEL_USER    | ULEN(1)+UNAME                     | STATUS(1)                              |
| 0x03 | LIST_USERS  | —                                 | STATUS(1)+COUNT(1)+[ULEN(1)+UNAME]*   |
| 0x04 | GET_STATS   | —                                 | STATUS(1)+HIST(8)+CURR(4)+SENT(8)+RECV(8) |
| 0x05 | SET_TIMEOUT | TIMEOUT(4)                        | STATUS(1)                              |
| 0x06 | GET_LOG     | —                                 | STATUS(1)+COUNT(2)+[ELEN(2)+ENTRY]*   |
| 0x07 | CLOSE       | —                                 | STATUS(1); cierra sesión               |

Códigos de STATUS: `0x00` = ok, `0x01` = no encontrado, `0x02` = tabla llena,
`0x03` = argumentos inválidos, `0xFF` = error interno.

La sesión es stateful: un cliente puede enviar múltiples comandos sin reconectar (el servidor
vuelve a esperar el siguiente comando tras cada respuesta), hasta recibir `CLOSE` o EOF. Se admiten
como máximo 16 sesiones MNG simultáneas y `SET_TIMEOUT` se aplica tanto a SOCKS5 como a MNG.

La especificación completa con ejemplos en hexdump está en `doc/SPEC.md`.

### 1.3. Cliente de monitoreo

Aplicación de línea de comandos (`bin/client`) que implementa el rol cliente de MNG/1 con I/O
bloqueante. Ofrece seis subcomandos que mapean directamente a los comandos del protocolo.

---

## 2. Problemas encontrados

### 2.1. Interrupciones de señal en pselect

El `selector` de la cátedra usa `pselect(2)`. Para que `SIGINT`/`SIGTERM` interrumpan el select
y el loop principal pueda reaccionar, fue necesario usar `sigaction` **sin `SA_RESTART`**.
Con `SA_RESTART`, el kernel reinicia automáticamente la llamada bloqueada y la señal nunca llega
al loop. La solución fue no setear ese flag: la señal causa `EINTR`, que el selector devuelve como
éxito, y el loop comprueba `shutdown_signals` en la iteración siguiente.

### 2.2. Cierre de conexiones tras escribir la respuesta de error

Al rechazar una autenticación MNG/1, el servidor necesita enviar la respuesta de fallo *y luego*
cerrar. Si se cierra el fd inmediatamente, el write buffer pendiente puede perderse. El estado
`MNG_AUTH_WRITE` vacía primero la respuesta y cierra cuando `authenticated == false`.
`close_after_write` cumple la misma función para `CMD_CLOSE`, y `shutting_down` para una respuesta
que ya estaba preparada al comenzar el apagado controlado.

### 2.3. Reuso de buffers entre comandos MNG

En una sesión multi-comando, los buffers de `mng_conn` deben reiniciarse entre comandos.
El write buffer queda completamente vaciado antes de volver a `MNG_CMD_READ`, y el read buffer
queda completamente consumido al ejecutar el comando. Llamar `buffer_reset()` en la transición
`MNG_RESP_WRITE → MNG_CMD_READ` reutiliza ambos buffers de forma segura sin riesgo de mezclar
datos de comandos distintos.

### 2.4. Tamaño del write buffer para GET_LOG

El peor caso de `GET_LOG` ocurre con 100 entradas de 511 bytes cada una:
`1 (STATUS) + 2 (COUNT) + 100 × (2 + 511) = 51.301 bytes`. El write buffer inicial de 4 KB
era insuficiente. Se aumentó a `MNG_WRITE_SIZE = 65536` (64 KB) para garantizar que toda la
respuesta cabe en el buffer y se envía en una sola pasada.

### 2.5. Guardado del fd antes de unregister en socks5_kill

Al barrer timeouts, `socks5_kill()` llama a `selector_unregister_fd()` que dispara el callback
`handle_close → socks5_destroy()`, que puede reciclar el struct al pool. Si luego se intentara
usar el fd guardado en el struct, el valor ya podría haber sido pisado. La solución fue guardar
los fds en variables locales antes de llamar a `unregister`.

### 2.6. Pipelining no soportado

El protocolo MNG/1 no soporta pipelining (enviar un comando antes de recibir la respuesta del
anterior). Esto simplifica el parser del servidor pero limita la latencia en clientes que necesiten
ejecutar muchos comandos seguidos. Se documentó como limitación en la SPEC.

### 2.7. Ciclo de vida de las resoluciones DNS

`getaddrinfo()` se ejecuta fuera del event loop para no bloquear el resto del servidor. El worker
DNS conserva una referencia sobre la conexión y publica únicamente resultado y status bajo mutex;
el thread principal consume esa publicación y continúa con `CONNECT`. Los workers son joinables y
el apagado espera los que sigan en vuelo antes de destruir el selector o los pools. Si falla la
notificación al selector, el loop principal detecta y reintenta el resultado pendiente.

---

## 3. Limitaciones conocidas

| Limitación | Impacto | Justificación |
|------------|---------|---------------|
| **FD_SETSIZE (~1024):** el selector usa `pselect(2)`. En Linux, cada conexión SOCKS5 activa usa 2 fds (cliente + origin), lo que limita a ~505–510 conexiones SOCKS5 simultáneas. | Cumple RF1 (≥500) con poco margen en Linux. | Reemplazar `pselect` por `epoll`/`kqueue` requeriría modificar código de cátedra. |
| **Solo IPv4 en el socket pasivo.** El servidor escucha en `0.0.0.0` (IPv4 únicamente). | Clientes IPv6 no pueden conectarse al proxy. | Extensión trivial (agregar socket pasivo IPv6). |
| **Solo comando CONNECT.** `BIND` y `UDP ASSOCIATE` retornan error `0x07`. | No soporta FTP activo ni aplicaciones UDP. | RFC1928 los define como opcionales. |
| **Credenciales en texto plano.** El canal MNG/1 no usa TLS. | Riesgo en redes no confiables. | Mitigado: el socket de monitoreo bindea a `127.0.0.1` por defecto. |
| **Sin pipelining en MNG/1.** El cliente debe esperar la respuesta antes de enviar el siguiente comando. | Latencia mayor en sesiones de muchos comandos. | Simplifica el parser; aceptable para uso administrativo. |
| **MAX_USERS = 10.** La tabla de usuarios está acotada en compilación. | No escala a decenas de usuarios. | Constante modificable con recompilación. |
| **Log no rotado.** `socks5_access.log` crece indefinidamente. | Riesgo de disco lleno en operación prolongada. | Se puede rotar con logrotate externamente. |

---

## 4. Posibles extensiones

- **IPv6 en el socket pasivo:** agregar un socket `AF_INET6` en `server.c` y registrarlo en el
  selector. La lógica de conexión al origin ya soporta IPv6 a través de `getaddrinfo`.
- **TLS en el canal de monitoreo:** envolver el socket MNG/1 con OpenSSL/mbedTLS para proteger
  credenciales e impedir sniffing en redes no confiables.
- **UDP ASSOCIATE:** implementar el tercer comando SOCKS5. Requiere sockets UDP por conexión y
  lógica de reenvío con la cabecera UDP SOCKS5.
- **Pipelining en MNG/1:** permitir múltiples comandos en vuelo. Requiere una cola de respuestas
  pendientes en el servidor.
- **Rotación y compresión del log:** implementar rotación interna cuando el archivo supere un umbral,
  o integrar con logrotate.
- **Disector de tráfico (sniffer POP3/HTTP):** interceptar el relay para extraer credenciales o
  URLs del tráfico en texto plano (previsto como segunda entrega en la consigna).
- **Pool de workers con epoll/kqueue:** reemplazar `pselect` por `epoll` (Linux) o `kqueue` (macOS)
  para superar el límite de `FD_SETSIZE` y escalar a decenas de miles de conexiones.

---

## 5. Conclusiones

El trabajo implementa un proxy SOCKS5 funcional que cumple todos los requisitos funcionales (RF1–RF9)
y no funcionales (RNF3, RNF5) establecidos en la consigna. El I/O no bloqueante se concentra en el
thread del event loop; la única excepción son los workers de resolución DNS. Estos no modifican la
máquina de estados directamente: publican su resultado de forma sincronizada para que lo consuma el
thread principal y mantienen viva la conexión mientras trabajan.

El protocolo de monitoreo MNG/1 es un protocolo binario completo con handshake de versión,
autenticación propia, sesión multi-comando y 7 operaciones de administración. Su integración en el
mismo event loop que SOCKS5 evita threads adicionales y la complejidad de sincronización asociada.

El punto de diseño más delicado fue la gestión del ciclo de vida de los file descriptors:
garantizar que un fd no se use después de que su struct fue reciclada al pool, y que el buffer de
escritura quede siempre vaciado antes de cerrar, requirió atención a los casos borde del event loop.

---

## 6. Ejemplos de prueba

### 6.1. Smoke test funcional

Entorno: macOS (Darwin 25.5.0), loopback, compilado con `cc -std=c11 -Wall -Wextra -pedantic`.

```
$ ./bin/server -p 1080 -P 8080 -u alice:secret -A admin:adminpass
1.0.0 escuchando SOCKS5 en 0.0.0.0:1080
monitoreo en 127.0.0.1:8080
```

**Tráfico SOCKS5 (servidor HTTP local en puerto 9999 como destino):**
```
$ curl -s --proxy socks5://alice:secret@127.0.0.1:1080 http://127.0.0.1:9999/ \
       -o /dev/null -w "HTTP %{http_code}\n"
HTTP 200
```

**Estadísticas tras el curl:**
```
$ ./bin/client -A admin:adminpass stats
Conexiones históricas: 1
Conexiones activas:    0
Bytes enviados:        933
Bytes recibidos:       77
```

**Gestión de usuarios:**
```
$ ./bin/client -A admin:adminpass users
Usuarios (1):
  alice

$ ./bin/client -A admin:adminpass add-user bob bobpass
OK

$ ./bin/client -A admin:adminpass users
Usuarios (2):
  alice
  bob

$ ./bin/client -A admin:adminpass del-user alice
OK

$ ./bin/client -A admin:adminpass users
Usuarios (1):
  bob
```

**Timeout y access log:**
```
$ ./bin/client -A admin:adminpass set-timeout 120
OK

$ ./bin/client -A admin:adminpass log
Entradas del log (1):
  2026-07-04T12:10:59Z alice 127.0.0.1:9999 OK

$ cat socks5_access.log
2026-07-04T12:10:43Z alice 127.0.0.1:9999 OK
2026-07-04T12:10:59Z alice 127.0.0.1:9999 OK
```

**Credencial incorrecta (debe fallar con exit 1):**
```
$ ./bin/client -A admin:wrong stats
autenticación fallida: credenciales inválidas
$ echo $?
1
```

### 6.2. Prueba B1 — Máximo de conexiones simultáneas

Se abrieron 1050 conexiones TCP al puerto SOCKS5 simultáneamente (sin autenticar,
estado `HELLO_READ`) usando un script Python:

```python
import socket, time
socks = []
for i in range(1050):
    s = socket.socket()
    s.connect(('127.0.0.1', 1080))
    s.setblocking(False)
    socks.append(s)
print(f'Conexiones abiertas: {len(socks)}')
# → Conexiones abiertas: 1050
```

**Resultado:** el servidor aceptó las 1050 conexiones sin errores.

En macOS, `pselect(2)` no está acotado por `FD_SETSIZE=1024` de la misma forma que en Linux,
y `raise_fd_limit()` eleva el límite de descriptores abiertos al máximo del sistema al inicio.
En Linux, el límite observado sería ~505–510 conexiones SOCKS5 con origin activo (2 fds cada una),
o ~1019 conexiones TCP en estado HELLO (1 fd cada una), por la fórmula:
`FD_SETSIZE - (stdin + stdout + stderr + socks_fd + mng_fd) = 1024 - 5 = 1019`.

### 6.3. Prueba B2 — Throughput

Se descargaron 3 archivos de 1 MB en paralelo a través del proxy (loopback):

```
$ ./bin/client -A admin:adminpass stats
Conexiones históricas: 3
Conexiones activas:    0
Bytes enviados:        3,146,340
Bytes recibidos:       264
```

- **Tiempo total:** 0.02 s
- **Datos transferidos:** 3 MB (3 × 1 MB)
- **Throughput observado:** ~138 MB/s

El proxy no introduce overhead apreciable en loopback. En un escenario real, el cuello de botella
sería la red o el origin server, no el proxy.

### 6.4. Pruebas del ciclo de vida DNS

Se verificaron resolución FQDN exitosa y negativa, fallo de creación del worker y fallo inyectado de
la notificación al selector. También se probó el apagado con resoluciones demoradas y 40 consultas
concurrentes. ASan+UBSan y TSan finalizaron sin diagnósticos después de sincronizar la publicación y
el acceso al thread del selector.

### 6.5. Prueba de lifecycle y capacidad MNG

El target `make test-stress` verifica por sockets reales el rechazo de la sesión MNG número 17, el
cierre por timeout, el apagado controlado y una carga mixta de 16 sesiones MNG autenticadas junto
con 500 conexiones SOCKS5 con `CONNECT` completo. Los cuatro casos finalizaron correctamente.

---

## 7. Guía de instalación

### Dependencias

- Compilador C11: GCC ≥ 9 o Clang ≥ 11
- POSIX threads (`-pthread`)
- Python 3 para ejecutar `make test-stress` (opcional)
- Sistema operativo: Linux o macOS

### Compilación

```bash
git clone https://github.com/eugemigliaro/tpe_protos_g14.git
cd tpe_protos_g14
make
make test-stress # prueba integrada de carga y lifecycle MNG
```

Los binarios se generan en `bin/`:
- `bin/server` — servidor proxy SOCKS5 + canal de monitoreo
- `bin/client` — cliente de monitoreo MNG/1

```bash
make clean   # elimina bin/ y obj/
```

---

## 8. Instrucciones de configuración

### Servidor (`bin/server`)

```
Opciones:
  -p <puerto>      Puerto SOCKS5 (default: 1080)
  -P <puerto>      Puerto de monitoreo (default: 8080)
  -l <dirección>   Dirección bind SOCKS5 (default: 0.0.0.0)
  -L <dirección>   Dirección bind monitoreo (default: 127.0.0.1)
  -u <user:pass>   Usuario SOCKS5 (repetible, máx. 10)
  -A <user:pass>   Credencial de administrador (canal de monitoreo)
  -N               Deshabilita disectores
  -v               Versión
  -h               Ayuda
```

**Notas:**
- El access log se escribe siempre como `socks5_access.log` en el directorio de trabajo.
- Sin `-u`: el proxy opera sin autenticación (método NO AUTH, 0x00).
- Sin `-A`: el canal de monitoreo rechaza todas las conexiones.
- El socket de monitoreo escucha en `127.0.0.1` por defecto; para exponerlo en otra interfaz
  usar `-L <ip>`.

### Cliente (`bin/client`)

```
Uso: ./bin/client [opciones] <subcomando> [args]

Opciones:
  -L <ip>         IP del servidor de monitoreo (default: 127.0.0.1)
  -P <puerto>     Puerto (default: 8080)
  -A <user:pass>  Credencial de administrador (obligatorio)
  -v              Versión
  -h              Ayuda

Subcomandos:
  stats                    Métricas del servidor
  users                    Listar usuarios SOCKS5
  add-user <user> <pass>   Agregar usuario
  del-user <user>          Eliminar usuario
  set-timeout <segundos>   Timeout de inactividad (0 = sin timeout)
  log                      Últimas entradas del access log (máx. 100)
```

---

## 9. Ejemplos de configuración y monitoreo

### Ejemplo mínimo (sin autenticación SOCKS5)

```bash
./bin/server -p 1080 -P 8080 -A admin:adminpass
# Cualquier cliente puede usar el proxy sin credenciales
curl --proxy socks5://127.0.0.1:1080 http://example.com
```

### Ejemplo con usuarios y monitoreo

```bash
./bin/server -p 1080 -P 8080 \
    -u alice:secret -u bob:pass2 \
    -A admin:adminpass
# El log queda en ./socks5_access.log
```

### Sesión completa de monitoreo

```bash
$ ./bin/client -A admin:adminpass stats
Conexiones históricas: 42
Conexiones activas:    3
Bytes enviados:        1048576
Bytes recibidos:       8192

$ ./bin/client -A admin:adminpass users
Usuarios (2):
  alice
  bob

$ ./bin/client -A admin:adminpass add-user carol carol123
OK

$ ./bin/client -A admin:adminpass del-user bob
OK

$ ./bin/client -A admin:adminpass set-timeout 120
OK

$ ./bin/client -A admin:adminpass set-timeout 0
OK

$ ./bin/client -A admin:adminpass log
Entradas del log (2):
  2026-07-04T12:00:00Z alice example.com:80 OK
  2026-07-04T12:01:30Z carol api.example.org:443 OK
```

### Apagado controlado

```bash
kill -SIGINT <pid>
# → no acepta nuevas conexiones; drena SOCKS y respuestas MNG pendientes

kill -SIGINT <pid>   # segunda señal
# → apagado forzado (N SOCKS y M monitoreo activas descartadas)
```

---

## 10. Documento de diseño

Ver [`doc/DESIGN.md`](DESIGN.md) para la descripción completa de la arquitectura interna, que incluye:

- **Arquitectura general:** event loop no bloqueante en un solo thread; selector de cátedra.
- **Máquina de estados SOCKS5** (11 estados: `HELLO_READ` → … → `COPY` → `DONE`).
- **Estado por conexión** (`struct socks5`): fds, parsers en union, buffers, pool con free-list,
  contador de referencias.
- **Resolución de nombres (RF3/RF4):** worker joinable con referencia propia, publicación
  sincronizada e iteración de IPs en `REQUEST_CONNECTING`.
- **Lecturas/escrituras parciales:** I/O con el buffer de cátedra; intereses del selector
  recalculados en cada evento.
- **Canal de monitoreo MNG/1:** máximo de 16 sesiones activas, pool `mng_conn` (16 slots), timeout,
  STM de 6 estados, ejecución de 7 comandos y cliente de monitoreo con I/O bloqueante.
