CC = gcc
CFLAGS = $(shell pkg-config --cflags --libs gtk+-3.0 cairo cairo-xcb xcb xcb-keysyms)
LIBS = $(shell pkg-config --libs x11 cairo cairo-xlib librsvg-2.0 glib-2.0) -lm

TARGET = applauncher
SRC = applauncher.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(SRC) -o $(TARGET) $(CFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)
