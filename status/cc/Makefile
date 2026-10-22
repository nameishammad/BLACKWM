# Compiler aur flags
CC = gcc
CFLAGS = -O2 $(shell pkg-config --cflags cairo cairo-xcb x11 xcb xcb-aux xcb-icccm xft libpulse alsa)
LIBS = $(shell pkg-config --libs cairo cairo-xcb x11 xcb xcb-aux xcb-icccm xft libpulse alsa) -lpthread -lm

# Target executable aur source files
TARGET = blackcc
SRC = cc.c

# Default rule (make chalane par yeh execute hoga)
all: $(TARGET)

# Executable banane ki rule
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LIBS)

# Extra generated files ko clean karne ke liye rule
clean:
	rm -f $(TARGET)

.PHONY: all clean
