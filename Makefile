CFLAGS := -Wall $(CFLAGS) 

OBJS = vi.o ex.o lbuf.o mot.o sbuf.o ren.o dir.o syn.o reg.o led.o \
	uc.o term.o rset.o regex.o cmd.o conf.o hund.o

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
