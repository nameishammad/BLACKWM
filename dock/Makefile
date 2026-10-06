CC = gcc
CFLAGS = $(shell pkg-config --cflags x11 cairo cairo-xlib librsvg-2.0 glib-2.0)
LIBS = $(shell pkg-config --libs x11 cairo cairo-xlib librsvg-2.0 glib-2.0) -lm

TARGET = my_dock
SRC = my_dock.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(SRC) -o $(TARGET) $(CFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)
