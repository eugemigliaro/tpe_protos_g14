#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "version.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "Uso: %s [-p PUERTO] [-P PUERTO_CONF] [-v] [-h]\n"
        "  -p PUERTO        puerto SOCKS5 (default 1080)\n"
        "  -P PUERTO_CONF   puerto del protocolo de monitoreo (default 8080)\n"
        "  -v               imprime version y termina\n"
        "  -h               imprime esta ayuda y termina\n",
        prog);
}

int main(int argc, char *argv[]) {
    unsigned short socks_port = 1080;
    unsigned short conf_port = 8080;

    int opt;
    while ((opt = getopt(argc, argv, "p:P:vh")) != -1) {
        switch (opt) {
            case 'p': socks_port = (unsigned short) atoi(optarg); break;
            case 'P': conf_port  = (unsigned short) atoi(optarg); break;
            case 'v': printf("server %s\n", version_string()); return 0;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    printf("[scaffold] server %s — SOCKS5 puerto %u, monitoreo puerto %u\n",
           version_string(), socks_port, conf_port);
    printf("[scaffold] TODO: event loop no bloqueante + maquina de estados SOCKS5 (Fase 1).\n");
    return 0;
}
