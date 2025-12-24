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

LEBIRUN_OBJS = src/syscalls.o src/dirent.o

.PHONY: all clean install install-headers install-libs

all: lib/liblebirun.a

src/syscalls.o: src/syscalls.c
	$(CC) -c -ffreestanding -nostdinc -isystem include -O2 -o $@ $<

src/dirent.o: src/dirent.c
	$(CC) -c -ffreestanding -nostdinc -isystem include -O2 -o $@ $<

lib/liblebirun.a: $(LEBIRUN_OBJS)
	$(AR) rcs $@ $(LEBIRUN_OBJS)

clean:
	rm -f $(LEBIRUN_OBJS) lib/liblebirun.a src/*.o

install: install-headers install-libs

install-headers:
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(DESTDIR)$(INCLUDEDIR)/.

install-libs: lib/liblebirun.a
	mkdir -p $(DESTDIR)$(LIBDIR)
	cp lib/libc.a $(DESTDIR)$(LIBDIR)
	cp lib/crt*.o $(DESTDIR)$(LIBDIR)
	cp lib/liblebirun.a $(DESTDIR)$(LIBDIR)