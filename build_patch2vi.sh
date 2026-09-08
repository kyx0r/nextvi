#!/bin/sh

POSIXLY_CORRECT=1
cbuild_OPWD="$PWD"
BASE="${0%/*}" && [ "$BASE" = "$0" ] && BASE="." # -> BASE="$(realpath "$0")" && BASE="${BASE%/*}"
cd "$BASE" || log "$R" "Unable to change directory to ${BASE##*/}. Re-execute using a POSIX shell and check again."
BASE="${PWD%/}"
trap 'cd "$cbuild_OPWD"' EXIT

# Color escape sequences
G="\033[32m" #     Green
R="\033[31m" #     Red
B="\033[34m" #     Blue
NC="\033[m"  #     Unset

log() {
    # shellcheck disable=SC2059 # Using %s with ANSII escape sequences is not possible
    printf "${1}->$NC "
    shift
    printf "%s\n" "$*"
}

require() {
    set -- $1
    command -v "$1" >/dev/null 2>&1 || {
        log "$R" "[$1] is not installed. Please ensure the command is available [$1] and try again."
        exit 1
    }
}

run() {
    log "$B" "$*"
    # shellcheck disable=SC2068 # We want to split elements, but avoid whitespace problems (`$*`), and also avoid `eval $*`
    $@
}

: "${CC:=cc}"
: "${STRIP:=strip}"
: "${PREFIX:=/usr/local}"
: "${OS:=$(uname)}"
: "${CFLAGS:=-O2}"

CFLAGS="\
-pedantic -Wall -Wextra \
-Wno-implicit-fallthrough \
-Wno-missing-field-initializers \
-Wno-unused-parameter \
-Wno-unused-result \
-Wfatal-errors -std=c99 \
$CFLAGS"

case "$OS" in
*_NT*) CFLAGS="$CFLAGS -D_POSIX_C_SOURCE=200809L" ;;
*Darwin*) CFLAGS="$CFLAGS -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE" ;;
*Linux*) CFLAGS="$CFLAGS -D_POSIX_C_SOURCE=200809L" ;;
*) CFLAGS="$CFLAGS -D_DEFAULT_SOURCE" ;;
esac

# patch2vi.c embeds the whole editor via #include "vi.c" and drives it
# through nextvi_main(); rename nextvi's main() for the build and restore it
# after (both directions run through here, POSIX sed has no -i).
rename_main() {
    sed "s/^int $1(int argc, char \*argv\[\])$/int $2(int argc, char *argv[])/" vi.c > vi.c.p2v &&
    mv -f vi.c.p2v vi.c
}

build() {
    require "${CC}"
    require sed
    log "$G" "Entering step: \"Build \"${BASE##*/}\" using \"$CC\"\""
    rename_main main nextvi_main || {
        log "$R" "Failed to rename main() in vi.c"
        exit 1
    }
    trap 'rename_main nextvi_main main; cd "$cbuild_OPWD"' EXIT
    run "$CC patch2vi.c -o patch2vi $CFLAGS" || {
        log "$R" "Failed during step: \"Build \"${BASE##*/}\" using \"$CC\""
        exit 1
    }
    rename_main nextvi_main main
    trap 'cd "$cbuild_OPWD"' EXIT
}

install() {
    run rm -f "$DESTDIR$PREFIX/bin/patch2vi" 2> /dev/null
    command -v "$STRIP" >/dev/null 2>&1 && run "$STRIP" patch2vi
    run mkdir -p "$DESTDIR$PREFIX/bin/" &&
    run cp -f patch2vi "$DESTDIR$PREFIX/bin/patch2vi" &&
    [ -x "$DESTDIR$PREFIX/bin/patch2vi" ] && log "$G" "\"${BASE##*/}\" has been installed to $DESTDIR$PREFIX/bin/patch2vi" || log "$R" "Couldn't finish installation"
}

