PREFIX  ?= /usr/local
SVCDIR  ?= /etc/systemd/system
CFLAGS  := -O2 $(shell pkg-config --cflags libdrm)
LIBS    := $(shell pkg-config --libs libdrm) -lm

all: cosmic-hdr

cosmic-hdr: cosmic-hdr.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

install: cosmic-hdr
	install -Dm755 cosmic-hdr     $(DESTDIR)$(PREFIX)/bin/cosmic-hdr
	install -Dm755 hdr-game     $(DESTDIR)$(PREFIX)/bin/hdr-game
	install -Dm644 cosmic-hdr.service $(DESTDIR)$(SVCDIR)/cosmic-hdr.service

enable: install
	systemctl daemon-reload
	systemctl enable --now cosmic-hdr.service

disable:
	systemctl disable --now cosmic-hdr.service || true

uninstall: disable
	rm -f $(DESTDIR)$(PREFIX)/bin/cosmic-hdr
	rm -f $(DESTDIR)$(PREFIX)/bin/hdr-game
	rm -f $(DESTDIR)$(SVCDIR)/cosmic-hdr.service
	systemctl daemon-reload

clean:
	rm -f cosmic-hdr

.PHONY: all install enable disable uninstall clean
