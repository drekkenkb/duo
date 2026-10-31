# duo — two shells, tabs or split, that survive ssh disconnects
#
#   make                      build ./build/duo (run it from there without installing)
#   make install              copy it to $(PREFIX)/bin (default ~/.local/bin)
#   make uninstall            remove it, plus the autostart lines if you added them
#   make autostart            optional: start duo on SSH logins (edits ~/.bashrc / ~/.zshrc)
#   make uninstall-autostart  remove just the autostart lines
#   make check                terminal-emulator tests (make asan: same, sanitised)
#   make help                 this list
#
# System-wide: `make && sudo make install PREFIX=/usr/local`; give uninstall
# and autostart the same PREFIX, and run autostart without sudo.
# SHELLS="bash zsh" picks which rc files autostart edits; by default it's the
# ones that exist plus your login shell's.
# Settings like the prefix key live in src/config.h.

CC     ?= gcc
CFLAGS ?= -O2 -g
# forkpty, cfmakeraw and wcwidth all sit behind feature macros under -std=c11.
STD    := -std=c11 -D_GNU_SOURCE
WARN   := -Wall -Wextra -Wno-unused-parameter
LDLIBS := -lutil

PREFIX ?= $(HOME)/.local
BUILD  := build
SHELLS ?=

SRC     := $(wildcard src/*.c)
HDR     := $(wildcard src/*.h)
LIB_SRC := src/vterm.c src/buf.c src/mouse.c

.PHONY: all check asan install uninstall autostart uninstall-autostart help clean

all: $(BUILD)/duo

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/duo: $(SRC) $(HDR) | $(BUILD)
	$(CC) $(STD) $(CFLAGS) $(WARN) $(SRC) -o $@ $(LDLIBS)

$(BUILD)/vtcheck: $(LIB_SRC) tests/vtcheck.c $(HDR) | $(BUILD)
	$(CC) $(STD) $(CFLAGS) $(WARN) -Isrc $(LIB_SRC) tests/vtcheck.c -o $@

check: $(BUILD)/vtcheck
	@./$(BUILD)/vtcheck

asan: $(LIB_SRC) tests/vtcheck.c $(HDR) | $(BUILD)
	$(CC) $(STD) -O1 -g -fsanitize=address,undefined $(WARN) -Isrc \
		$(LIB_SRC) tests/vtcheck.c -o $(BUILD)/vtcheck-asan
	@./$(BUILD)/vtcheck-asan

install: $(BUILD)/duo
	install -Dm755 $(BUILD)/duo $(DESTDIR)$(PREFIX)/bin/duo
	@case ":$$PATH:" in *":$(PREFIX)/bin:"*) ;; \
	  *) echo "note: $(PREFIX)/bin isn't on your PATH; add it or run $(PREFIX)/bin/duo" ;; esac
	@echo "optional: 'make autostart' opens duo automatically on SSH logins"

# Running sessions are left alone: they end when both shells exit.
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/duo
	@sh scripts/autostart.sh remove
	@rmdir /tmp/duo-$$(id -u) 2>/dev/null || true
	@if pgrep -u "$$(id -u)" -x duo >/dev/null; then \
	  echo "note: duo sessions are still running; end them with ./$(BUILD)/duo -k (or pkill -x duo)"; \
	fi

autostart:
	@sh scripts/autostart.sh add "$(PREFIX)/bin/duo" $(SHELLS)

uninstall-autostart:
	@sh scripts/autostart.sh remove

help:
	@sed -n 's/^#   make/make/p' $(firstword $(MAKEFILE_LIST))

clean:
	rm -rf $(BUILD)
