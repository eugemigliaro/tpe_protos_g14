#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "version.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "Uso: %s [-L ADDR] [-P PUERTO] [-v] [-h] [comando ...]\n"
        "  -L ADDR    direccion del servicio de monitoreo (default 127.0.0.1)\n"
        "  -P PUERTO  puerto del servicio de monitoreo (default 8080)\n"
        "  -v         imprime version y termina\n"
        "  -h         imprime esta ayuda y termina\n"
        "Ejemplo: %s add-user pablito pass1234\n",
        prog, prog);
}

int main(int argc, char *argv[]) {
    const char *addr = "127.0.0.1";
    unsigned short port = 8080;

    int opt;
    while ((opt = getopt(argc, argv, "L:P:vh")) != -1) {
        switch (opt) {
            case 'L': addr = optarg; break;
            case 'P': port = (unsigned short) atoi(optarg); break;
            case 'v': printf("client %s\n", version_string()); return 0;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    printf("[scaffold] client %s — destino %s:%u\n", version_string(), addr, port);
    printf("[scaffold] TODO: protocolo de monitoreo y comandos (Fase 3).\n");
    return 0;
}
