DEFAULT_HOST!=../default-host.sh
HOST?=$(DEFAULT_HOST)
HOSTARCH!=../target-triplet-to-arch.sh $(HOST)

CFLAGS?=-O2 -g
CPPFLAGS?=

DESTDIR?=
PREFIX?=/usr/local
INCLUDEDIR?=$(PREFIX)/include
LIBDIR?=$(PREFIX)/lib

CFLAGS:=$(CFLAGS) -ffreestanding -Wall -Wextra
CPPFLAGS:=$(CPPFLAGS) -Iinclude

OBJS=\
errno/errno.o \
string/memcmp.o \
string/memcpy.o \
string/memmove.o \
string/memset.o \
string/strlen.o \
string/strcpy.o \
string/strcmp.o \
string/strncmp.o

TARGET=libc-common.a

.PHONY: all clean install install-headers install-libs
.SUFFIXES: .o .c

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $(OBJS)

.c.o:
	$(CC) -MD -c $< -o $@ -std=gnu11 $(CFLAGS) $(CPPFLAGS)

clean:
	rm -f $(TARGET)
	rm -f $(OBJS) *.o */*.o */*/*.o
	rm -f $(OBJS:.o=.d) *.d */*.d */*/*.d

install: install-headers install-libs

install-headers:
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(DESTDIR)$(INCLUDEDIR)/.

install-libs: $(TARGET)
	mkdir -p $(DESTDIR)$(LIBDIR)
	cp $(TARGET) $(DESTDIR)$(LIBDIR)

-include $(OBJS:.o=.d)
