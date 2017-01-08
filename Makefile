
APP=avcut
AVCUT_VERSION=0.2

ifneq ($(CROSS_COMPILE),)
	CC=$(CROSS_COMPILE)gcc
	LD=$(CROSS_CoMPILE)ld
	PKG_CONFIG=$(CROSS_COMPILE)pkg-config
else
	PKG_CONFIG?=pkg-config
endif

CFLAGS+=-Wall -DAVCUT_VERSION=\"$(AVCUT_VERSION)\"
LDLIBS=$(shell for x in libavcodec libavformat libavutil; do $(PKG_CONFIG) --libs $(PC_FLAGS) "$$x"; done)

## enable support for libav (EXPERIMENTAL)
#CFLAGS=-DUSING_LIBAV

TAR?=$(shell which tar)
ARCH?=$(shell gcc -dumpmachine | cut -d "-" -f1)
PREFIX?=/usr


.PHONY: clean install

all: $(APP)

$(APP): avcut.c

clean:
	rm -f *.o $(APP)

install: $(APP)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 755 avcut $(DESTDIR)$(PREFIX)/bin/
	install -m 644 README.md $(DESTDIR)$(PREFIX)/share/doc/$(APP)/

package: $(APP)
	$(TAR) -czf avcut-$(AVCUT_VERSION)-$(ARCH).tar.gz avcut README.md LICENSE

debug: CFLAGS+=-g -DDEBUG
debug: all

static: CFLAGS+=-static
static: PC_FLAGS=--static
static: all

