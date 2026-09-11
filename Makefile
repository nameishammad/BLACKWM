CC = gcc

TARGET = blackwm
SRC = wm.c

# PKGS = xcb xcb-util xcb-keysyms xcb-icccm xcb-composite xcb-shape cairo cairo-xcb
PKGS = xcb xcb-util xcb-keysyms xcb-icccm xcb-composite xcb-shape xcb-cursor cairo cairo-xcb

CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags $(PKGS))
LIBS   = $(shell pkg-config --libs $(PKGS))

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
