# Especificación del protocolo de monitoreo — MNG/1

**Versión:** 1  
**Fecha:** 2026-07  
**Estado:** Definitivo  

---

## 1. Objetivo

El protocolo **MNG/1** permite a un cliente autorizado conectarse al servidor proxy SOCKS5 a través
de un canal TCP dedicado para:

- Consultar métricas de operación (conexiones históricas, concurrentes, bytes transferidos).
- Listar, agregar y eliminar usuarios SOCKS5 en tiempo de ejecución sin reiniciar el servidor.
- Ajustar el tiempo de inactividad máximo de conexiones SOCKS5 y MNG en tiempo de ejecución.
- Consultar las últimas entradas del access log.
- Cerrar ordenadamente la sesión de monitoreo.

El protocolo es **independiente del protocolo SOCKS5** y opera en un puerto TCP diferente.

---

## 2. Transporte y codificación

| Propiedad   | Valor                                             |
|-------------|---------------------------------------------------|
| Transporte  | TCP                                               |
| Formato     | Binario (campos de longitud fija o length-prefix) |
| Byte order  | Big-endian (network byte order) para todos los campos de más de 1 byte |
| Framing     | Longitud implícita en los campos de cada mensaje (no hay delimitador externo) |
| Pipelining  | No soportado. El cliente DEBE esperar la respuesta antes de enviar el siguiente comando. |
| Cifrado     | No (fuera de alcance). Desplegar en red de confianza o túnel SSH. |

---

## 3. Máquina de estados

```
        ┌──────────────┐
        │  TCP connect │
        └──────┬───────┘
               │
               ▼
       ┌───────────────┐
       │  MNG_AUTH_READ │  ← cliente envía handshake (VER + credenciales)
       └───────┬────────┘
               │
       ┌───────┴───────────────────────────────┐
       │ STATUS 0x00 (ok)          STATUS≠0x00 (fallo / ver no soportada)
       │                                │
       ▼                                ▼
┌──────────────────┐           ┌──────────────────┐
│ MNG_AUTH_WRITE   │           │  MNG_AUTH_WRITE   │ (escribe fallo y cierra)
└──────┬───────────┘           └──────────────────┘
       │ STATUS ok escrito
       ▼
┌──────────────┐
│ MNG_CMD_READ  │  ← cliente envía CMD
└──────┬────────┘
       │
       ▼
┌──────────────────┐
│ MNG_RESP_WRITE   │  ← servidor escribe STATUS [+ datos]
└──────┬───────────┘
       │
       ├── cmd ≠ CLOSE  →  vuelve a MNG_CMD_READ  (loop)
       │
       └── cmd == CLOSE o EOF  →  MNG_DONE (cierre de conexión)
```

---

## 4. Handshake de autenticación

### 4.1. Request del cliente

```
+-----+------+-------+------+--------+
| VER | ULEN | UNAME | PLEN | PASSWD |
+-----+------+-------+------+--------+
|  1  |  1   | ULEN  |  1   |  PLEN  |  bytes
+-----+------+-------+------+--------+
```

| Campo  | Tipo    | Descripción                                 |
|--------|---------|---------------------------------------------|
| VER    | uint8   | Versión del protocolo. DEBE ser `0x01`.      |
| ULEN   | uint8   | Longitud del nombre de usuario (0–255).      |
| UNAME  | bytes   | Nombre de usuario, sin terminador nulo.      |
| PLEN   | uint8   | Longitud de la contraseña (0–255).           |
| PASSWD | bytes   | Contraseña, sin terminador nulo.             |

### 4.2. Respuesta del servidor

```
+-----+--------+
| VER | STATUS |
+-----+--------+
|  1  |   1    |  bytes
+-----+--------+
```

| STATUS | Significado                     |
|--------|---------------------------------|
| 0x00   | Autenticación exitosa.           |
| 0x01   | Credenciales inválidas.          |
| 0xFF   | Versión de protocolo no soportada.|

Si STATUS ≠ 0x00 el servidor cierra la conexión inmediatamente tras enviar la respuesta.

---

## 5. Formato general de comando y respuesta

### 5.1. Request de comando

```
+-----+----------+
| CMD | args...  |
+-----+----------+
|  1  | variable |  bytes
+-----+----------+
```

El campo CMD identifica el comando. Los argumentos son específicos de cada comando (§6).

### 5.2. Respuesta

```
+--------+----------+
| STATUS | data...  |
+--------+----------+
|   1    | variable |  bytes
+--------+----------+
```

