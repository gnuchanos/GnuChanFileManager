CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
DESTDIR ?=
CFLAGS ?= -O2 -g
VERSION := $(shell cat version.txt)
WARNINGS = -Wall -Wextra -Wpedantic -Werror
GTK_PACKAGES = gtk+-3.0 gio-2.0
GIO_PACKAGES = gio-2.0
APP = GnuChanFileManager
APP_OBJECTS = build/main.o build/gfm_file_ops.o

.PHONY: all clean test install uninstall

all: $(APP)

build:
	mkdir -p build

build/main.o: src/main.c src/gfm_file_ops.h | build
	$(CC) $(CFLAGS) $(WARNINGS) -std=c11 -D_GNU_SOURCE -DAPP_VERSION=\"$(VERSION)\" $$($(PKG_CONFIG) --cflags $(GTK_PACKAGES)) -Isrc -c $< -o $@

build/gfm_file_ops.o: src/gfm_file_ops.c src/gfm_file_ops.h | build
	$(CC) $(CFLAGS) $(WARNINGS) -std=c11 $$($(PKG_CONFIG) --cflags $(GIO_PACKAGES)) -Isrc -c $< -o $@

$(APP): $(APP_OBJECTS)
	$(CC) $(CFLAGS) $(WARNINGS) $^ $$($(PKG_CONFIG) --libs $(GTK_PACKAGES)) -o $@

build/test_file_ops.o: tests/test_file_ops.c src/gfm_file_ops.h | build
	$(CC) $(CFLAGS) $(WARNINGS) -std=c11 $$($(PKG_CONFIG) --cflags $(GIO_PACKAGES)) -Isrc -c $< -o $@

build/test_file_ops: build/test_file_ops.o build/gfm_file_ops.o
	$(CC) $(CFLAGS) $(WARNINGS) $^ $$($(PKG_CONFIG) --libs $(GIO_PACKAGES)) -o $@

test: build/test_file_ops
	./build/test_file_ops

install: $(APP)
	install -Dm755 $(APP) "$(DESTDIR)$(PREFIX)/bin/$(APP)"
	install -Dm644 gnuchanfilemanager.desktop "$(DESTDIR)$(PREFIX)/share/applications/gnuchanfilemanager.desktop"
	install -Dm644 config.conf "$(DESTDIR)$(PREFIX)/share/gnuchanfilemanager/config.conf"
	install -Dm644 version.txt "$(DESTDIR)$(PREFIX)/share/gnuchanfilemanager/version.txt"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(APP)"
	rm -f "$(DESTDIR)$(PREFIX)/share/applications/gnuchanfilemanager.desktop"
	rm -f "$(DESTDIR)$(PREFIX)/share/gnuchanfilemanager/config.conf"
	rm -f "$(DESTDIR)$(PREFIX)/share/gnuchanfilemanager/version.txt"

clean:
	rm -rf build $(APP)
