CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter

all: pulsar-run

pulsar-run: pulsar-run.c dmgmount.c dmgmount.h
	$(CC) $(CFLAGS) -o $@ pulsar-run.c dmgmount.c

clean:
	rm -f pulsar-run

.PHONY: all clean
