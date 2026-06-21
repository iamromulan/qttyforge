# qttyforge — build
#
# Native build:        make
# Debug build:         make DEBUG=1
# Static binary:       make STATIC=1
# Cross (static musl): make CC=aarch64-linux-musl-gcc   STATIC=1
#                      make CC=arm-linux-musleabihf-gcc STATIC=1
#
# Release static binaries for armv7 + aarch64 are produced in CI (musl);
# this Makefile just honours CC / CFLAGS / STATIC so any toolchain works.

BIN    := qttyforge
SRCDIR := src
OBJDIR := build

CC ?= cc

ifeq ($(DEBUG),1)
CFLAGS ?= -O0 -g
else
CFLAGS ?= -O2
endif

override CPPFLAGS += -D_GNU_SOURCE -I$(SRCDIR)
override CFLAGS   += -std=c11 -Wall -Wextra

ifeq ($(STATIC),1)
override LDFLAGS += -static
endif

# openpty() lives in -lutil on glibc, but in libc on musl and macOS. Add
# -lutil only for non-musl Linux (glibc), so static musl builds (Alpine /
# cross) and macOS don't fail on a missing libutil.
UNAME_S    := $(shell uname -s)
CC_MACHINE := $(shell $(CC) -dumpmachine 2>/dev/null)
ifeq ($(UNAME_S),Linux)
ifeq ($(findstring musl,$(CC_MACHINE)),)
override LDLIBS += -lutil
endif
endif

PREFIX  ?= /usr/local
DESTDIR ?=

SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean install

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -rf $(OBJDIR) $(BIN)

-include $(DEPS)
