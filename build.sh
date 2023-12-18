#!/bin/sh -e

if [ -z "$OPTFLAGS" ]; then
	if [ "$1" = "debug" ]; then
		OPTFLAGS="-O0"
	else
		OPTFLAGS="-O2"
	fi
fi

CFLAGS="\
-pedantic -Wall -Wextra \
-Wno-implicit-fallthrough \
-Wno-missing-field-initializers \
-Wno-unused-parameter \
-Wno-unused-result \
-Wfatal-errors -std=c99 \
-D_POSIX_C_SOURCE=200809L $OPTFLAGS $CFLAGS"

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

debug() {
	run "$CC vi.c -o vi -g $CFLAGS"
}

pgobuild() {
	ccversion="$($CC --version)"
	case "$ccversion" in *clang*) clang=1 ;; esac
	if [ "$clang" = 1 ] && [ -z "$PROFDATA" ]; then
		if command -v llvm-profdata >/dev/null 2>&1; then
			PROFDATA=llvm-profdata
		elif xcrun -f llvm-profdata >/dev/null 2>&1; then
			PROFDATA="xcrun llvm-profdata"
		fi
		[ -z "$PROFDATA" ] && echo "pgobuild with clang requires llvm-profdata" && exit 1
	fi
	run "$CC vi.c -fprofile-generate=. -o vi $CFLAGS"
	echo "qq" | ./vi -v ./vi.c >/dev/null
	[ "$clang" = 1 ] && run "$PROFDATA" merge ./*.profraw -o default.profdata
	run "$CC vi.c -fprofile-use=. -o vi $CFLAGS"
	rm -f ./*.gcda ./*.profraw ./default.profdata
}

if [ "$#" -gt 0 ]; then
	"$@"
else
	build
fi
