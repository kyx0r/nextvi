#!/bin/sh
CFLAGS="-pedantic -Wall -Wfatal-errors -std=c99 -D_POSIX_C_SOURCE=200809L $CFLAGS"
SRCS="vi.c ex.c lbuf.c sbuf.c ren.c led.c uc.c term.c regex.c conf.c hund.c"
OBJS="vi.o ex.o lbuf.o sbuf.o ren.o led.o uc.o term.o regex.o conf.o hund.o"
OS="$(uname)"
case ${CC:-0} in
	0) CC="gcc";;
	*) :
esac
if [ "$OS" = "FreeBSD" ]; then
	CFLAGS="$CFLAGS -D_BSD_SOURCE"
fi
if [ "$OS" = "OpenBSD" ]; then
	CFLAGS="$CFLAGS -D_BSD_SOURCE"
fi

split() {
	set -f
	old_ifs=$IFS
	IFS=$2
	set -- $1
	printf '%s\n' "$@"
	IFS=$old_ifs
	set +f
}

cls() {
	rm *.o vi
	exit 0
}

ins() {
	cp -f vi /bin
	exit 0
}

build() {
	split "$SRCS" " " | while read src; do
		$CC -c $CFLAGS $src &
		printf '%s\n' "$CC -c $CFLAGS $src"
	done
	split "$OBJS" " " | while read obj; do
		while [ ! -f "$obj" ]; do sleep 1; done
	done
	$CC $CFLAGS $OBJS -o vi
	printf '%s\n' "$CC $CFLAGS $OBJS -o vi"
	exit 0
}

"$@"
build
