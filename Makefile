PREFIX ?= /usr/local
CFLAGS := -O2 $(shell pkg-config --cflags libdrm)
LIBS   := $(shell pkg-config --libs libdrm) -lm

all: acid-hdr

acid-hdr: acid-hdr.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

install: acid-hdr
	install -Dm755 acid-hdr $(DESTDIR)$(PREFIX)/bin/acid-hdr

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/acid-hdr

clean:
	rm -f acid-hdr

.PHONY: all install uninstall clean
