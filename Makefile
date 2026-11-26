CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -pthread
LDLIBS = -lmosquitto -lcjson
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
DESTDIR ?=

all: iot_display

iot_display: iot_display.c monitor_logic.c monitor_logic.h
	$(CC) $(CPPFLAGS) $(CFLAGS) iot_display.c monitor_logic.c $(LDFLAGS) -o $@ $(LDLIBS)

tests/test_monitor: tests/test_monitor.c monitor_logic.c monitor_logic.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. tests/test_monitor.c monitor_logic.c $(LDFLAGS) -o $@ -lcjson

test: iot_display tests/test_monitor
	./tests/test_monitor
	sh tests/test_cli.sh

integration-test: iot_display
	python3 tests/test_integration.py

install: iot_display
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 iot_display "$(DESTDIR)$(BINDIR)/iot_display"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/iot_display"

clean:
	rm -f iot_display tests/test_monitor

.PHONY: all clean test integration-test install uninstall
