CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LIBS    ?= -lxbps
PREFIX  ?= /usr/sbin

all: build

build: dnf

dnf: dnf.c xbps_backend.c xbps_backend.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ dnf.c xbps_backend.c $(LIBS)

test: dnf
	./test.sh

install: dnf
	install -D -m 0755 dnf $(DESTDIR)$(PREFIX)/dnf

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dnf

clean:
	rm -f dnf

.PHONY: all build test install uninstall clean
