# vmenu - dynamic menu
# See LICENSE file for copyright and license details.

include config.mk

SRC = drw.c vmenu.c stest.c util.c
OBJ = $(SRC:.c=.o)

all: vmenu stest

.c.o:
	$(CC) -c $(CFLAGS) $<

$(OBJ): arg.h config.mk drw.h

vmenu: vmenu.o drw.o util.o
	$(CC) -o $@ vmenu.o drw.o util.o $(LDFLAGS)

stest: stest.o
	$(CC) -o $@ stest.o $(LDFLAGS)

clean:
	rm -f vmenu stest $(OBJ) vmenu-$(VERSION).tar.gz

dist: clean
	mkdir -p vmenu-$(VERSION)
	cp LICENSE Makefile README arg.h config.mk vmenu.1\
		drw.h util.h vmenu_path vmenu_run stest.1 $(SRC)\
		vmenu-$(VERSION)
	tar -cf vmenu-$(VERSION).tar vmenu-$(VERSION)
	gzip vmenu-$(VERSION).tar
	rm -rf vmenu-$(VERSION)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f vmenu vmenu_path vmenu_run stest $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/vmenu
	chmod 755 $(DESTDIR)$(PREFIX)/bin/vmenu_path
	chmod 755 $(DESTDIR)$(PREFIX)/bin/vmenu_run
	chmod 755 $(DESTDIR)$(PREFIX)/bin/stest
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < vmenu.1 > $(DESTDIR)$(MANPREFIX)/man1/vmenu.1
	sed "s/VERSION/$(VERSION)/g" < stest.1 > $(DESTDIR)$(MANPREFIX)/man1/stest.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/vmenu.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/stest.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/vmenu\
		$(DESTDIR)$(PREFIX)/bin/vmenu_path\
		$(DESTDIR)$(PREFIX)/bin/vmenu_run\
		$(DESTDIR)$(PREFIX)/bin/stest\
		$(DESTDIR)$(MANPREFIX)/man1/vmenu.1\
		$(DESTDIR)$(MANPREFIX)/man1/stest.1

.PHONY: all clean dist install uninstall