print_usage() {
    echo "Usage: $0 {install|pgobuild|build|debug|clean}"
    echo "Options may be shortened to a prefix"
    exit "$1"
}

# Argument processing
while [ $# -gt 0 ] || [ "$1" = "" ]; do
    case "$1" in
    i*)
        shift
        [ -x ./patch2vi ] && install && exit 0 || build && install && exit 0
        ;;
    d*)
        shift
        if command -v scan-build >/dev/null 2>&1; then
                CC="scan-build $CC"
        fi
        CFLAGS="$CFLAGS -O0 -g -fsanitize=address -fsanitize=undefined"
        log "$G" "Entering step: \"Append \"\$CFLAGS\" with debugging flags\""
        set -- build "$@"
        ;;
    "" | b | bu*)
        # If the user doesn't use "build" explicitly, do not run the build step again.
        [ -n "$1" ] && explicit="1"
        if [ "$explicit" != "1" ]; then
            if [ -f ./patch2vi ]; then
                log "$R" "Nothing to do; \"${BASE##*/}\" was already compiled"
                print_usage 0
            fi
        fi
        # Start build process
        build && exit 0 || exit 1
        ;;
    p*)
        shift
        # deltas.sh applies itself onto patch2vi.c and test_patch2vi.sh, so the
        # training run rewrites them in place; keep copies and put them back.
        pgorestore() {
            [ -f patch2vi.c.pgo ] && mv -f patch2vi.c.pgo patch2vi.c
            [ -f test_patch2vi.sh.pgo ] && mv -f test_patch2vi.sh.pgo test_patch2vi.sh
            return 0
        }
        pgotrain() {
            cp -f patch2vi.c patch2vi.c.pgo &&
            cp -f test_patch2vi.sh test_patch2vi.sh.pgo || return 1
            trap 'pgorestore; rename_main nextvi_main main; cd "$cbuild_OPWD"' EXIT
            # -e drives the script with the embedded editor, no shell involved
            log "$B" "./patch2vi -e deltas.sh"
            ./patch2vi -e deltas.sh >/dev/null 2>&1 ||
                log "$R" "Training run failed; the profile may be incomplete"
            pgorestore
            trap 'rename_main nextvi_main main; cd "$cbuild_OPWD"' EXIT
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
                [ -z "$PROFDATA" ] && log "$R" "pgobuild with clang requires llvm-profdata" && exit 1
            fi
            rename_main main nextvi_main || {
                log "$R" "Failed to rename main() in vi.c"
                exit 1
            }
            trap 'rename_main nextvi_main main; cd "$cbuild_OPWD"' EXIT
            run "$CC patch2vi.c -fprofile-generate=. -o patch2vi -O2 $CFLAGS" || return 1
            pgotrain || return 1
            [ "$clang" = 1 ] && run "$PROFDATA" merge ./*.profraw -o default.profdata
            run "$CC patch2vi.c -fprofile-use=. -o patch2vi -O2 $CFLAGS" || return 1
            rm -f ./*.gcda ./*.profraw ./default.profdata
            rename_main nextvi_main main
            trap 'cd "$cbuild_OPWD"' EXIT
        }
        require "${CC}"
        require sed
        [ -f ./deltas.sh ] || {
            log "$R" "pgobuild needs deltas.sh for the training run"
            exit 1
        }
        log "$G" "Entering step: \"Build \"${BASE##*/}\" using \"$CC\" and PGO\""
        pgobuild || {
            log "$R" "Failed during step: \"Build \"${BASE##*/}\" using \"$CC\" and PGO\""
            exit 1
        } && exit 0 || exit 1
        ;;
    c*)
        shift
        run rm -f patch2vi patch2vi.c.pgo test_patch2vi.sh.pgo \
            ./*.gcda ./*.profraw default.profdata 2>/dev/null
        exit 0
        ;;
    *)
        print_usage 1
        ;;
    esac
done
