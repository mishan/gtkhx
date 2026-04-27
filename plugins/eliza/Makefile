LDFLAGS = -ggdb
CFLAGS = -Wall -fPIC
CC = gcc

all: eliza.so

eliza.so:	eliza.c
	$(CC) $(LDFLAGS) $(CFLAGS) -I../../ -DSKIPCONFIG -shared -Wl,-soname,eliza.so -o $@ $< -lc

distclean: clean

clean:
	rm -f *.so *.o

install: eliza.so
