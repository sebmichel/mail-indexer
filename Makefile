PREFIX=/usr/local

CFLAGS = -c -g -Wall -O0
CFLAGS += $(shell pkg-config --cflags gmime-2.6) $(shell pkg-config --cflags json)
LDFLAGS =
LDFLAGS += $(shell pkg-config --libs gmime-2.6) $(shell pkg-config --libs json)
CC = color-gcc

LIBDEST=$(PREFIX)/lib
HEADDEST=$(PREFIX)/include

all: mail-indexer.o
	${CC} $(LDFLAGS) mail-indexer.o -o mail-indexer

mail-indexer.o : mail-indexer.c
	${CC} $(CFLAGS) mail-indexer.c

clean:
	rm -f *.o mail-indexer
