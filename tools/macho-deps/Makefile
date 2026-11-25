CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

all: macho-deps

macho-deps: macho-deps.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f macho-deps

.PHONY: all clean
