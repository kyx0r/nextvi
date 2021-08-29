#!/bin/sh -e
CFLAGS="-pedantic -Wall -Wfatal-errors -std=c99 -D_POSIX_C_SOURCE=200809L $CFLAGS"
SRCS="vi.c ex.c lbuf.c sbuf.c ren.c led.c uc.c term.c regex.c conf.c hund.c"
OBJS="vi.o ex.o lbuf.o sbuf.o ren.o led.o uc.o term.o regex.o conf.o hund.o"
OS="$(uname)"
: ${CC:=$(command -v cc)}
: ${PREFIX:=/usr/local}
case "$OS" in *BSD*) CFLAGS="$CFLAGS -D_BSD_SOURCE" ;; esac

run() {
	printf '%s\n' "$*"
	"$@"
}

clean() {
	run rm -f $OBJS vi
}

install() {
	[ -x vi ] || build
	run mkdir -p "$DESTDIR$PREFIX/bin/"
	run cp -f vi "$DESTDIR$PREFIX/bin/vi"
}

build() {
	for src in $SRCS; do
		run "$CC" -c $CFLAGS $src
	done
	run "$CC" $CFLAGS $OBJS -o vi
}

"${@:-build}"
