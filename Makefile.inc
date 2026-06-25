COMPILER=cc

# C11 estricto + warnings + símbolos de debug. _POSIX_C_SOURCE expone getopt/getaddrinfo/etc.
COMPILER_FLAGS=-std=c11 -Wall -Wextra -pedantic -g -D_POSIX_C_SOURCE=200809L -pthread

# Includes compartidos
INCLUDES=-Isrc/shared/include -Isrc/server/include -Isrc/server/utils

LDFLAGS=-pthread
