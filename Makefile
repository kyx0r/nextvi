CFLAGS := -pedantic -Wall -Wfatal-errors -std=c99 -D_POSIX_C_SOURCE=200809L $(CFLAGS) 
OS := $(shell sh -c 'uname -s 2>/dev/null || echo not')

ifeq ($(OS), FreeBSD)
	CFLAGS+=-D_BSD_SOURCE
endif
ifeq ($(OS), OpenBSD)
	CFLAGS+=-D_BSD_SOURCE
endif

OBJS = vi.o ex.o lbuf.o sbuf.o ren.o led.o \
	uc.o term.o regex.o conf.o hund.o

all: vi

%.o: %.c
	$(CC) -c $(CFLAGS) $<
vi: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(CFLAGS)
clean:
	rm -f *.o vi
install: vi
	cp -f vi /bin
