CC := i686-elf-gcc
AR := i686-elf-ar

V ?= 0
ifeq ($(V),0)
  Q = @
  MSG_CC = @printf '  CC      %s\n' $<;
  MSG_AR = @printf '  AR      %s\n' $@;
else
  Q =
  MSG_CC =
  MSG_AR =
endif

CFLAGS = -Os -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -m32 \
         -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough \
         -Wno-unused-function -Wno-unused-variable -Wno-missing-field-initializers

INCLUDES = -I../../include

OBJS = crypto/sha256.o crypto/sha512.o crypto/hmac.o

.PHONY: all clean

all: libbearssl.a

libbearssl.a: $(OBJS)
	$(MSG_AR)$(AR) rcs $@ $(OBJS)

crypto/%.o: crypto/%.c
	$(Q)mkdir -p $(dir $@)
	$(MSG_CC)$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) libbearssl.a