| STATUS | Significado                           |
|--------|---------------------------------------|
| 0x00   | Éxito. `data` presente según el comando. |
| 0x01   | Recurso no encontrado (o ya existe).   |
| 0x02   | Límite alcanzado (tabla llena).        |
| 0x03   | Argumentos incorrectos / CMD desconocido. |
| 0xFF   | Error interno del servidor.            |

---

## 6. Comandos

### 6.1. ADD_USER — `0x01`

Agrega un usuario SOCKS5. Falla con STATUS `0x02` si la tabla está llena, o con STATUS `0x01`
si el usuario ya existe.

**Request:**
```
+-----+------+-------+------+--------+
| CMD | ULEN | UNAME | PLEN | PASSWD |
+-----+------+-------+------+--------+
| 0x01|  1   | ULEN  |  1   |  PLEN  |
+-----+------+-------+------+--------+
```

**Response (ok):** `STATUS(0x00)` — sin datos adicionales.

---

### 6.2. DEL_USER — `0x02`

Elimina un usuario SOCKS5 por nombre. Falla con STATUS `0x01` si no existe.

**Request:**
```
+-----+------+-------+
| CMD | ULEN | UNAME |
+-----+------+-------+
| 0x02|  1   | ULEN  |
+-----+------+-------+
```

**Response (ok):** `STATUS(0x00)` — sin datos adicionales.

---

### 6.3. LIST_USERS — `0x03`

Lista todos los usuarios SOCKS5 registrados en ese momento.

**Request:**
```
+-----+
| CMD |
+-----+
| 0x03|
+-----+
```

**Response (ok):**
```
+--------+-------+------+-------+------+-------+- -+
| STATUS | COUNT | ULEN | UNAME | ULEN | UNAME |...|
+--------+-------+------+-------+------+-------+- -+
|   1    |   1   |  1   | ULEN  |  1   | ULEN  |...|  × COUNT
+--------+-------+------+-------+------+-------+- -+
```

| Campo | Tipo   | Descripción                          |
|-------|--------|--------------------------------------|
| COUNT | uint8  | Número de usuarios (0–255).           |
| ULEN  | uint8  | Longitud del nombre del i-ésimo usuario. |
| UNAME | bytes  | Nombre del usuario, sin nulo.         |

---

### 6.4. GET_STATS — `0x04`

Retorna las métricas de operación del proxy.

**Request:**
```
+-----+
| CMD |
+-----+
| 0x04|
+-----+
```

**Response (ok):**
```
+--------+------+------+------------+------------+
| STATUS | HIST | CURR | BYTES_SENT | BYTES_RECV |
+--------+------+------+------------+------------+
|   1    |  8   |  4   |     8      |     8      |  bytes
+--------+------+------+------------+------------+
```

| Campo      | Tipo     | Descripción                                       |
|------------|----------|---------------------------------------------------|
| HIST       | uint64   | Conexiones SOCKS5 históricas (desde el arranque). |
| CURR       | uint32   | Conexiones SOCKS5 activas en este instante.       |
| BYTES_SENT | uint64   | Bytes enviados desde el proxy hacia los clientes. |
| BYTES_RECV | uint64   | Bytes recibidos desde los clientes hacia el proxy.|

Todos los campos en big-endian.

---

### 6.5. SET_TIMEOUT — `0x05`

Cambia el tiempo de inactividad máximo de conexiones SOCKS5 y MNG. El valor `0` deshabilita el
timeout para ambas.

**Request:**
```
+-----+---------+
| CMD | TIMEOUT |
+-----+---------+
| 0x05|    4    |  bytes
+-----+---------+
```

| Campo   | Tipo   | Descripción                        |
|---------|--------|------------------------------------|
| TIMEOUT | uint32 | Segundos de inactividad. Big-endian.|

**Response (ok):** `STATUS(0x00)` — sin datos adicionales.

---

### 6.6. GET_LOG — `0x06`

Retorna las últimas entradas del access log (hasta 100 entradas, en orden cronológico).

**Request:**
```
+-----+
| CMD |
+-----+
| 0x06|
+-----+
```

**Response (ok):**
```
+--------+-------+------+-------+------+-------+- -+
| STATUS | COUNT | ELEN | ENTRY | ELEN | ENTRY |...|
+--------+-------+------+-------+------+-------+- -+
|   1    |   2   |  2   | ELEN  |  2   | ELEN  |...|  × COUNT
+--------+-------+------+-------+------+-------+- -+
```

| Campo | Tipo   | Descripción                                            |
|-------|--------|--------------------------------------------------------|
| COUNT | uint16 | Número de entradas (0–100). Big-endian.                |
| ELEN  | uint16 | Longitud en bytes de la i-ésima entrada. Big-endian.   |
| ENTRY | bytes  | Texto UTF-8 de la entrada del log, sin terminador nulo.|

