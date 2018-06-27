
CC?=gcc
CFLAGS ?=-Wall -std=gnu99 -ggdb3 -Wshadow -Wstrict-aliasing -Wstrict-overflow -Wno-missing-field-initializers

DESTDIR?=/
PREFIX?=usr

cmux: cmux.c
	$(CC) $(CFLAGS) cmux.c -o cmux

install: cmux
	install -m 0755 cmux $(DESTDIR)/$(PREFIX)/bin/cmux

clean:
	-@rm cmux

.PHONY: install clean
