COMPILER=cc

# C11 estricto + warnings + símbolos de debug.
COMPILER_FLAGS=-std=c11 -Wall -Wextra -pedantic -g -pthread

# Feature test macros para exponer getopt/getaddrinfo/sigaction/pthread_kill.
# En Linux (target de entrega) usamos _POSIX_C_SOURCE; en macOS, _POSIX_C_SOURCE
# estricto oculta las funciones BSD de señales, así que usamos _DARWIN_C_SOURCE.
ifeq ($(shell uname -s),Darwin)
COMPILER_FLAGS+=-D_DARWIN_C_SOURCE
else
COMPILER_FLAGS+=-D_POSIX_C_SOURCE=200809L
endif

# Includes compartidos
INCLUDES=-Isrc/shared/include -Isrc/server -Isrc/server/include -Isrc/server/utils

LDFLAGS=-pthread
