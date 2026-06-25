# tpe_protos_g14 — Servidor proxy SOCKS5 (Protocolos 72.07, ITBA)

Trabajo Práctico Especial 2026/1. Servidor proxy **SOCKS5 (RFC1928)** en **C11** + protocolo de
monitoreo propio + cliente. Idioma de trabajo: **español**.

Este repo es la **entrega** (su historia git se entrega). El contexto curado del TP vive en el repo
de estudio del owner, **fuera de este repo**.

## Setup (igual para todo el equipo)

Este repo se clona **dentro del repo de estudio**, en `practica/`, con su nombre por defecto:

```sh
git clone <url-repo-estudio> protocolos_de_comunicacion
cd protocolos_de_comunicacion/practica
git clone https://github.com/eugemigliaro/tpe_protos_g14.git
cd tpe_protos_g14
```

Así queda en `practica/tpe_protos_g14/` y las rutas relativas `../tpe/` resuelven para todos.
El repo de estudio ya gitignorea `practica/tpe_protos_g14/`, así que el clon anidado nunca se
trackea por error desde el repo de estudio.

## Contexto del TP (leer primero)

Ruta relativa desde la raíz de este repo: `../tpe/` (carpeta del repo de estudio).

- `../tpe/consigna.md` — consigna oficial. **Fuente de verdad.**
- `../tpe/requisitos.md` — checklist RF/RNF trazable con niveles RFC2119.
- `../tpe/apuntes-clases-tp.md` — apuntes de las clases que la cátedra dio sobre este TP.
- `../tpe/plan.md` — plan por fases. `../tpe/progreso.md` — estado vivo.
- `../tpe/analisis-nash.md` — referencia conceptual del grupo anterior (**NO copiar código**).

## Reglas duras

- **C11.** I/O **no bloqueante multiplexada en un solo thread** (única excepción: `getaddrinfo`).
- **Read/write parciales SIEMPRE.** Nunca asumir que un `read()` == un mensaje, ni que `write()`
  escribe todo. El resto pendiente se conserva para el próximo evento.
- **Estado por conexión**; el protocolo se modela como **máquina de estados**.
- Reportar errores sin caerse; usar todos los reply codes de SOCKS5.
- Código de cátedra reutilizado: **atribuir**. Nada de copiar código de otros grupos.
- Commits chicos, claros, en español. No commitear `bin/` ni `obj/`.

## Build y ejecución

```sh
make                       # genera bin/server y bin/client
make clean
./bin/server -p 1080 -P 8080
./bin/client -L 127.0.0.1 -P 8080
```

## Estructura

```
src/server/   lógica del servidor (SOCKS5 + monitoreo)
src/client/   cliente del protocolo de monitoreo
src/shared/   código común a server y client
doc/          SPEC.md, DECISIONS.md, DESIGN.md, informe
```
