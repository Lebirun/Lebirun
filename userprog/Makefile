CC = i686-elf-gcc
LD = i686-elf-ld

LIBC = ../libc
LIBC_ABS = $(abspath $(LIBC))
KERNEL_DIR = ../kernel/arch/i386
INITRD_DIR = ../initrd
NCURSES_DIR = ncurses-6.6
NANO_DIR = nano-8.7.1
HTOP_DIR = htop-3.4.1
HTOP_BUILD = htop-3.4.1-build
SYSROOT_LIB = ../root/lib

CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -m32 -Os -fomit-frame-pointer -nostdinc
CPPFLAGS = -isystem $(LIBC_ABS)/leblibc/include -isystem $(LIBC_ABS)/leblibc/build-i386/obj/include -isystem $(LIBC_ABS)/leblibc/arch/i386 -isystem $(LIBC_ABS)/leblibc/arch/generic -I$(LIBC_ABS)/include -I$(LIBC_ABS)/src -I$(NCURSES_DIR)/include

SO_CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fPIC -m32 -O0 -nostdinc -fvisibility=default

CRT1 = $(LIBC)/leblibc/build-i386/lib/crt1.o
CRTI = $(LIBC)/leblibc/build-i386/lib/crti.o
CRTN = $(LIBC)/leblibc/build-i386/lib/crtn.o

CRT1_ABS = $(LIBC_ABS)/leblibc/build-i386/lib/crt1.o
CRTI_ABS = $(LIBC_ABS)/leblibc/build-i386/lib/crti.o
CRTN_ABS = $(LIBC_ABS)/leblibc/build-i386/lib/crtn.o
LIBC_A = $(LIBC)/leblibc/build-i386/lib/libc.a
NCURSES_A = $(INITRD_DIR)/libncurses.a

PROGRAMS = pong.bin

MAKEFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
LSH_DIR := $(MAKEFILE_DIR)lsh
LSH_BUILD := $(LSH_DIR)/build-cross
LSH_OUTDIR := $(LSH_BUILD)/.build
HOSTCC ?= gcc
LSH_CONFIGURE_ARGS = --host=i686-elf --prefix=/usr

KERNEL_USER_SHELL = ../kernel/arch/i386/user_shell.bin
SYSROOT_BIN = ../root/bin

.PHONY: copy-shell copy-pong stage-lsh stage-coreutils ncurses nano htop

copy-shell: lsh.bin
	cp lsh.bin $(KERNEL_USER_SHELL)
	@echo "Using lsh.bin as user shell"

copy-pong: pong.bin
	cp pong.bin $(INITRD_DIR)/pong.bin

stage-lsh: lsh.bin
	mkdir -p $(SYSROOT_BIN)
	cp lsh.bin $(SYSROOT_BIN)/lsh
	ln -sf lsh $(SYSROOT_BIN)/sh

stage-coreutils:
	$(MAKE) -C coreutils stage

