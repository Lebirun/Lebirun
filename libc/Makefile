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
stdio/snprintf.o \
stdio/sprintf.o \
stdio/fprintf.o \
stdio/sscanf.o \
stdio/file.o \
stdio/putchar.o \
stdio/puts.o \
stdio/getchar.o \
stdio/perror.o \
stdlib/abort.o \
stdlib/assert.o \
stdlib/exit.o \
stdlib/atoi.o \
stdlib/atol.o \
stdlib/strtol.o \
stdlib/strtoul.o \
stdlib/malloc.o \
stdlib/abs.o \
stdlib/env.o \
stdlib/stdlib_ext.o \
syscall/syscalls.o \
unistd/unistd.o \
	termios/getset.o \
	termios/controls.o \
	termios/speed.o \
	termios/raw.o \
	termios/pgrp.o \
	termios/ioctl.o \
	string/strncpy.o \
	string/strcat.o \
string/strncat.o \
string/strchr.o \
string/strrchr.o \
string/strstr.o \
string/memchr.o \
string/mempcpy.o \
string/strdup.o \
string/strspn.o \
string/strcspn.o \
string/strtok.o \
string/strerror.o \
string/string_ext.o \
graphics/graphics.o \
graphics/bar.o \
graphics/squares.o \
graphics/circle.o \
stat/stat.o \
signal/signal.o \
fcntl/fcntl.o \
time/time.o \
setjmp/setjmp.o \
sgtty/sgtty.o \
locale/locale.o \
wchar/wchar.o \
select/select.o \
pwd/pwd.o \
dirent/dirent.o \
mman/mman.o \
wait/wait.o \
inttypes/inttypes.o \
fnmatch/fnmatch.o \
regex/regex.o \
getopt/getopt.o \
term/term.o \
uchar/uchar.o \
glob/glob.o \
error/error.o \
libgen/libgen.o \
sched/sched.o \
math/math.o \
utime/utime.o \
iconv/iconv.o \
spawn/spawn.o \
random/random.o \
gettext/gettext.o

HOSTEDOBJS=\
$(ARCH_HOSTEDOBJS) \

OBJS=\
$(FREEOBJS) \
$(HOSTEDOBJS) \

LIBK_OBJS=$(FREEOBJS:.o=.libk.o)

BINARIES=libc.a libk.a

.PHONY: all clean install install-headers install-libs libc-common
.SUFFIXES: .o .libk.o .c .S

all: libc-common $(BINARIES) crt0

libc-common:
	$(MAKE) -C $(LIBC_COMMON)

crt0: crt/crt0.o
	cp crt/crt0.o lib/crt0.o

crt/crt0.o: crt/crt0.c
	$(CC) -MD -c $< -o $@ -std=gnu11 $(CFLAGS) $(CPPFLAGS)

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
	rm -f lib/crt0.o crt/crt0.o crt/crt0.d
	$(MAKE) -C $(LIBC_COMMON) clean

install: install-headers install-libs

install-headers:
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(DESTDIR)$(INCLUDEDIR)/.
	cp -R --preserve=timestamps $(LIBC_COMMON)/include/. $(DESTDIR)$(INCLUDEDIR)/.

install-libs: $(BINARIES) crt/crt0.o
	mkdir -p $(DESTDIR)$(LIBDIR)
	cp $(BINARIES) $(DESTDIR)$(LIBDIR)
	cp crt/crt0.o $(DESTDIR)$(LIBDIR)

-include $(OBJS:.o=.d)
-include $(LIBK_OBJS:.o=.d)
