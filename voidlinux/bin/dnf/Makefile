CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic
PREFIX  ?= /usr/sbin

all: build

build: dnf

dnf: dnf.c
	$(CC) $(CFLAGS) -o $@ dnf.c

test: dnf
	./test.sh

install: dnf
	install -D -m 0755 dnf $(DESTDIR)$(PREFIX)/dnf

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dnf

clean:
	rm -f dnf

.PHONY: all build test install uninstall clean
