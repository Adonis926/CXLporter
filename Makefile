CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS ?=

BIN := cxlporter
SRC := src/cxlporter.c

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC) src/cxlfork_uapi.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(BIN)
