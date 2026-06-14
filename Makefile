PREFIX  ?= /usr/local
SVCDIR  ?= /etc/systemd/system
CFLAGS  := -O2 $(shell pkg-config --cflags libdrm)
LIBS    := $(shell pkg-config --libs libdrm) -lm

all: acid-hdr

acid-hdr: acid-hdr.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

install: acid-hdr
	install -Dm755 acid-hdr     $(DESTDIR)$(PREFIX)/bin/acid-hdr
	install -Dm755 hdr-game     $(DESTDIR)$(PREFIX)/bin/hdr-game
	install -Dm644 acid-hdr.service $(DESTDIR)$(SVCDIR)/acid-hdr.service

enable: install
	systemctl daemon-reload
	systemctl enable --now acid-hdr.service

disable:
	systemctl disable --now acid-hdr.service || true

uninstall: disable
	rm -f $(DESTDIR)$(PREFIX)/bin/acid-hdr
	rm -f $(DESTDIR)$(PREFIX)/bin/hdr-game
	rm -f $(DESTDIR)$(SVCDIR)/acid-hdr.service
	systemctl daemon-reload

clean:
	rm -f acid-hdr

.PHONY: all install enable disable uninstall clean
