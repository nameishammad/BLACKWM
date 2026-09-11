CC      = cc
PKGS    = x11 xext xrender xcomposite xdamage cairo cairo-xlib
CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags $(PKGS))
LDLIBS  = $(shell pkg-config --libs $(PKGS))
PREFIX  = /usr/local

all: wsoverview

wsoverview: wsoverview.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: wsoverview
	install -Dm755 wsoverview $(DESTDIR)$(PREFIX)/bin/wsoverview

clean:
	rm -f wsoverview

.PHONY: all install clean
