CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS ?=

# The kernel UAPI is authoritative.  Copy only cxlfork.h into an ignored build
# include directory so its <linux/types.h> dependency resolves to the host's
# installed userspace headers rather than to unexported kernel internals.
CXLFORK_UAPI_HEADER ?= ../kernel/linux-6.6.32/include/uapi/linux/cxlfork.h
CXLFORK_K0_SOURCE ?= ../kernel/linux-6.6.32/tools/testing/selftests/cxlfork/cxlfork_capsule_k0.c
UAPI_BUILD_DIR := .build-uapi/include
CFLAGS += -I$(UAPI_BUILD_DIR)

BIN := cxlporter cxlporter-control cxlporter-paper cxlfork-ghost-agent \
	cxlfork-capsule-runtime

.PHONY: all control clean

all: $(BIN)

control: cxlporter-control

cxlporter: src/cxlporter.c $(UAPI_BUILD_DIR)/linux/cxlfork.h
	$(CC) $(CFLAGS) -Wno-format -o $@ $< $(LDFLAGS)

cxlporter-control: src/cxlporter_control.c $(UAPI_BUILD_DIR)/linux/cxlfork.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

cxlporter-paper: src/cxlporter_paper.c src/paper_protocol.h
	$(CC) $(CFLAGS) -Werror -o $@ $< $(LDFLAGS)

cxlfork-ghost-agent: src/cxlfork_ghost_agent.c src/paper_protocol.h \
		$(UAPI_BUILD_DIR)/linux/cxlfork.h
	$(CC) $(CFLAGS) -Werror -static -o $@ $< $(LDFLAGS)

cxlfork-capsule-runtime: $(CXLFORK_K0_SOURCE) \
		$(UAPI_BUILD_DIR)/linux/cxlfork.h
	$(CC) $(CFLAGS) -Werror -static -no-pie -fno-pie \
		-fno-stack-protector -fcf-protection=none -pthread -o $@ $<

$(UAPI_BUILD_DIR)/linux/cxlfork.h: $(CXLFORK_UAPI_HEADER)
	mkdir -p $(dir $@)
	cp $< $@

clean:
	rm -f $(BIN)
	rm -rf .build-uapi
