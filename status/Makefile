CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lxcb-util -lxcb-keysyms -lxcb-icccm -lxcb-composite -lxcb-shape -lxcb-cursor -lcairo -lxcb-render -lxcb -lm

bar: statusbar.c
	$(CC) $(CFLAGS) -I/usr/include/cairo -I/usr/include/freetype2 -I/usr/include/libpng16 -I/usr/include/pixman-1 statusbar.c -o statusbar $(LDFLAGS)

clean:
	rm -f statusbar
