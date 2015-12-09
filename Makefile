
APP=avcut

CFLAGS+=-Wall
LDLIBS=-lavcodec -lavformat -lavutil

## enable support for libav (EXPERIMENTAL)
#CFLAGS=-DUSING_LIBAV

## create a script that calls mpv to show the frames 10s before and after
## a cutting point
#CFLAGS=-DCREATE_CHECK_SCRIPT

.PHONY: clean

all: $(APP)

$(APP): avcut.c

clean:
	rm -f *.o $(APP)

debug: CFLAGS+=-g -DDEBUG
debug: all