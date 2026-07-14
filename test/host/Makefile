# Host-side unit tests (Phase 21): the pure parsers compile with any C
# compiler because they are libc-only by design. `make` builds and runs.
CC     ?= cc
CFLAGS  = -Wall -Wextra -O1 -g \
          -I unity \
          -I ../../idf/components/stream/include \
          -I ../../idf/components/radiko/include \
          -I ../../idf/components/radiko \
          -I ../../idf/components/ota/include

SRCS = unity/unity.c \
       ../../idf/components/stream/hls_parse.c \
       ../../idf/components/radiko/radiko_parse.c \
       ../../idf/components/radiko/puff.c \
       ../../idf/components/ota/ota_parse.c \
       test_hls_parse.c \
       test_radiko_parse.c \
       test_ota_parse.c \
       main.c

all: run

build/tests: $(SRCS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) -o $@

run: build/tests
	./build/tests

clean:
	rm -rf build

.PHONY: all run clean
