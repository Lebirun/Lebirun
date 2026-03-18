CC = x86_64-elf-gcc
AR = x86_64-elf-ar
RANLIB = x86_64-elf-ranlib

LIBC = ..
LIBC_ABS = $(abspath $(LIBC))
LEB_ROOT = $(abspath ../..)
SYSROOT = $(LEB_ROOT)/sysroot
NCURSES_DIR = ncurses-6.6

.PHONY: ncurses clean

ncurses:
	@echo "Building ncurses in $(NCURSES_DIR)"
	@test -d "$(NCURSES_DIR)" || { echo "ERROR: missing $(NCURSES_DIR)"; exit 1; }
	@if [ ! -f "$(LIBC_ABS)/leblibc/build-x86_64/lib/crti.o" ]; then \
		echo "Building leblibc runtime objects..."; \
		$(MAKE) -C "$(LIBC_ABS)/leblibc" build-x86_64 || (cat "$(LIBC_ABS)/leblibc/build-x86_64/config.log" >&2; false); \
	fi
	@echo "Installing leblibc headers to sysroot..."
	@$(MAKE) -C "$(LIBC_ABS)/leblibc" install-headers DESTDIR="$(SYSROOT)" ARCH=x86_64 >/dev/null 2>&1 || true
	@cd $(NCURSES_DIR) && \
		export LEB_ROOT=$(LEB_ROOT); \
		export SYSROOT=$$LEB_ROOT/sysroot; \
		export CC=x86_64-elf-gcc; \
		export CPP="$$CC -E"; \
		export AR=x86_64-elf-ar; export RANLIB=x86_64-elf-ranlib; \
		export CFLAGS="--sysroot=$$SYSROOT -Os -fno-stack-protector -fno-pic -fno-builtin"; \
		export CPPFLAGS="-D_XOPEN_SOURCE=600 -I$$SYSROOT/usr/include -I$$LEB_ROOT/libc/leblibc/include -isystem=$$LEB_ROOT/libc/leblibc/build-x86_64/obj/include -isystem=$$LEB_ROOT/libc/leblibc/arch/x86_64 -isystem=$$LEB_ROOT/libc/leblibc/arch/generic"; \
		export LDFLAGS="-nostdlib -static -T $$LEB_ROOT/libc/user.ld"; \
		./configure --host=x86_64-elf --build=$$(uname -m)-pc-linux-gnu --prefix=/usr \
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
			ac_cv_func_wctombs=yes ac_cv_func_wctomb=yes ac_cv_func_wctob=yes ac_cv_func_btowc=yes \
			ac_cv_func_setenv=yes ac_cv_func_putenv=yes ac_cv_func_unsetenv=yes \
			ac_cv_func_getcwd=yes ac_cv_func_getwd=yes \
			cf_cv_posix_c_source=no cf_cv_xopen_source=no \
			ac_cv_search_regcomp=none ac_cv_func_regcomp=yes cf_cv_regex_hdrs=regex.h \
			ac_cv_func_setvbuf_reversed=no \
			cf_cv_mb_len_max=yes ; \
		$(MAKE); \
		$(MAKE) install DESTDIR=$$SYSROOT; \
		ln -sf libncursesw.a $$SYSROOT/usr/lib/libncurses.a

clean:
	@if [ -d "$(NCURSES_DIR)" ] && [ -f "$(NCURSES_DIR)/Makefile" ]; then \
		cd $(NCURSES_DIR) && $(MAKE) clean 2>/dev/null || true; \
	fi
	@rm -rf $(NCURSES_DIR)/config.cache
	@find $(NCURSES_DIR) -type f -name '*.o' -delete 2>/dev/null || true
