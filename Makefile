include ./Makefile.inc

# Fuentes (los wildcards de states/ y utils/ quedan vacíos hasta la Fase 1)
SERVER_SOURCES=$(wildcard src/server/*.c) $(wildcard src/server/states/*.c) $(wildcard src/server/utils/*.c)
CLIENT_SOURCES=$(wildcard src/client/*.c)
SHARED_SOURCES=$(wildcard src/shared/*.c)

OUTPUT_FOLDER=./bin
OBJECTS_FOLDER=./obj

SERVER_OBJECTS=$(SERVER_SOURCES:src/%.c=obj/%.o)
CLIENT_OBJECTS=$(CLIENT_SOURCES:src/%.c=obj/%.o)
SHARED_OBJECTS=$(SHARED_SOURCES:src/%.c=obj/%.o)

SERVER_BIN=$(OUTPUT_FOLDER)/server
CLIENT_BIN=$(OUTPUT_FOLDER)/client

all: server client

server: $(SERVER_BIN)
client: $(CLIENT_BIN)

$(SERVER_BIN): $(SERVER_OBJECTS) $(SHARED_OBJECTS)
	mkdir -p $(OUTPUT_FOLDER)
	$(COMPILER) $(SERVER_OBJECTS) $(SHARED_OBJECTS) -o $@ $(LDFLAGS)

$(CLIENT_BIN): $(CLIENT_OBJECTS) $(SHARED_OBJECTS)
	mkdir -p $(OUTPUT_FOLDER)
	$(COMPILER) $(CLIENT_OBJECTS) $(SHARED_OBJECTS) -o $@ $(LDFLAGS)

# Regla patrón: un .o por cada .c, recreando la jerarquía de carpetas bajo obj/
obj/%.o: src/%.c
	mkdir -p $(dir $@)
	$(COMPILER) $(COMPILER_FLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(OUTPUT_FOLDER) $(OBJECTS_FOLDER)

.PHONY: all server client clean
