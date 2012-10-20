PREFIX=/usr/local

CFLAGS = -c -g -Wall -O0
CFLAGS += $(shell pkg-config --cflags gmime-2.6) $(shell pkg-config --cflags json)
LDFLAGS =
LDFLAGS += $(shell pkg-config --libs gmime-2.6) $(shell pkg-config --libs json)
CC = color-gcc

LIBDEST=$(PREFIX)/lib
HEADDEST=$(PREFIX)/include

all: mail2es.o
	${CC} $(LDFLAGS) mail2es.o -o mail2es

mail2es.o : mail2es.c
	${CC} $(CFLAGS) mail2es.c

clean:
	rm -f *.o mail2es