ncurses:
	@echo "Building ncurses in $(NCURSES_DIR)"
	@test -d "$(NCURSES_DIR)" || { echo "ERROR: missing $(NCURSES_DIR)"; exit 1; }
	@if [ ! -f "$(LIBC_ABS)/leblibc/build-i386/lib/crti.o" ]; then \
		echo "Building leblibc runtime objects..."; \
		$(MAKE) -C "$(LIBC_ABS)/leblibc" build-i386 || (cat "$(LIBC_ABS)/leblibc/build-i386/config.log" >&2; false); \
	fi
	@echo "Installing leblibc headers to sysroot..."
	@$(MAKE) -C "$(LIBC_ABS)/leblibc" install-headers DESTDIR="$(abspath ..)/sysroot" ARCH=i386 >/dev/null 2>&1 || true
	@cd $(NCURSES_DIR) && \
		export LEB_ROOT=$(abspath ..); \
		export SYSROOT=$$LEB_ROOT/sysroot; \
		export CC=i686-elf-gcc; \
		export CPP="$$CC -E"; \
		export AR=i686-elf-ar; export RANLIB=i686-elf-ranlib; \
		export CFLAGS="--sysroot=$$SYSROOT -isystem=$$SYSROOT/usr/include -I$$LEB_ROOT/libc/leblibc/include -isystem=$$LEB_ROOT/libc/leblibc/build-i386/obj/include -isystem=$$LEB_ROOT/libc/leblibc/arch/i386 -isystem=$$LEB_ROOT/libc/leblibc/arch/generic -Os -fno-stack-protector -fno-pic -fno-builtin"; \
		export CPPFLAGS="-I$$SYSROOT/usr/include -I$$LEB_ROOT/libc/leblibc/include -isystem=$$LEB_ROOT/libc/leblibc/build-i386/obj/include -isystem=$$LEB_ROOT/libc/leblibc/arch/i386 -isystem=$$LEB_ROOT/libc/leblibc/arch/generic"; \
		export LDFLAGS="-nostdlib -static -T $$LEB_ROOT/libc/user.ld"; \
		./configure --host=i686-elf --prefix=/usr \
			--without-cxx --without-cxx-binding --without-ada \
			--without-progs --without-tests --without-manpages --without-dlsym \
			--without-shared --with-static --with-normal \
			--with-terminfo-dirs=/usr/share/terminfo --with-default-terminfo-dir=/usr/share/terminfo \
			--disable-database --with-fallbacks=linux,vt100,vt220,xterm --enable-termcap \
			--with-term-driver=terminfo \
			--disable-largefile --disable-stripping --enable-sigwinch \
			cf_cv_working_poll=yes ac_cv_func_poll=yes cf_cv_func_nanosleep=yes \
			ac_cv_type_pid_t=yes ac_cv_type_ssize_t=yes ac_cv_type_wchar_t=yes \
			ac_cv_sizeof_wchar_t=4 ac_cv_sizeof_bool=1 ac_cv_sizeof_signed_char=1 \
			ac_cv_header_termio_h=no ac_cv_header_termios_h=yes ac_cv_header_sgtty_h=no \
		cf_cv_have_tcgetattr=yes \
			ac_cv_func_mbtowc=yes ac_cv_func_mblen=yes ac_cv_func_mbrtowc=yes ac_cv_func_mbrlen=yes \
			ac_cv_func_wctombs=yes ac_cv_func_wctomb=yes ac_cv_func_wctob=yes ac_cv_func_btowc=yes ac_cv_func_setenv=yes ac_cv_func_putenv=yes ac_cv_func_unsetenv=yes ac_cv_func_getcwd=yes ac_cv_func_getwd=yes ; \
		$(MAKE); \
		$(MAKE) install DESTDIR=$$SYSROOT; \
		ln -sf libncursesw.a $$SYSROOT/usr/lib/libncurses.a

