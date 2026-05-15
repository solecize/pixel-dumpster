# tools/Makefile — Build host tools for pixel-dumpster

CC       ?= cc
CFLAGS   ?= -Wall -Wextra -O2
CURL_LIB  = -lcurl
CURSES_LIB = -lncurses

TARGETS = dumpster-diver content-cli playlist-cli wizard-cli

.PHONY: all clean

all: $(TARGETS)

dumpster-diver: dumpster-diver.c discovery.c discovery.h cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o $@ dumpster-diver.c discovery.c cJSON.c $(CURL_LIB) -lpthread

content-cli: content-cli.c
	$(CC) $(CFLAGS) -o $@ $< $(CURSES_LIB) $(CURL_LIB)

playlist-cli: playlist-cli.c
	$(CC) $(CFLAGS) -o $@ $< $(CURSES_LIB) $(CURL_LIB)

wizard-cli: wizard-cli.c
	$(CC) $(CFLAGS) -o $@ $< $(CURSES_LIB)

clean:
	rm -f $(TARGETS)
