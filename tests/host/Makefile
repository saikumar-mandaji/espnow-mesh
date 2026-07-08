# Host-only test build for the portable espnow_mesh core.
#
# Deliberately uses a plain host `cc`, NOT the ESP-IDF toolchain: the code
# under test (src/mesh/mesh.c, include/espnow_mesh/mesh.h) is pure C99 with
# no ESP-IDF/FreeRTOS/Wi-Fi dependencies, so it can and should be tested
# directly on the development machine / CI runner.

CC      ?= cc
CFLAGS  ?= -std=gnu99 -O0 -g -Wall -Wextra -Wpedantic
ROOT    := ../..
CORE_DIR:= $(ROOT)/src/mesh
INCLUDE := $(ROOT)/include

SRCS := test_mesh.c $(CORE_DIR)/mesh.c
BIN  := test_mesh

.PHONY: test clean

test: $(BIN)
	./$(BIN)

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) -iquote $(INCLUDE) -o $(BIN) $(SRCS)

clean:
	rm -f $(BIN)