nano: ncurses
	@echo "Building nano in $(NANO_DIR)"
	@test -d "$(NANO_DIR)" || { echo "ERROR: missing $(NANO_DIR)"; exit 1; }
	@cd $(NANO_DIR) && \
		export LEB_ROOT=$(abspath ..); \
		export SYSROOT=$$LEB_ROOT/sysroot; \
		export CC=i686-elf-gcc; \
		export CPP="$$CC -E"; \
		export AR=i686-elf-ar; export RANLIB=i686-elf-ranlib; \
			export CFLAGS="--sysroot=$$SYSROOT -isystem=$$SYSROOT/usr/include -Os -fno-stack-protector -fno-pic -fno-builtin -DGNULIB_defined_verify_NSIG_constraint=1"; \
		export CPPFLAGS="-I$$SYSROOT/usr/include -D_GNU_SOURCE"; \
		export LDFLAGS="-nostdlib -static -T $$LEB_ROOT/libc/user.ld"; \
		export PKG_CONFIG_PATH=""; \
		export NCURSES_CFLAGS="-I$$SYSROOT/usr/include"; \
		export NCURSES_LIBS="-L$$SYSROOT/usr/lib -lncurses -lc -lgcc"; \
		export LIBS=""; \
		./configure --host=i686-elf --prefix=/usr \
			--disable-nls --disable-utf8 --disable-browser --disable-speller \
			--disable-operatingdir --disable-wordcomp --disable-extra --enable-tiny \
			--disable-largefile --disable-threads \
			ac_cv_func_getpwent=no ac_cv_func_getpwnam=no ac_cv_func_getpwuid=no \
			ac_cv_func_getgrnam=no ac_cv_func_nl_langinfo=no ac_cv_func_iconv=no \
			ac_cv_func_iswalnum=no ac_cv_header_glob_h=no ac_cv_func_snprintf=yes \
			ac_cv_func_vsnprintf=yes ac_cv_func_getcwd=yes ac_cv_func_getwd=yes \
			ac_cv_func_fcntl=yes ac_cv_func___fseterr=yes ac_cv_func_gettimeofday=yes \
			ac_cv_func_clock_gettime=yes ac_cv_func_clock_getres=yes \
			ac_cv_func_sigaction=yes ac_cv_func_readdir=yes ac_cv_func_opendir=yes \
			ac_cv_func_closedir=yes ac_cv_func_sigprocmask=yes ac_cv_func_sigemptyset=yes \
			ac_cv_func_sigaddset=yes ac_cv_func_sigdelset=yes ac_cv_func_sigfillset=yes \
			ac_cv_func_sigismember=yes ac_cv_func_sigpending=yes ; \
		python -c 'import os,pathlib; p=pathlib.Path("src/Makefile"); s=p.read_text(); old="$$(AM_V_CCLD)$$(LINK) $$(nano_OBJECTS) $$(nano_LDADD) $$(LIBS)"; root=os.environ["LEB_ROOT"]; new=f"$$(AM_V_CCLD)$$(LINK) {root}/libc/leblibc/build-i386/lib/crt1.o {root}/libc/leblibc/build-i386/lib/crti.o $$(nano_OBJECTS) $$(nano_LDADD) $$(LIBS) {root}/libc/leblibc/build-i386/lib/crtn.o"; p.write_text(s.replace(old,new,1))'; \
		$(MAKE); \
		cp src/nano $$LEB_ROOT/root/bin/nano

htop: ncurses
	@echo "Building htop in $(HTOP_DIR)"
	@test -d "$(HTOP_DIR)" || { echo "ERROR: missing $(HTOP_DIR)"; exit 1; }
	@if [ ! -f "$(LIBC_ABS)/leblibc/build-i386/lib/crti.o" ]; then \
		echo "Building leblibc runtime objects..."; \
		$(MAKE) -C "$(LIBC_ABS)/leblibc" build-i386 || (cat "$(LIBC_ABS)/leblibc/build-i386/config.log" >&2; false); \
	fi
	@echo "Installing sysroot headers..."
	@$(MAKE) -C "$(LIBC_ABS)/leblibc" install-headers DESTDIR="$(abspath ../sysroot)" ARCH=i386 >/dev/null 2>&1
	@$(MAKE) -C "$(LIBC_ABS)/leblibc" install-libs DESTDIR="$(abspath ../sysroot)" >/dev/null 2>&1
	@mkdir -p $(HTOP_BUILD)
	@if [ ! -f "$(HTOP_BUILD)/Makefile" ]; then \
		echo "Configuring htop..."; \
		rm -rf $(HTOP_BUILD); \
		mkdir -p $(HTOP_BUILD); \
		cd $(HTOP_BUILD) && \
		export LEB_ROOT=$(abspath ..); \
		export SYSROOT=$$LEB_ROOT/sysroot; \
		export CC=i686-elf-gcc; \
		export CPP="$$CC -E"; \
		export AR=i686-elf-ar; export RANLIB=i686-elf-ranlib; \
		rm -f $$SYSROOT/usr/lib/crt0.o; \
		../$(HTOP_DIR)/configure --host=i686-elf --prefix=/usr \
			--disable-unicode --disable-hwloc --with-ncurses \
			CFLAGS="--sysroot=$$SYSROOT -isystem $$SYSROOT/usr/include -Os -fno-stack-protector -fno-pic -nostartfiles" \
			LDFLAGS="--sysroot=$$SYSROOT -nostartfiles -L$$SYSROOT/usr/lib" \
			LIBS="$$SYSROOT/usr/lib/crt1.o $$SYSROOT/usr/lib/crti.o $$SYSROOT/usr/lib/crtn.o -lncurses -lm -lc" \
			ac_cv_func_malloc_0_nonnull=yes ac_cv_func_realloc_0_nonnull=yes \
			ac_cv_func_calloc_0_nonnull=yes ac_cv_sizeof_long=4 ac_cv_sizeof_int=4 \
			> configure.log 2>&1 || (cat configure.log >&2; false); \
	fi
	@echo "Building htop..."
	@$(MAKE) -C $(HTOP_BUILD) > /dev/null 2>&1 || ($(MAKE) -C $(HTOP_BUILD) && false)
	@cp $(HTOP_BUILD)/htop $(SYSROOT_BIN)/htop
	@echo "htop installed to $(SYSROOT_BIN)/htop"

