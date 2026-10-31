# ZDF-Downloader: grafische Oberfläche (zdf-downloader) und Terminal-Version
# (zdf-downloader-cli)
#
#   make            baut beide Programme
#   make deb        baut das Debian-Paket zdf-downloader_<Version>_<Architektur>.deb
#   make install    installiert nach $(PREFIX) (Standard: /usr/local, braucht sudo)

VERSION  := 1.1.1
DEBREV   := 1
ARCH     := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
PREFIX   ?= /usr/local
DESTDIR  ?=

CC       ?= gcc
CFLAGS   ?= -O2
CFLAGS   += -Wall -Wextra -pthread
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0)
GLIB_LIBS   := $(shell pkg-config --libs glib-2.0)

CORE := src/zd_core.c src/zd_core.h

all: zdf-downloader zdf-downloader-cli

zdf-downloader: src/zd_gui.c $(CORE)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ src/zd_gui.c src/zd_core.c $(GTK_LIBS)

zdf-downloader-cli: src/zd_cli.c $(CORE)
	$(CC) $(CFLAGS) $(GLIB_CFLAGS) -o $@ src/zd_cli.c src/zd_core.c $(GLIB_LIBS)

install: all
	install -Dm755 zdf-downloader "$(DESTDIR)$(PREFIX)/bin/zdf-downloader"
	install -Dm755 zdf-downloader-cli "$(DESTDIR)$(PREFIX)/bin/zdf-downloader-cli"
	install -Dm644 data/zdf-downloader.desktop \
		"$(DESTDIR)$(PREFIX)/share/applications/zdf-downloader.desktop"
	install -Dm644 data/zdf-downloader.svg \
		"$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/zdf-downloader.svg"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/zdf-downloader" "$(DESTDIR)$(PREFIX)/bin/zdf-downloader-cli"
	rm -f "$(DESTDIR)$(PREFIX)/share/applications/zdf-downloader.desktop"
	rm -f "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/zdf-downloader.svg"

deb: all
	rm -rf build/deb
	$(MAKE) install DESTDIR="$(CURDIR)/build/deb" PREFIX=/usr
	strip build/deb/usr/bin/zdf-downloader build/deb/usr/bin/zdf-downloader-cli
	install -d build/deb/DEBIAN
	sed -e 's/@VERSION@/$(VERSION)-$(DEBREV)/' -e 's/@ARCH@/$(ARCH)/' \
		-e "s/@SIZE@/$$(du -sk --apparent-size build/deb/usr | cut -f1)/" \
		data/control.in > build/deb/DEBIAN/control
	dpkg-deb --root-owner-group --build build/deb zdf-downloader_$(VERSION)-$(DEBREV)_$(ARCH).deb

clean:
	rm -rf build zdf-downloader zdf-downloader-cli

distclean: clean
	rm -f *.deb

.PHONY: all install uninstall deb clean distclean
