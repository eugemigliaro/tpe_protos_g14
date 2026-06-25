#include <stdio.h>

#include "args.h"
#include "version.h"

int main(int argc, char *argv[]) {
    struct socks5args args;
    parse_args(argc, argv, &args); /* maneja -h/-v y termina si corresponde */

    printf("[scaffold] server %s\n", version_string());
    printf("[scaffold]   SOCKS5     %s:%u\n", args.socks_addr, args.socks_port);
    printf("[scaffold]   monitoreo  %s:%u\n", args.mng_addr, args.mng_port);
    printf("[scaffold]   disectors  %s\n", args.disectors_enabled ? "on" : "off");
    printf("[scaffold] TODO: event loop no bloqueante + maquina de estados SOCKS5 (Fase 1).\n");
    return 0;
}