lsh.bin:
	@mkdir -p $(LSH_BUILD)
	@if [ ! -f "$(LSH_OUTDIR)/config.mk" ] || [ "$(LSH_DIR)/configure" -nt "$(LSH_OUTDIR)/config.mk" ]; then \
		echo "Configuring lsh..."; \
		mkdir -p "$(LSH_BUILD)"; \
		(cd "$(LSH_BUILD)" && sh ../configure $(LSH_CONFIGURE_ARGS) > "$(LSH_BUILD)/configure.log" 2>&1) || (cat "$(LSH_BUILD)/configure.log" >&2; false); \
	fi
	@if [ ! -f "$(LIBC_ABS)/leblibc/build-i386/lib/crti.o" ]; then \
		echo "Building leblibc runtime objects..."; \
		$(MAKE) -C "$(LIBC_ABS)/leblibc" build-i386 || (cat "$(LIBC_ABS)/leblibc/build-i386/config.log" >&2; false); \
	fi; \
	$(MAKE) -C $(LSH_DIR) OUTDIR="$(LSH_OUTDIR)" clean CFLAGS="-Wall -Wextra -fno-builtin -fno-stack-protector -fno-pic -m32 -O2 -nostdinc -isystem $(LIBC_ABS)/leblibc/include -isystem $(LIBC_ABS)/leblibc/build-i386/obj/include -isystem $(LIBC_ABS)/leblibc/arch/i386 -isystem $(LIBC_ABS)/leblibc/arch/generic" LDFLAGS="-nostdlib -static -Wl,-z,noexecstack -T $(LIBC_ABS)/user.ld" LIBS="$(CRT1_ABS) $(CRTI_ABS) -L$(LIBC_ABS)/leblibc/build-i386/lib -lc $(CRTN_ABS) -lgcc" lsh
	@cp "$(LSH_DIR)/lsh" "$(MAKEFILE_DIR)lsh.bin"

.PHONY: all clean

all: ncurses nano htop $(PROGRAMS) copy-shell copy-pong stage-lsh stage-coreutils

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@



%.bin: %.o $(CRT1) $(CRTI) $(CRTN) $(LIBC_A)
	$(CC) -nostdlib -static -Wl,-z,noexecstack -T $(LIBC)/user.ld -L$(LIBC)/leblibc/build-i386/lib -o $@ $(CRT1) $(CRTI) $< -lc $(CRTN) -lgcc

clean:
	rm -f *.o *.bin *.so
	rm -f $(KERNEL_USER_SHELL)
	rm -f $(INITRD_DIR)/pong.bin
	rm -f $(INITRD_DIR)/stdio_test.bin
	rm -f $(SYSROOT_BIN)/lsh
	rm -f $(SYSROOT_BIN)/sh
	rm -f lsh.bin
	rm -f $(SYSROOT_BIN)/nano
	rm -f $(SYSROOT_BIN)/htop
	@if [ -d "$(NCURSES_DIR)" ]; then $(MAKE) -C $(NCURSES_DIR) clean; fi
	@if [ -f "$(NANO_DIR)/Makefile" ]; then $(MAKE) -C $(NANO_DIR) clean || true; fi
	@if [ -f "$(HTOP_BUILD)/Makefile" ]; then $(MAKE) -C $(HTOP_BUILD) clean || true; fi
