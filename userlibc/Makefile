CC = i686-elf-gcc
AR = i686-elf-ar
CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -fno-pic -m32 -O2
CPPFLAGS = -Iinclude

SRCDIR = src
OBJDIR = obj

SRCS = $(wildcard $(SRCDIR)/*.c)
OBJS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))

CRT0_OBJ = $(OBJDIR)/crt0.o
LIB_OBJS = $(filter-out $(CRT0_OBJ),$(OBJS))

TARGET = libuser.a

.PHONY: all clean install

all: $(OBJDIR) $(TARGET) $(CRT0_OBJ)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(TARGET): $(LIB_OBJS)
	$(AR) rcs $@ $^

clean:
	rm -rf $(OBJDIR) $(TARGET)

install: all
	mkdir -p ../sysroot/usr/lib
	mkdir -p ../sysroot/usr/include
	cp $(TARGET) ../sysroot/usr/lib/
	cp $(CRT0_OBJ) ../sysroot/usr/lib/crt0.o
	cp -r include/* ../sysroot/usr/include/
