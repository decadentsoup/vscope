SHELL		= /bin/sh

MAKE		= make

CC		= cc
CFLAGS		= -Werror -Wall -Wextra
LDFLAGS		=

INSTALL		= install
INSTALL_PROGRAM	= $(INSTALL)

DESTDIR		=
prefix		= /usr/local
exec_prefix	= $(prefix)
bindir		= $(exec_prefix)/bin

.SUFFIXES:
.PHONY: all install uninstall clean

all: vscope

vscope.o: vscope.c
	$(CC) -DVERSION=\"git-`git rev-parse HEAD`\" $(CFLAGS) `pkg-config --cflags libpulse sdl2 gl` -c $< -o $@

vscope: vscope.o
	$(CC) $(LDFLAGS) $^ -o $@ -lm `pkg-config --libs libpulse sdl2 gl`

install: all
	mkdir -p $(DESTDIR)$(bindir)
	$(INSTALL_PROGRAM) vscope $(DESTDIR)$(bindir)/vscope

uninstall:
	rm -f $(DESTDIR)$(bindir)/vscope

clean:
	rm -f vscope.o vscope
