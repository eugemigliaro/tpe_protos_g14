# TPE — Servidor proxy SOCKS5 (Grupo 14)

Trabajo Práctico Especial — Protocolos de Comunicación (72.07), ITBA, 2026/1.

Servidor proxy **SOCKS5 (RFC1928)** con autenticación usuario/contraseña (RFC1929), protocolo de
monitoreo/configuración propio, cliente de monitoreo y métricas.

> Estado: **completo** — RF1–RF9, RNF3, RNF5 implementados.

## Compilación

Requiere un compilador C11 (`cc`/`gcc`), `make` y POSIX threads.

```sh
make            # genera bin/server y bin/client
make test-stress # integración: límite, timeout, shutdown y 500 SOCKS + 16 MNG
make clean      # borra bin/ y obj/
```

## Ejecución rápida

```sh
# Servidor con un usuario SOCKS5 y credencial de admin
./bin/server -p 1080 -P 8080 -u alice:secret -A admin:adminpass

# Tráfico a través del proxy
curl --proxy socks5://alice:secret@127.0.0.1:1080 http://example.com

# Cliente de monitoreo
./bin/client -A admin:adminpass stats
./bin/client -A admin:adminpass log
```

## Opciones del servidor

| Flag | Descripción | Default |
|------|-------------|---------|
| `-p PUERTO` | Puerto SOCKS5 | 1080 |
| `-P PUERTO` | Puerto de monitoreo | 8080 |
| `-l ADDR` | Dirección bind SOCKS5 | 0.0.0.0 |
| `-L ADDR` | Dirección bind monitoreo | 127.0.0.1 |
| `-u USER:PASS` | Usuario SOCKS5 (repetible, máx. 10) | — |
| `-A USER:PASS` | Credencial de administrador | — |
| `-N` | Deshabilita disectores | — |
| `-v` | Versión | — |
| `-h` | Ayuda | — |

El access log se escribe en `./socks5_access.log` (directorio de trabajo).  
Sin `-u`: el proxy opera sin autenticación. Sin `-A`: el canal de monitoreo rechaza todo.

## Subcomandos del cliente

```sh
./bin/client -A admin:pass stats                  # métricas
./bin/client -A admin:pass users                  # listar usuarios
./bin/client -A admin:pass add-user alice secret  # agregar usuario
./bin/client -A admin:pass del-user alice         # eliminar usuario
./bin/client -A admin:pass set-timeout 120        # timeout SOCKS/MNG (0=off)
./bin/client -A admin:pass log                    # últimas 100 entradas del log
```

Flags del cliente: `-L ADDR` (default 127.0.0.1), `-P PUERTO` (default 8080), `-A USER:PASS`, `-v`, `-h`.

## Estructura del proyecto

```
.
├── Makefile
├── doc/
│   ├── informe.md     # informe final (11 secciones)
│   ├── SPEC.md        # especificación del protocolo MNG/1 (estilo RFC)
│   ├── DECISIONS.md   # decisiones de diseño y justificaciones
│   └── DESIGN.md      # arquitectura y máquinas de estados
└── src/
    ├── server/        # SOCKS5 + canal de monitoreo
    ├── client/        # cliente del protocolo MNG/1
    └── shared/        # código común (version, mng_proto.h)
```

## Documentación

- `doc/informe.md` — informe final con las 11 secciones de la cátedra.
- `doc/SPEC.md` — especificación completa del protocolo de monitoreo MNG/1.
- `doc/DECISIONS.md` — decisiones de diseño y justificaciones.
- `doc/DESIGN.md` — arquitectura, máquinas de estados y canal de monitoreo.

## Integrantes

- Eugenio Migliaro, 65508, emigliaro@itba.edu.ar
- Jonás Glaubart, 65790, jglaubart@itba.edu.ar
- Franco Manuel Pampuri, 65552, fpampuri@itba.edu.ar
