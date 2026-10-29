CC      = cc
PKGS    = x11 xext xrender xcomposite xdamage cairo cairo-xlib
CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags $(PKGS))
LDLIBS  = $(shell pkg-config --libs $(PKGS)) -lm
PREFIX  = /usr/local

all: voverview

wsoverview: voverview.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: voverview
	install -Dm755 voverview $(DESTDIR)$(PREFIX)/bin/voverview

clean:
	rm -f voverview

.PHONY: all install clean
