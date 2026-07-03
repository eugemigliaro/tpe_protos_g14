#ifndef ACCESS_LOG_H_g14
#define ACCESS_LOG_H_g14

/**
 * access_log.h - registro de accesos por usuario (RF8).
 *
 * Cada entrada tiene el formato:
 *   YYYY-MM-DDTHH:MM:SSZ USUARIO DESTINO:PUERTO RESULTADO
 *
 * Las últimas LOG_RECENT_MAX entradas se mantienen en un buffer circular
 * en RAM para responder al comando GET_LOG sin I/O. El archivo en disco
 * sirve para persistencia entre reinicios.
 */

#define LOG_RECENT_MAX 100  /* entradas recientes guardadas en RAM */

/**
 * Abre (o crea) el archivo de access log en path.
 * Si path es NULL no se persiste a disco (las entradas igualmente van
 * al buffer circular en RAM).
 * Retorna 0 en éxito, -1 en error.
 */
int  access_log_open(const char *path);

/** Cierra el archivo de access log. */
void access_log_close(void);

/**
 * Emite una entrada al access log (RAM + disco si está abierto).
 *   username: usuario autenticado; NULL o "" se escribe como "-".
 *   dest:     "host:puerto" del destino.
 *   result:   "OK" u otro string de resultado.
 */
void access_log_entry(const char *username, const char *dest,
                      const char *result);

/**
 * Copia hasta max_entries punteros a entradas recientes en entries[].
 * Las cadenas apuntan al buffer interno estático (no liberar).
 * Retorna la cantidad real de entradas disponibles.
 */
int  access_log_get_recent(const char **entries, int max_entries);

#endif
