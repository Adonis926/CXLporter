CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS ?=

# The kernel UAPI is authoritative.  Copy only cxlfork.h into an ignored build
# include directory so its <linux/types.h> dependency resolves to the host's
# installed userspace headers rather than to unexported kernel internals.
CXLFORK_UAPI_HEADER ?= ../kernel/linux-6.6.32/include/uapi/linux/cxlfork.h
UAPI_BUILD_DIR := .build-uapi/include
CFLAGS += -I$(UAPI_BUILD_DIR)

BIN := cxlporter cxlporter-control

.PHONY: all control clean

all: $(BIN)

control: cxlporter-control

cxlporter: src/cxlporter.c $(UAPI_BUILD_DIR)/linux/cxlfork.h
	$(CC) $(CFLAGS) -Wno-format -o $@ $< $(LDFLAGS)

cxlporter-control: src/cxlporter_control.c $(UAPI_BUILD_DIR)/linux/cxlfork.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(UAPI_BUILD_DIR)/linux/cxlfork.h: $(CXLFORK_UAPI_HEADER)
	mkdir -p $(dir $@)
	cp $< $@

clean:
	rm -f $(BIN)
	rm -rf .build-uapi
