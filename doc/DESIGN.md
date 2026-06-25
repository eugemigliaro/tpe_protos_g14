# Documento de diseño

> Esqueleto. Describir la arquitectura de la aplicación y la máquina de estados (con diagrama ASCII
> o imagen). Ver `../tpe/apuntes-clases-tp.md` §9–§12.

## Arquitectura general

_(Event loop no bloqueante en un solo thread, dos sockets pasivos — SOCKS5 y monitoreo —, estado por
conexión, separación parser / validador / motor / encoder / I/O.)_

## Máquina de estados SOCKS5

```
HELLO  (negociación de método, RFC1928)
  -> AUTH (user/pass, RFC1929)          [si se negoció método 0x02]
  -> REQUEST (CONNECT + ATYP: IPv4 / IPv6 / FQDN)
  -> RESOLVE (getaddrinfo no bloqueante)
  -> CONNECT (probar IPs en orden hasta una que ande — RF4)
  -> RELAY  (copia bidireccional con read/write parciales)
  -> CLOSING -> CLOSED (liberar recursos)
```

_(Completar: tabla estado → eventos → transiciones, manejo de cierres de medio canal.)_

## Estado por conexión

_(Buffers de entrada/salida por sentido, estado del parser, estado del protocolo, datos de sesión.)_

## Componentes

| Componente | Responsabilidad |
|------------|-----------------|
| Parser | bytes → comandos/frames |
| Validador | formato, argumentos, límites |
| Motor | ejecuta la operación |
| Encoder | respuestas internas → bytes |
| I/O | leer/escribir manejando parcialidades |
| Estado de conexión | en qué punto está cada cliente |

## Resolución de nombres

_(getaddrinfo en thread aparte / getaddrinfo_a; cómo despierta al thread principal.)_
