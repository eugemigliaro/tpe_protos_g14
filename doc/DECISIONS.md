# Decisiones de diseño

Decisiones técnicas de la implementación y su justificación.

## Transporte
- SOCKS5: **TCP** (lo fija RFC1928).
- Protocolo de monitoreo: **TCP**. Es orientado a conexión, con comandos/respuestas asociadas y
  sesión autenticada; TCP es lo natural (no hay que resolver pérdida/orden a mano).

## Formato del protocolo de monitoreo (texto vs binario) — binario
Elegimos **binario** con handshake de versión + campos `LEN+STRING` + códigos de status.
- **Por qué:** lo que se evalúa es "diseñar e implementar un protocolo"; un binario con framing por
  longitud es la evidencia más fuerte de esa competencia y obliga a manejar framing correctamente.
  La cátedra ya dio la plantilla (versión, METHOD, LEN+STRING, STATUS).
- Trade-off: menos legible que texto; se compensa documentando bien la SPEC y con ejemplos en hexdump.

## Framing
- Monitoreo: longitud explícita (`LEN+STRING`), sin separadores. Se leen exactamente `LEN` bytes.
- Handshake de versión al inicio (permite evolucionar sin romper compatibilidad).

## Pipelining
- El protocolo de monitoreo no admite pipelining: el cliente espera la respuesta de cada comando
  antes de enviar el siguiente. La restricción está definida en `SPEC.md`.

## Concurrencia e I/O — selector de cátedra
- Un único thread, I/O no bloqueante multiplexada con el **`selector` de la cátedra** (integrado vía
  los parches `git am`, atribuido en la historia git).
- Única excepción permitida: resolución de nombres fuera del thread principal (ver abajo).

## Resolución de nombres y robustez (RF4)
- `getaddrinfo` **fuera del thread principal** (thread dedicado que solo resuelve y despierta al main) para no bloquear el event loop.
- Robustez: ante un FQDN con múltiples IPs, **iterar toda la lista** y probar la siguiente si una
  falla. Cada resolución mantiene una referencia propia sobre la conexión; el shutdown espera los
  workers DNS antes de liberar el selector y los pools.

## Manejo de errores y reply codes (RF5)
- Usar **todos los reply codes** de SOCKS5 (no solo success / general failure), mapeando cada causa
  de fallo a su código: `ECONNREFUSED`→0x05, `ENETUNREACH`→0x03, `EHOSTUNREACH`→0x04,
  `ETIMEDOUT`→0x06, resto→0x01.

## Autenticación
- **Proxy:** usuario/contraseña, RFC1929.
- **Canal de monitoreo:** credencial de **admin propia** (distinta de los usuarios del proxy),
  configurada por flag al arrancar el servidor (ej. `-A user:pass`) y enviada en el handshake.
  Además el socket de monitoreo **bindea a `127.0.0.1` por default**.
- Limitación asumida: credenciales en texto plano sobre localhost (documentada como limitación).

## Métricas y registro de accesos
- **Métricas** (conexiones históricas/concurrentes, bytes): en memoria, **volátiles** (lo permite RF6).
- **Registro de accesos** (RF8): **persistido a archivo** (append `fecha usuario destino:puerto
  resultado`), porque el caso de uso (queja externa posterior) exige que sobreviva reinicios. El
  comando del protocolo de monitoreo lo expone.

## Límites y máximo de usuarios
- **`MAX_USERS = 10`** (constante documentada). Usuarios gestionados en runtime por el canal de
  monitoreo. Subir la constante si hiciera falta.
- **`MNG_MAX_CONNECTIONS = 16`** sesiones de monitoreo simultáneas. El límite reserva los
  descriptores necesarios para sostener 500 conexiones SOCKS5 completas bajo `FD_SETSIZE=1024`.
- Buffers de I/O: `SOCKS5_BUFFER_SIZE = 4096` por sentido. FQDN hasta 255 bytes (RFC1928).

## Graceful shutdown (RF9)
- `SIGINT`/`SIGTERM` se manejan con `sigaction` **sin `SA_RESTART`**: la señal interrumpe el
  `pselect` del selector (`EINTR`, que el selector devuelve como éxito) y el loop principal reacciona.
- El handler solo incrementa un `volatile sig_atomic_t` (async-signal-safe); toda la lógica corre en
  el thread principal tras `selector_select`.
- **1ª señal:** se desregistran ambos sockets pasivos. Las sesiones MNG que esperan entrada se
  cierran; las que tienen una respuesta preparada terminan de escribirla. El loop continúa hasta
  que no quedan conexiones SOCKS5 ni MNG activas.
- **2ª señal:** deja de procesar conexiones activas; antes de liberar recursos espera los workers
  DNS en vuelo, ya que `getaddrinfo()` no es cancelable de forma portable.

## Concurrencia y límite de conexiones (RF1)
- El selector de cátedra usa `pselect(2)` + `fd_set`, acotado por **`FD_SETSIZE` (1024)**. Con 2 fds
  por conexión (cliente + origin) el techo real es **~505–510 conexiones simultáneas**: cumple el
  mínimo de 500 exigido, con poco margen.
- Al arrancar se sube `RLIMIT_NOFILE` (soft→hard) para poder abrir esos descriptores.
- Superar ese techo requeriría reemplazar `pselect` por `epoll` en el selector (código de cátedra);
  no se hace. Documentado como limitación.

## Timeouts de inactividad
- Cada conexión SOCKS5 o MNG guarda `last_activity`; un barrido periódico cierra las que superan
  **`SOCKS5_IDLE_TIMEOUT` (60 s)** sin actividad. La granularidad del barrido es el `select_timeout`
  del selector (10 s), así que el cierre real ocurre entre 60 y ~70 s de inactividad.
- El barrido saltea conexiones en resolución de DNS (thread en vuelo) para no liberar su estado.
- El valor se comparte entre ambos servicios y puede modificarse en runtime mediante
  `SET_TIMEOUT` del protocolo de monitoreo. El valor cero deshabilita ambos barridos.

## Código de terceros / cátedra
- Utilidades de cátedra integradas vía parches `git am` (`selector`, `buffer`, `stm`, `parser`,
  `parser_utils`, `netutils`, `test.h`) + `args.c` oficial. Atribuidas en la historia git y en los
  headers. `socks5nio` se usó solo como **referencia conceptual**, no se copió.
