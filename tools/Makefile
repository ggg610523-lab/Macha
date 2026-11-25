CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
LDLIBS = -ldl

all: reef_mtl_probe

reef_mtl_probe: reef_mtl_probe.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f reef_mtl_probe

.PHONY: all clean
