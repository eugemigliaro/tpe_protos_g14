/*
Las entradas se guardan en un buffer circular en RAM (para GET_LOG) y se persisten a un archivo de texto en modo append (para sobrevivir reinicios). 
El formato de cada línea es: YYYY-MM-DDTHH:MM:SSZ USUARIO DESTINO:PUERTO RESULTADO
*/
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "access_log.h"

#define LOG_LINE_MAX 512

static FILE *log_file = NULL;

/* Buffer circular de las últimas LOG_RECENT_MAX entradas. */
static char recent[LOG_RECENT_MAX][LOG_LINE_MAX];
static int  recent_head  = 0;  // próxima posición de escritura
static int  recent_count = 0;  // entradas válidas 

int access_log_open(const char *path){
    if (path == NULL) {
        return 0;
    }
    log_file = fopen(path, "a");
    return (log_file != NULL) ? 0 : -1;
}

voidaccess_log_close(void){
    if (log_file != NULL) {
        fclose(log_file);
        log_file = NULL;
    }
}

void access_log_entry(const char *username, const char *dest, const char *result){
    time_t    now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);

    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &utc);

    const char *user = (username != NULL && username[0] != '\0') ? username : "-";
    const char *d    = (dest   != NULL) ? dest   : "-";
    const char *r    = (result != NULL) ? result : "?";

    snprintf(recent[recent_head], LOG_LINE_MAX, "%s %s %s %s", ts, user, d, r);
    recent_head = (recent_head + 1) % LOG_RECENT_MAX;
    if (recent_count < LOG_RECENT_MAX) {
        recent_count++;
    }

    if (log_file != NULL) {
        // el entry recién escrito está en (recent_head - 1 + MAX) % MAX
        fprintf(log_file, "%s\n",
                recent[(recent_head - 1 + LOG_RECENT_MAX) % LOG_RECENT_MAX]);
        fflush(log_file);
    }
}

int access_log_get_recent(const char **entries, int max_entries){
    int count = (recent_count < max_entries) ? recent_count : max_entries;
    // los más viejos están desde (recent_head - recent_count) módulo tamaño
    int start = (recent_head - recent_count + LOG_RECENT_MAX) % LOG_RECENT_MAX;
    for (int i = 0; i < count; i++) {
        entries[i] = recent[(start + i) % LOG_RECENT_MAX];
    }
    return count;
}
