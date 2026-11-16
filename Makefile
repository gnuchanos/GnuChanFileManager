CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
DESTDIR ?=

APP := GnuChanFileManager
BUILD_DIR := build
VERSION := $(shell cat version.txt)

X11_PKGS := x11 xft
GIO_PKGS := gio-2.0 gdk-pixbuf-2.0
CFLAGS ?= -O2 -g
WARNFLAGS := -Wall -Wextra -Wpedantic -Werror
CPPFLAGS += -Isrc -D_GNU_SOURCE -DAPP_VERSION=\"$(VERSION)\"
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(X11_PKGS) $(GIO_PKGS))
PKG_GIO_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(GIO_PKGS))
LIBS := $(shell $(PKG_CONFIG) --libs $(X11_PKGS) $(GIO_PKGS)) -lpthread

SRCS := src/main.c src/gfm_file_ops.c
OBJS := $(BUILD_DIR)/main.o $(BUILD_DIR)/gfm_file_ops.o

all: $(APP)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/main.o: src/main.c src/gfm_file_ops.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNFLAGS) -std=c11 $(CPPFLAGS) $(PKG_CFLAGS) -c $< -o $@

$(BUILD_DIR)/gfm_file_ops.o: src/gfm_file_ops.c src/gfm_file_ops.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNFLAGS) -std=c11 $(CPPFLAGS) $(PKG_GIO_CFLAGS) -c $< -o $@

$(APP): $(OBJS)
	$(CC) $(CFLAGS) $(WARNFLAGS) $(OBJS) $(LIBS) -o $@

$(BUILD_DIR)/test_file_ops: tests/test_file_ops.c src/gfm_file_ops.c src/gfm_file_ops.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNFLAGS) -std=c11 -Isrc $(shell $(PKG_CONFIG) --cflags $(GIO_PKGS)) $< src/gfm_file_ops.c $(shell $(PKG_CONFIG) --libs $(GIO_PKGS)) -o $@

test: $(BUILD_DIR)/test_file_ops
	./$(BUILD_DIR)/test_file_ops

install: $(APP)
	install -Dm755 $(APP) $(DESTDIR)$(PREFIX)/bin/$(APP)
	install -Dm644 gnuchanfilemanager.desktop $(DESTDIR)$(PREFIX)/share/applications/gnuchanfilemanager.desktop
	install -Dm644 src/config/config.conf $(DESTDIR)$(PREFIX)/share/gnuchanfilemanager/config.conf
	install -Dm644 version.txt $(DESTDIR)$(PREFIX)/share/gnuchanfilemanager/version.txt
	if [ -d src/icons/ui ]; then \
		install -dm755 $(DESTDIR)$(PREFIX)/share/gnuchanfilemanager/icons/ui; \
		for p in src/icons/ui/*.png; do [ -e "$$p" ] && install -Dm644 "$$p" $(DESTDIR)$(PREFIX)/share/gnuchanfilemanager/icons/ui/$$(basename "$$p"); done; \
	fi

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(APP)
	rm -f $(DESTDIR)$(PREFIX)/share/applications/gnuchanfilemanager.desktop
	rm -rf $(DESTDIR)$(PREFIX)/share/gnuchanfilemanager

clean:
	rm -rf $(BUILD_DIR) $(APP)

.PHONY: all test install uninstall clean
