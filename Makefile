
APP=avcut

CFLAGS+=-Wall
LDLIBS=-lavcodec -lavformat -lavutil -lavfilter

.PHONY: clean

all: $(APP)

$(APP): avcut.c

clean:
	rm -f *.o $(APP)

debug: CFLAGS+=-g -DDEBUG
debug: all