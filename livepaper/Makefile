CC = gcc
CFLAGS = -Wall -Wextra -O2
LIBS = -lX11 -lavformat -lavcodec -lswscale -lavutil
TARGET = livepaper
SRC = livepaper.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
