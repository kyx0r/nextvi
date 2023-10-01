#!/bin/sh -e
CFLAGS="\
-pedantic -Wall -Wextra \
-Wno-implicit-fallthrough \
-Wno-missing-field-initializers \
-Wno-unused-parameter \
-Wno-unused-result \
-Wfatal-errors -std=c99 \
-D_POSIX_C_SOURCE=200809L -O2 $CFLAGS"

: "${CC:=cc}"
: "${PREFIX:=/usr/local}"
: "${OS:=$(uname)}"
case "$OS" in
	*BSD*)		CFLAGS="$CFLAGS -D_BSD_SOURCE"		;;
	*Darwin*)	CFLAGS="$CFLAGS -D_DARWIN_C_SOURCE"	;;
esac

run() {
	printf '%s\n' "$*"
	eval "$*"
}

install() {
	[ -x vi ] || build
	run mkdir -p "$DESTDIR$PREFIX/bin/"
	run cp -f vi "$DESTDIR$PREFIX/bin/vi"
}

build() {
	run "$CC vi.c -o vi $CFLAGS"
}

pgobuild() {
	run "$CC vi.c -fprofile-generate=. -o vi $CFLAGS"
	echo "qq" | ./vi -v ./vi.c >/dev/null
	case "$CC" in *clang*) run llvm-profdata merge ./*.profraw -o default.profdata ;; esac
	run "$CC vi.c -fprofile-use=. -o vi $CFLAGS"
	rm -f ./*.gcda ./*.profraw ./default.profdata
}

if [ "$#" -gt 0 ]; then
	"$@"
else
	build
fi
