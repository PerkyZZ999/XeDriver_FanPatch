CC ?= gcc
CFLAGS = -O2 -Wall -Wextra -std=gnu11
TARGET = xe_pcode_probe

all: $(TARGET)

$(TARGET): xe_pcode_probe.c pcode_protocol.h
	$(CC) $(CFLAGS) -o $@ xe_pcode_probe.c

clean:
	rm -f $(TARGET)

.PHONY: all clean
