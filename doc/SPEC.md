# Especificación del protocolo de monitoreo

> Esqueleto. Completar al diseñar el protocolo (Fase 3). Debe quedar en **estilo RFC**: agnóstico
> al lenguaje y completo para que un tercero pueda reimplementarlo. Ver `../tpe/apuntes-clases-tp.md`
> §15 (plantilla) y la consigna (Evaluación → informe).

## 1. Objetivo

_(Qué resuelve el protocolo: monitoreo, gestión de usuarios y cambio de configuración en runtime.)_

## 2. Transporte y formato

- Transporte: _(TCP — justificar en `DECISIONS.md`)_
- Formato: _(texto / binario — justificar)_
- Framing: _(cómo termina un mensaje)_
- Autenticación: _(cómo se autentica el canal de monitoreo)_

## 3. Estados

_(Máquina de estados del protocolo: handshake → transacción → cierre.)_

## 4. Formato general de mensaje

_(Estructura de request y de response.)_

## 5. Comandos

| Comando | Argumentos | Respuesta OK | Respuesta ERR |
|---------|------------|--------------|---------------|
| _(p.ej. add-user)_ | _user pass_ | | |
| _(stats)_ | | | |
| _(config)_ | _param valor_ | | |

## 6. Errores

_(Códigos/mensajes de error y su significado.)_

## 7. Ejemplos de conversación

```
C> ...
S> ...
```

## 8. Cierre

_(Cómo se cierra la conexión.)_

## 9. Casos borde y límites

_(Tamaños máximos, caracteres permitidos, qué pasa con argumentos faltantes/sobrantes, pipelining.)_
