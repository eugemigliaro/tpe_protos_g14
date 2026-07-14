#ifndef ACCESS_LOG_H_g14
#define ACCESS_LOG_H_g14

/**
 * Registro de accesos por usuario (RF8).
 *
 * Cada entrada tiene el formato:
 *   YYYY-MM-DDTHH:MM:SSZ USUARIO DESTINO:PUERTO RESULTADO
 *
 * Las últimas LOG_RECENT_MAX entradas se mantienen en RAM para responder
 * GET_LOG sin I/O. El archivo en disco persiste entradas entre reinicios.
 */

#define LOG_RECENT_MAX 100  /* entradas recientes guardadas en RAM */

/**
 * Abre o crea el archivo de access log en path.
 * Si path es NULL, las entradas se conservan únicamente en RAM.
 * Retorna 0 en éxito, -1 en error.
 */
int  access_log_open(const char *path);

/** Cierra el archivo de access log. */
void access_log_close(void);

/**
 * Emite una entrada al access log en RAM y, si está abierto, en disco.
 * username NULL o vacío se representa como "-".
 */
void access_log_entry(const char *username, const char *dest,
                      const char *result);

/**
 * Copia hasta max_entries punteros a entradas recientes en entries[].
 * Las cadenas apuntan al buffer interno estático y no deben liberarse.
 */
int  access_log_get_recent(const char **entries, int max_entries);

#endif
