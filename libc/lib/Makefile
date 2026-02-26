CC = i686-elf-gcc
AR = i686-elf-ar
RANLIB = i686-elf-ranlib

LIBC = ..
LIBC_ABS = $(abspath $(LIBC))
LEB_ROOT = $(abspath ../..)
SYSROOT = $(LEB_ROOT)/sysroot
NCURSES_DIR = ncurses-6.6

.PHONY: ncurses clean

ncurses:
	@echo "Building ncurses in $(NCURSES_DIR)"
	@test -d "$(NCURSES_DIR)" || { echo "ERROR: missing $(NCURSES_DIR)"; exit 1; }
	@if [ ! -f "$(LIBC_ABS)/leblibc/build-i386/lib/crti.o" ]; then \
		echo "Building leblibc runtime objects..."; \
		$(MAKE) -C "$(LIBC_ABS)/leblibc" build-i386 || (cat "$(LIBC_ABS)/leblibc/build-i386/config.log" >&2; false); \
	fi
	@echo "Installing leblibc headers to sysroot..."
	@$(MAKE) -C "$(LIBC_ABS)/leblibc" install-headers DESTDIR="$(SYSROOT)" ARCH=i386 >/dev/null 2>&1 || true
	@cd $(NCURSES_DIR) && \
		export LEB_ROOT=$(LEB_ROOT); \
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

clean:
	@if [ -d "$(NCURSES_DIR)" ] && [ -f "$(NCURSES_DIR)/Makefile" ]; then \
		cd $(NCURSES_DIR) && $(MAKE) clean 2>/dev/null || true; \
	fi
	@rm -rf $(NCURSES_DIR)/config.cache
	@find $(NCURSES_DIR) -type f -name '*.o' -delete 2>/dev/null || true
