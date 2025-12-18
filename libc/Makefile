DEFAULT_HOST!=../default-host.sh
HOST?=$(DEFAULT_HOST)
HOSTARCH!=../target-triplet-to-arch.sh $(HOST)

CFLAGS?=-O2 -g
CPPFLAGS?=
LDFLAGS?=
LIBS?=

DESTDIR?=
PREFIX?=/usr/local
EXEC_PREFIX?=$(PREFIX)
INCLUDEDIR?=$(PREFIX)/include
LIBDIR?=$(EXEC_PREFIX)/lib

LIBC_COMMON=../libc-common

CFLAGS:=$(CFLAGS) -ffreestanding -Wall -Wextra
CPPFLAGS:=$(CPPFLAGS) -D__is_libc -Iinclude -I$(LIBC_COMMON)/include
LIBK_CFLAGS:=$(CFLAGS)
LIBK_CPPFLAGS:=$(CPPFLAGS) -D__is_libk -I../kernel/include

ARCHDIR=arch/$(HOSTARCH)

include $(ARCHDIR)/make.config

CFLAGS:=$(CFLAGS) $(ARCH_CFLAGS)
CPPFLAGS:=$(CPPFLAGS) $(ARCH_CPPFLAGS)
LIBK_CFLAGS:=$(LIBK_CFLAGS) $(KERNEL_ARCH_CFLAGS)
LIBK_CPPFLAGS:=$(LIBK_CPPFLAGS) $(KERNEL_ARCH_CPPFLAGS)

COMMON_OBJS=\
$(LIBC_COMMON)/errno/errno.o \
$(LIBC_COMMON)/string/memcmp.o \
$(LIBC_COMMON)/string/memcpy.o \
$(LIBC_COMMON)/string/memmove.o \
$(LIBC_COMMON)/string/memset.o \
$(LIBC_COMMON)/string/strlen.o \
$(LIBC_COMMON)/string/strcpy.o \
$(LIBC_COMMON)/string/strcmp.o \
$(LIBC_COMMON)/string/strncmp.o

FREEOBJS=\
$(ARCH_FREEOBJS) \
stdio/printf.o \
stdio/putchar.o \
stdio/puts.o \
stdio/getchar.o \
stdlib/abort.o \
stdlib/exit.o \
syscall/syscalls.o \
graphics/graphics.o \
graphics/bar.o \
graphics/squares.o \
graphics/circle.o

HOSTEDOBJS=\
$(ARCH_HOSTEDOBJS) \

OBJS=\
$(FREEOBJS) \
$(HOSTEDOBJS) \

LIBK_OBJS=$(FREEOBJS:.o=.libk.o)

BINARIES=libc.a libk.a

.PHONY: all clean install install-headers install-libs libc-common
.SUFFIXES: .o .libk.o .c .S

all: libc-common $(BINARIES)

libc-common:
	$(MAKE) -C $(LIBC_COMMON)

libc.a: $(OBJS) $(COMMON_OBJS)
	$(AR) rcs $@ $(OBJS) $(COMMON_OBJS)

libk.a: $(LIBK_OBJS) $(COMMON_OBJS)
	$(AR) rcs $@ $(LIBK_OBJS) $(COMMON_OBJS)

.c.o:
	$(CC) -MD -c $< -o $@ -std=gnu11 $(CFLAGS) $(CPPFLAGS)

.c.S:
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

.c.libk.o:
	$(CC) -MD -c $< -o $@ -std=gnu11 $(LIBK_CFLAGS) $(LIBK_CPPFLAGS)

.S.libk.o:
	$(CC) -MD -c $< -o $@ $(LIBK_CFLAGS) $(LIBK_CPPFLAGS)

clean:
	rm -f $(BINARIES) *.a
	rm -f $(OBJS) $(LIBK_OBJS) *.o */*.o */*/*.o
	rm -f $(OBJS:.o=.d) $(LIBK_OBJS:.o=.d) *.d */*.d */*/*.d
	$(MAKE) -C $(LIBC_COMMON) clean

install: install-headers install-libs

install-headers:
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(DESTDIR)$(INCLUDEDIR)/.
	cp -R --preserve=timestamps $(LIBC_COMMON)/include/. $(DESTDIR)$(INCLUDEDIR)/.

install-libs: $(BINARIES)
	mkdir -p $(DESTDIR)$(LIBDIR)
	cp $(BINARIES) $(DESTDIR)$(LIBDIR)

-include $(OBJS:.o=.d)
-include $(LIBK_OBJS:.o=.d)
