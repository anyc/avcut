
-include local.mk

APP=avcut
AVCUT_VERSION=0.5

ifneq ($(CROSS_COMPILE),)
	CC?=$(CROSS_COMPILE)gcc
	LD?=$(CROSS_COMPILE)ld
	PKG_CONFIG?=$(CROSS_COMPILE)pkg-config
else
	PKG_CONFIG?=pkg-config
endif

CFLAGS+=-Wall -DAVCUT_VERSION=\"$(AVCUT_VERSION)\"
CFLAGS+=$(shell for x in libavcodec libavformat libavutil; do $(PKG_CONFIG) --cflags $(PC_FLAGS) "$$x"; done)
LDLIBS+=$(shell for x in libavcodec libavformat libavutil; do $(PKG_CONFIG) --libs $(PC_FLAGS) "$$x"; done)

AVCUT_PROFILE_DIRECTORY?=$(PREFIX)/share/avcut/profiles/

### set profile directory
CFLAGS+=-DAVCUT_PROFILE_DIRECTORY=\"$(AVCUT_PROFILE_DIRECTORY)\"

TAR?=$(shell which tar)
ARCH?=$(shell gcc -dumpmachine | cut -d "-" -f1)
PREFIX?=/usr


.PHONY: clean install

all: $(APP)

$(APP): avcut.c

clean:
	rm -f *.o $(APP)

install: $(APP)
	mkdir -p "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 avcut "$(DESTDIR)$(PREFIX)/bin/"
	mkdir -p "$(DESTDIR)$(PREFIX)/share/doc/$(APP)/"
	install -m 644 README.md "$(DESTDIR)$(PREFIX)/share/doc/$(APP)/"
	
	mkdir -p $(DESTDIR)$(AVCUT_PROFILE_DIRECTORY)
	cp profiles/* $(DESTDIR)$(AVCUT_PROFILE_DIRECTORY)
	chmod -R 644 $(DESTDIR)$(AVCUT_PROFILE_DIRECTORY)/*
	chmod 755 $(DESTDIR)$(AVCUT_PROFILE_DIRECTORY)

package: $(APP)
	$(TAR) -czf "avcut-$(AVCUT_VERSION)-$(ARCH).tar.gz" avcut README.md LICENSE

debug: CFLAGS+=-g -DDEBUG
debug: all

static: CFLAGS+=-static
static: PC_FLAGS=--static
static: all

version:
	@echo $(AVCUT_VERSION)
