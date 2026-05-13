# cicndump — standalone Mac-resource-fork cicn-to-PNG dumper.
#
# Not built by the main meson tree. The cicns it produces are
# committed into src/pixmaps/, so cicndump only runs when new
# icons need extracting (or the originals get updated).
#
# Build:
#   make
# Use:
#   ./cicndump ../../icons.rsrc out
#
# Dependencies: libpng (apt: libpng-dev / brew: libpng).

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpointer-arith -Wstrict-prototypes
LDFLAGS ?=
LIBS    := -lpng

cicndump: cicndump.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f cicndump
	rm -rf out

.PHONY: clean