Formato de cada entrada (texto): `TIMESTAMP USUARIO DESTINO RESULTADO`  
Ejemplo: `2026-07-03T14:23:01Z alice example.com:443 OK`  
Usuario `-` cuando la conexión fue sin autenticación.

---

### 6.7. CLOSE — `0x07`

Solicita el cierre ordenado de la sesión de monitoreo.

**Request:**
```
+-----+
| CMD |
+-----+
| 0x07|
+-----+
```

**Response:** `STATUS(0x00)` — el servidor cierra la conexión TCP tras enviar el STATUS.

---

## 7. Ejemplos de conversación (bytes en hex)

### 7.1. Autenticación exitosa + GET_STATS

```
C→S  01 05 61 64 6d 69 6e 09 61 64 6d 69 6e 70 61 73 73
     ^  ^  [admin (5B)]      ^  [adminpass (9B)]
     VER ULEN                PLEN

S→C  01 00
     ^  ^
     VER STATUS=ok

C→S  04
     (CMD=GET_STATS)

S→C  00
     00 00 00 00 00 00 00 0a    (HIST=10, uint64 big-endian)
     00 00 00 03                (CURR=3,  uint32 big-endian)
     00 00 00 00 00 00 04 b0    (BYTES_SENT=1200, uint64)
     00 00 00 00 00 00 09 60    (BYTES_RECV=2400, uint64)
```

### 7.2. Autenticación fallida

```
C→S  01 05 61 64 6d 69 6e 05 77 72 6f 6e 67
     (VER=1, "admin", "wrong")

S→C  01 01
     ^  ^
     VER STATUS=0x01 (credenciales inválidas) → servidor cierra
```

### 7.3. ADD_USER + DEL_USER

```
C→S  01 03 62 6f 62 06 73 65 63 72 65 74
     (CMD=ADD_USER, ULEN=3, "bob", PLEN=6, "secret")

S→C  00   (STATUS=ok)

C→S  02 03 62 6f 62
     (CMD=DEL_USER, ULEN=3, "bob")

S→C  00   (STATUS=ok)
```

### 7.4. LIST_USERS con un usuario

```
C→S  03
     (CMD=LIST_USERS)

S→C  00 01 05 61 6c 69 63 65
     ^  ^  ^  [alice (5B)]
     OK CNT=1 ULEN=5
```

### 7.5. CLOSE

```
C→S  07
     (CMD=CLOSE)

S→C  00
     (STATUS=ok) → servidor cierra TCP
```

---

## 8. Cierre de conexión

El cierre puede ocurrir por:

1. **CLOSE (`0x07`)**: el cliente lo solicita explícitamente. El servidor responde STATUS `0x00` y
   cierra el socket.
2. **EOF del cliente**: el servidor detecta el cierre del lado cliente y libera los recursos sin
   enviar respuesta.
3. **Error de autenticación**: el servidor envía STATUS ≠ `0x00` y cierra inmediatamente.
4. **Error interno**: el servidor cierra la conexión sin garantizar una respuesta previa.
5. **Timeout de inactividad**: el servidor cierra la sesión cuando supera el valor configurado
   mediante `SET_TIMEOUT`.

El servidor NO cierra la conexión de forma unilateral durante una sesión autenticada, salvo error
irrecuperable, timeout de inactividad o apagado del proceso.

---

## 9. Casos borde y límites

| Condición                             | Comportamiento esperado                                   |
|---------------------------------------|-----------------------------------------------------------|
| ULEN=0 o PLEN=0 en handshake          | Se acepta (nombre o contraseña vacíos). El servidor valida contra su credencial de admin. |
| CMD desconocido                        | STATUS `0x03` (bad args); la sesión continúa.            |
| ULEN > 255                            | Imposible en el protocolo (ULEN es uint8).               |
| TIMEOUT=0 en SET_TIMEOUT              | Deshabilita el barrido de inactividad SOCKS5 y MNG.       |
| GET_LOG sin entradas                  | STATUS `0x00`, COUNT=0x0000.                             |
| Más de 100 entradas en el log         | Se retornan las 100 más recientes.                        |
| ADD_USER con usuario ya existente     | STATUS `0x01`.                                           |
| Tabla de usuarios llena               | STATUS `0x02`.                                           |
| DEL_USER de usuario inexistente       | STATUS `0x01`.                                           |
| Pipelining                            | Comportamiento indefinido. No hacer.                     |
| Conexiones simultáneas de monitoreo   | Permitidas hasta `MNG_MAX_CONNECTIONS` (16). La siguiente conexión se acepta y cierra sin handshake. |
