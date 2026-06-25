# Decisiones de diseño

Decisiones técnicas con su justificación. Sirve para el informe y la defensa.
Estado: decisiones marcadas **[OK]** ya están acordadas; las demás se completan al implementar.

## Transporte
- SOCKS5: **TCP** (lo fija RFC1928).
- Protocolo de monitoreo: **TCP**. Es orientado a conexión, con comandos/respuestas asociadas y
  sesión autenticada; TCP es lo natural (no hay que resolver pérdida/orden a mano).

## Formato del protocolo de monitoreo (texto vs binario) — [OK] binario
Elegimos **binario** con handshake de versión + campos `LEN+STRING` + códigos de status.
- **Por qué:** lo que se evalúa es "diseñar e implementar un protocolo"; un binario con framing por
  longitud es la evidencia más fuerte de esa competencia y obliga a manejar framing correctamente.
  La cátedra ya dio la plantilla (versión, METHOD, LEN+STRING, STATUS).
- Trade-off: menos legible que texto; se mitiga documentando bien la SPEC y con ejemplos en hexdump.

## Framing
- Monitoreo: longitud explícita (`LEN+STRING`), sin separadores. Se leen exactamente `LEN` bytes.
- Handshake de versión al inicio (permite evolucionar sin romper compatibilidad).

## Pipelining
- _(a definir al implementar el protocolo de monitoreo; si no se soporta, aclararlo en SPEC.)_

## Concurrencia e I/O — [OK] selector de cátedra
- Un único thread, I/O no bloqueante multiplexada con el **`selector` de la cátedra** (integrado vía
  los parches `git am`, atribuido en la historia git).
- Única excepción permitida: resolución de nombres fuera del thread principal (ver abajo).

## Resolución de nombres y robustez (RF4)
- `getaddrinfo` **fuera del thread principal** (thread dedicado que solo resuelve y despierta al main,
  o `getaddrinfo_a`) para no bloquear el event loop.
- Robustez: ante un FQDN con múltiples IPs, **iterar toda la lista** y probar la siguiente si una falla.

## Manejo de errores y reply codes (RF5)
- Usar **todos los reply codes** de SOCKS5 (no solo success / general failure), mapeando cada causa
  de fallo a su código. _(detallar la tabla al implementar REQUEST/CONNECT.)_

## Autenticación — [OK]
- **Proxy:** usuario/contraseña, RFC1929.
- **Canal de monitoreo:** credencial de **admin propia** (distinta de los usuarios del proxy),
  configurada por flag al arrancar el servidor (ej. `-A user:pass`) y enviada en el handshake.
  Además el socket de monitoreo **bindea a `127.0.0.1` por default** (defensa en profundidad).
- Limitación asumida: credenciales en texto plano sobre localhost (documentada como limitación).

## Métricas y registro de accesos — [OK]
- **Métricas** (conexiones históricas/concurrentes, bytes): en memoria, **volátiles** (lo permite RF6).
- **Registro de accesos** (RF8): **persistido a archivo** (append `usuario fecha destino:puerto
  resultado`), porque el caso de uso (queja externa posterior) exige que sobreviva reinicios. El
  comando del protocolo de monitoreo lo expone.

## Límites — [OK] máximo de usuarios
- **`MAX_USERS = 10`** (constante documentada). Usuarios gestionados en runtime por el canal de
  monitoreo. Subir la constante si hiciera falta.
- _(definir además: tamaños de buffer, máximos de campos del protocolo.)_

## Graceful shutdown (RF9)
- SIGTERM/SIGINT: dejar de aceptar conexiones nuevas y esperar a que terminen las activas; una 2da
  señal fuerza el apagado. _(detallar al implementar.)_

## Código de terceros / cátedra
- Utilidades de cátedra integradas vía parches `git am` (`selector`, `buffer`, `stm`, `parser`,
  `parser_utils`, `netutils`, `test.h`) + `args.c` oficial. Atribuidas en la historia git y en los
  headers. `socks5nio` se usó solo como **referencia conceptual**, no se copió.
