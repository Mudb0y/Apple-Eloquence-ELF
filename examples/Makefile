CC      ?= gcc
CFLAGS  += -Wall -O2 -g

all: speak version

speak: speak.c
	$(CC) $(CFLAGS) $< -o $@ -ldl

version: version.c
	$(CC) $(CFLAGS) $< -o $@ -ldl

clean:
	rm -f speak version

.PHONY: all clean
