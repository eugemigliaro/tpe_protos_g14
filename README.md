# TPE — Servidor proxy SOCKS5 (Grupo 14)

Trabajo Práctico Especial — Protocolos de Comunicación (72.07), ITBA, 2026/1.

Servidor proxy **SOCKS5 (RFC1928)** con autenticación usuario/contraseña (RFC1929), protocolo de
monitoreo/configuración propio, cliente de monitoreo y métricas.

> Estado: **Fase 1 — SOCKS5 core**. Proxy CONNECT funcional con autenticación user/pass (RFC1929),
> destinos IPv4/IPv6/FQDN, iteración de IPs (RF4), reply codes completos (RF5) y relay bidireccional
> no bloqueante. El protocolo de monitoreo y el cliente llegan en fases siguientes (ver
> `../tpe/plan.md` en el repo de estudio).

## Compilación

Requiere un compilador C11 (`cc`/`gcc`) y `make`.

```sh
make            # genera bin/server y bin/client
make clean      # borra bin/ y obj/
```

## Ejecución

```sh
./bin/server -p 1080 -P 8080      # SOCKS5 en 1080, monitoreo en 8080
./bin/client -L 127.0.0.1 -P 8080 # cliente de monitoreo
```

| Artefacto | Ubicación | Descripción |
|-----------|-----------|-------------|
| `server`  | `bin/server` | Servidor SOCKS5 + monitoreo |
| `client`  | `bin/client` | Cliente del protocolo de monitoreo |

### Opciones del servidor

| Flag | Descripción |
|------|-------------|
| `-p PUERTO` | Puerto SOCKS5 (default 1080) |
| `-P PUERTO` | Puerto del protocolo de monitoreo (default 8080) |
| `-v` | Versión |
| `-h` | Ayuda |

### Opciones del cliente

| Flag | Descripción |
|------|-------------|
| `-L ADDR` | Dirección del servicio de monitoreo (default 127.0.0.1) |
| `-P PUERTO` | Puerto del servicio de monitoreo (default 8080) |
| `-v` | Versión |
| `-h` | Ayuda |

## Estructura del proyecto

```
.
├── Makefile, Makefile.inc
├── README.md
├── doc/
│   ├── SPEC.md        # protocolo de monitoreo (estilo RFC)
│   ├── DECISIONS.md   # decisiones de diseño y justificaciones
│   └── DESIGN.md      # arquitectura y máquina de estados
└── src/
    ├── server/        # SOCKS5 + monitoreo
    ├── client/        # cliente de monitoreo
    └── shared/        # código común
```

## Documentación

- `doc/SPEC.md` — especificación del protocolo de monitoreo.
- `doc/DECISIONS.md` — decisiones de diseño.
- `doc/DESIGN.md` — arquitectura y máquina de estados.
- Informe final: `doc/` (PDF, a entregar).

## Integrantes

- _(completar: nombre, legajo, email)_
