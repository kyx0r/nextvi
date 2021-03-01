CFLAGS := -Wall -std=c99 -D_POSIX_C_SOURCE $(CFLAGS) 

OBJS = vi.o ex.o lbuf.o sbuf.o ren.o led.o \
	uc.o term.o regex.o conf.o hund.o

all: vi

conf.o: conf.h

%.o: %.c
	$(CC) -c $(CFLAGS) $<
vi: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(CFLAGS)
clean:
	rm -f *.o vi
install: vi
	cp -f vi /bin
