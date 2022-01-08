#!/bin/sh -e
CFLAGS="\
-pedantic -Wall -Wextra \
-Wno-implicit-fallthrough \
-Wno-missing-field-initializers \
-Wno-unused-parameter \
-Wfatal-errors -std=c99 \
-D_POSIX_C_SOURCE=200809L $CFLAGS"

OS="$(uname)"
: ${CC:=$(command -v cc)}
: ${PREFIX:=/usr/local}
case "$OS" in *BSD*) CFLAGS="$CFLAGS -D_BSD_SOURCE" ;; esac

run() {
	printf '%s\n' "$*"
	"$@"
}

install() {
	[ -x vi ] || build
	run mkdir -p "$DESTDIR$PREFIX/bin/"
	run cp -f vi "$DESTDIR$PREFIX/bin/vi"
}

build() {
	run "$CC" "vi.c" $CFLAGS -o vi
}

pgobuild() {
	run "$CC" "vi.c" $CFLAGS -fprofile-generate=. -o vi
	echo "qq" | ./vi -v ./vi.c >/dev/null
	run "$CC" "vi.c" $CFLAGS -fprofile-use=. -o vi
	rm *.gcda
}

if [ "$#" -gt 0 ]; then
	"$@"
else
	build
fi
