CC      = gcc
CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags x11 xcomposite xrender xfixes xext)
LDLIBS  = $(shell pkg-config --libs x11 xcomposite xrender xfixes xext) -lm

overview: overview.c
	$(CC) $(CFLAGS) -o overview overview.c $(LDLIBS)

clean:
	rm -f overview

.PHONY: clean
