PREFIX  ?= /usr/local
SVCDIR  ?= /usr/lib/systemd/system
POLKIT  ?= /usr/share/polkit-1/actions
CFLAGS  := -O2 $(shell pkg-config --cflags libdrm)
LIBS    := $(shell pkg-config --libs libdrm) -lm

all: cosmic-hdr

cosmic-hdr: cosmic-hdr.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

CALDIR  ?= /usr/local/lib/cosmic-hdr

install: cosmic-hdr
	install -Dm755 cosmic-hdr                         $(DESTDIR)$(PREFIX)/bin/cosmic-hdr
	install -Dm755 hdr-game                           $(DESTDIR)$(PREFIX)/bin/hdr-game
	install -Dm644 cosmic-hdr.service                 $(DESTDIR)$(SVCDIR)/cosmic-hdr.service
	install -Dm644 cosmic-hdr.policy                  $(DESTDIR)$(POLKIT)/ru.sigmachan.cosmic-hdr.policy
	install -Dm755 hdr-cal.py                         $(DESTDIR)$(CALDIR)/hdr-cal.py

enable: install
	systemctl daemon-reload
	systemctl enable --now cosmic-hdr.service

disable:
	systemctl disable --now cosmic-hdr.service || true

uninstall: disable
	rm -f $(DESTDIR)$(PREFIX)/bin/cosmic-hdr
	rm -f $(DESTDIR)$(PREFIX)/bin/hdr-game
	rm -f $(DESTDIR)$(SVCDIR)/cosmic-hdr.service
	rm -f $(DESTDIR)$(POLKIT)/ru.sigmachan.cosmic-hdr.policy
	rm -rf $(DESTDIR)$(CALDIR)
	systemctl daemon-reload

clean:
	rm -f cosmic-hdr

.PHONY: all install enable disable uninstall clean
