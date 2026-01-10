DEFAULT_HOST!=../default-host.sh
HOST?=$(DEFAULT_HOST)
HOSTARCH!=../target-triplet-to-arch.sh $(HOST)

CC=$(HOST)-gcc
AR=$(HOST)-ar

CFLAGS?=-O2 -g
CPPFLAGS?=
LDFLAGS?=
LIBS?=

DESTDIR?=
PREFIX?=/usr/local
EXEC_PREFIX?=$(PREFIX)
INCLUDEDIR?=$(PREFIX)/include
LIBDIR?=$(EXEC_PREFIX)/lib

LIBC_OBJS = src/syscalls.o src/leb_syscalls.o src/dirent.o src/setjmp.o src/stdio.o src/string.o src/malloc.o src/errno.o src/ctype.o src/stdlib.o src/start.o src/stat.o src/resource.o

.PHONY: all clean install install-headers install-libs

clean:
	rm -f $(LIBC_OBJS) lib/libc.a src/*.o

install: install-headers install-libs

install-headers:
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(DESTDIR)$(INCLUDEDIR)/.

install-libs: lib/libc.a
	mkdir -p $(DESTDIR)$(LIBDIR)
	cp lib/libc.a $(DESTDIR)$(LIBDIR)
	cp lib/crt*.o $(DESTDIR)$(LIBDIR)