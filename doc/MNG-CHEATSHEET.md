# MNG/1 — Referencia rápida

| Propiedad  | Valor |
|------------|-------|
| Versión    | `0x01` |
| Transporte | TCP, puerto configurable (default 8080) |
| Formato    | Binario, big-endian (network byte order) |
| Pipelining | No soportado |

---

## Sesión típica

```
TCP connect → Auth handshake → Comando(s) en loop → CLOSE → TCP close
```

---

## Handshake de autenticación

| Dirección | Formato |
|-----------|---------|
| C → S | `VER(1)` `ULEN(1)` `UNAME(ulen)` `PLEN(1)` `PASSWD(plen)` |
| S → C | `VER(1)` `STATUS(1)` |

| STATUS auth | Significado |
|-------------|-------------|
| `0x00` | OK |
| `0x01` | Credenciales inválidas (cierra) |
| `0xFF` | Versión no soportada (cierra) |

---

## Comandos

| Byte | Comando | Request (C → S) | Response (S → C) |
|------|---------|------------------|-------------------|
| `0x01` | ADD_USER | `CMD(1)` `ULEN(1)` `UNAME` `PLEN(1)` `PASSWD` | `STATUS(1)` |
| `0x02` | DEL_USER | `CMD(1)` `ULEN(1)` `UNAME` | `STATUS(1)` |
| `0x03` | LIST_USERS | `CMD(1)` | `STATUS(1)` `COUNT(1)` {`ULEN(1)` `UNAME`}×count |
| `0x04` | GET_STATS | `CMD(1)` | `STATUS(1)` `HIST(8)` `CURR(4)` `SENT(8)` `RECV(8)` |
| `0x05` | SET_TIMEOUT | `CMD(1)` `TIMEOUT(4)` | `STATUS(1)` |
| `0x06` | GET_LOG | `CMD(1)` | `STATUS(1)` `COUNT(2)` {`ELEN(2)` `ENTRY`}×count |
| `0x07` | CLOSE | `CMD(1)` | `STATUS(1)` → cierra TCP |

---

## Status codes de comando

| Código | Significado |
|--------|-------------|
| `0x00` | OK |
| `0x01` | No encontrado / ya existe |
| `0x02` | Tabla llena |
| `0x03` | CMD desconocido / args inválidos |
| `0xFF` | Error interno |

---

## Tamaños de campo

| Campo | Tipo | Bytes |
|-------|------|-------|
| VER, CMD, STATUS, ULEN, PLEN, COUNT (users) | uint8 | 1 |
| COUNT (log), ELEN | uint16 | 2 |
| CURR, TIMEOUT | uint32 | 4 |
| HIST, BYTES_SENT, BYTES_RECV | uint64 | 8 |

Todos los campos multi-byte en **big-endian**.

---

## Límites

| Recurso | Valor |
|---------|-------|
| Usuarios SOCKS5 (MAX_USERS) | 10 |
| Entradas de log en RAM (LOG_RECENT_MAX) | 100 |
| Buffer lectura MNG | 2 KB |
| Buffer escritura MNG | 64 KB |
| Pool de conexiones MNG (MNG_POOL_MAX) | 16 |
| TIMEOUT = 0 | Deshabilita barrido de inactividad |
