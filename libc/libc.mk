# The native C library (docs/libc/). Built with the user flags into a
# static archive plus the program entry object; userland.mk links every
# program as crt0.o objects libc.a.

LIBC_A    := $(OUT)/libc/libc.a
LIBC_CRT0 := $(OUT)/libc/src/arch/$(ARCH)/crt0.o

USER_CFLAGS := \
	--target=$(KERNEL_TARGET) \
	$(COMMON_CFLAGS) \
	-fno-pic -fno-pie -mgeneral-regs-only \
	-Wno-missing-prototypes \
	-I$(ROOT)/libc/include \
	-I$(ROOT)/kernel/include

USER_LDFLAGS := \
	-nostdlib -static --no-dynamic-linker \
	-z max-page-size=0x1000 -z noexecstack -z separate-code \
	--build-id=none --gc-sections

# string.c must not have its loops turned into calls to itself.
LIBC_STRING_CFLAGS := $(USER_CFLAGS) -fno-builtin

LIBC_SRCS := \
	libc/src/errno.c \
	libc/src/ctype.c \
	libc/src/malloc.c \
	libc/src/stdlib.c \
	libc/src/conv.c \
	libc/src/printf.c \
	libc/src/stdio.c \
	libc/src/unistd.c \
	libc/src/dirent.c \
	libc/src/process.c \
	libc/src/socket.c \
	libc/src/cosmo.c

LIBC_OBJS := $(call objs_of,$(LIBC_SRCS)) $(OUT)/libc/src/string.o

$(eval $(call compile_rules,$(call objs_of,$(LIBC_SRCS)),USER_CFLAGS))
$(eval $(call compile_rules,$(OUT)/libc/src/string.o,LIBC_STRING_CFLAGS))
$(eval $(call assemble_rules,$(LIBC_CRT0),USER_CFLAGS))

$(LIBC_A): $(LIBC_OBJS)
	$(call log,AR,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)rm -f $@
	$(Q)$(AR) rcsD $@ $(LIBC_OBJS)

.PHONY: libc
libc: $(LIBC_A) $(LIBC_CRT0)

-include $(LIBC_OBJS:.o=.d) $(LIBC_CRT0:.o=.d)
