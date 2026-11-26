CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror
LDLIBS = -lmosquitto -lcjson

all: iot_display

iot_display: iot_display.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f iot_display

.PHONY: all clean
