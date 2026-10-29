CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags x11 xext libavformat libavcodec libswscale libavutil)
LIBS = $(shell pkg-config --libs x11 xext libavformat libavcodec libswscale libavutil)

TARGET = livepaper
SRC = livepaper.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
