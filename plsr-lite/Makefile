# plsr-lite is now built from the merged loader source shared with appexec
# (../appexec/src/appexec.c).  This variant links only the loader core
# and the reef host table, without the Mayonnaise UI objects; the weak
# plsr_mayo_* symbols resolve to NULL and every call site checks for that.
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
LDLIBS = -lz -lm -lpthread -ldl
SRC = ../appexec/src

plsr-lite: $(SRC)/appexec.c $(SRC)/reef_host.c $(SRC)/reef_host.h \
           $(SRC)/swiftui/engine.c $(SRC)/swiftui/state.c $(SRC)/swiftui/text.c \
           $(SRC)/swiftui/canvas.c $(SRC)/swiftui/host.c
	$(CC) $(CFLAGS) -I$(SRC) -I$(SRC)/swiftui -o $@ \
	    $(SRC)/appexec.c $(SRC)/reef_host.c \
	    $(SRC)/swiftui/engine.c $(SRC)/swiftui/state.c $(SRC)/swiftui/text.c \
	    $(SRC)/swiftui/canvas.c $(SRC)/swiftui/host.c $(LDLIBS)

clean:
	rm -f plsr-lite
