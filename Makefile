
APP=avcut

CFLAGS+=-Wall
LDLIBS=-lavcodec -lavformat -lavutil

## enable support for libav (EXPERIMENTAL)
#CFLAGS=-DUSING_LIBAV

.PHONY: clean

all: $(APP)

$(APP): avcut.c

clean:
	rm -f *.o $(APP)

debug: CFLAGS+=-g -DDEBUG
debug: all