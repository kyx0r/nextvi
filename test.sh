#!/bin/sh
# Nextvi test suite - POSIX shell
# Tests ex commands and vi normal mode

PASS=0
FAIL=0
N=0
VI=./vi

check() {
	name="$1" expected="$2" actual="$3"
	N=$((N + 1))
	printf 'Test %d: "%s"\n' "$N" "$name"
	if [ "$expected" = "$actual" ]; then
		PASS=$((PASS + 1))
	else
		FAIL=$((FAIL + 1))
		printf 'FAIL\n  expected: |%s|\n  actual:   |%s|\n' \
			"$expected" "$actual"
	fi
}

check_exit() {
	name="$1" expected="$2" actual="$3"
	N=$((N + 1))
	printf 'Test %d: "%s"\n' "$N" "$name"
	if [ "$expected" = "$actual" ]; then
		PASS=$((PASS + 1))
	else
		FAIL=$((FAIL + 1))
		printf 'FAIL\n  expected exit: %s\n  actual exit:   %s\n' \
			"$expected" "$actual"
	fi
}

TMPFILE=$(mktemp /tmp/nextvi_test_XXXXXX)
OUTFILE=/tmp/nextvi_out_$$
trap 'rm -f "$TMPFILE" "$OUTFILE"' EXIT

# run_ex: pass EXINIT, capture stdout; buffer unchanged on disk
run_ex() {
	EXINIT="$1" "$VI" -sm "$TMPFILE" </dev/null 2>/dev/null
}

# run_vi: pass vi key sequence; write result to OUTFILE, read it back
run_vi() {
	rm -f "$OUTFILE"
	EXINIT=":& $1:w! $OUTFILE:q" "$VI" -e "$TMPFILE" </dev/null >/dev/null 2>&1
	cat "$OUTFILE" 2>/dev/null
}

# run_mac: pass EXINIT for ex+macro tests (-e mode); writes to OUTFILE
run_mac() {
	rm -f "$OUTFILE"
	EXINIT="$1" "$VI" -e "$TMPFILE" </dev/null >/dev/null 2>&1
	cat "$OUTFILE" 2>/dev/null
}

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':1p:q')
check 'print line 1' 'hello world' "$out"

printf 'line1\nline2\nline3\n' > "$TMPFILE"
out=$(run_ex ':1,3p:q')
check 'print range 1-3' "$(printf 'line1\nline2\nline3')" "$out"

printf 'aaa\nbbb\nccc\n' > "$TMPFILE"
out=$(run_ex ':%p:q')
check 'print all with %' "$(printf 'aaa\nbbb\nccc')" "$out"

printf 'first\nsecond\nthird\n' > "$TMPFILE"
out=$(run_ex ':$p:q')
check 'print last line ($)' 'third' "$out"

printf 'irrelevant\n' > "$TMPFILE"
out=$(run_ex ':p hello test:q')
check 'print literal arg' 'hello test' "$out"

printf 'a\nb\nc\n' > "$TMPFILE"
out=$(run_ex ':3=1:q')
check 'print line number (=)' '3' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':%s/world/nextvi/:%p:q!')
check 'substitute simple' 'hello nextvi' "$out"

printf 'int a; int b;\n' > "$TMPFILE"
out=$(run_ex ':%s/int/uint/g:%p:q!')
check 'substitute global' 'uint a; uint b;' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':%s/(hello) (world)/\2 \1/:%p:q!')
check 'substitute backreference' 'world hello' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':s/void/x/:??!p not found:q')
check 'conditional else on sub no-match' 'not found' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':s/world/x/:??p found:q!')
check 'conditional then on sub match' 'found' "$out"

printf 'a\nb\nc\nd\ne\n' > "$TMPFILE"
out=$(run_ex ':2,4d:%p:q!')
check 'delete range 2-4' "$(printf 'a\ne')" "$out"

printf 'a\nb\nc\nd\n' > "$TMPFILE"
out=$(run_ex ':3,$d:%p:q!')
check 'delete to end' "$(printf 'a\nb')" "$out"

printf 'line1\n\nline2\n\nline3\n' > "$TMPFILE"
out=$(run_ex ':g/^$/d:%p:q!')
check 'delete empty lines (global)' "$(printf 'line1\nline2\nline3')" "$out"

printf 'a\nb\nc\n' > "$TMPFILE"
out=$(run_ex ':%-1j:%p:q!')
check 'join all no padding' 'abc' "$out"

printf 'a\nb\nc\n' > "$TMPFILE"
out=$(run_ex ':%-1jj:%p:q!')
check 'join all with space padding' 'a b c' "$out"

printf 'hello\nworld\n' > "$TMPFILE"
out=$(run_ex ':1ya 97:$pu 97:%p:q!')
check 'yank line and paste at end' "$(printf 'hello\nworld\nhello')" "$out"

printf 'line1\nline2\nline3\n' > "$TMPFILE"
out=$(run_ex ':1ya 97:2ya+ 97:1pu 97:%p:q!')
check 'append to register then paste' \
	"$(printf 'line1\nline1\nline2\nline2\nline3')" "$out"

printf 'line1\n' > "$TMPFILE"
out=$(run_ex ':97reg hello:$pu 97:%p:q!')
check 'put string into register via :reg' "$(printf 'line1\nhello')" "$out"

printf 'int a;\nvoid b;\nint c;\n' > "$TMPFILE"
out=$(run_ex ':g/int/p:q')
check 'global print matching lines' "$(printf 'int a;\nint c;')" "$out"

printf 'int a;\nvoid b;\nint c;\n' > "$TMPFILE"
out=$(run_ex ':g!/int/p:q')
check 'inverted global print' 'void b;' "$out"

printf 'int a;\nvoid b;\nint c;\n' > "$TMPFILE"
out=$(run_ex ':g/int/s/int/uint/\:p:q!')
check 'global with chained sub+print' "$(printf 'uint a;\nuint c;')" "$out"

printf 'int a;\nvoid b;\nint c;\n' > "$TMPFILE"
out=$(run_ex ':g/int/g/a/p:q')
check 'nested global' 'int a;' "$out"

printf 'test int here\n' > "$TMPFILE"
out=$(run_ex ':f>int:??p found:q')
check 'conditional then on search found' 'found' "$out"

printf 'test int here\n' > "$TMPFILE"
out=$(run_ex ':f>void:??!p not found:q')
check 'conditional else on search not found' 'not found' "$out"

printf 'a\nb\nc\nd\ne\n' > "$TMPFILE"
out=$(run_ex ':4? 1d:%p:q!')
check 'while loop counted delete' 'e' "$out"

printf 'old\nold\n' > "$TMPFILE"
out=$(run_ex ':10? s/old/new/:%p:q!')
check 'while loop breaks on xuerr (sub no-match)' "$(printf 'new\nold')" "$out"

printf 'b\na\nc\n' > "$TMPFILE"
out=$(run_ex ':%!sort:%p:q!')
check 'sort buffer via !' "$(printf 'a\nb\nc')" "$out"

printf '' > "$TMPFILE"
out=$(run_ex ':led 0:r \!printf hello:led:%p:q!')
check 'read from pipe into empty buffer' 'hello' "$out"

# ic defaults to 1 (case-insensitive); :ic toggles it to 0 (case-sensitive)
printf 'Hello\nhello\n' > "$TMPFILE"
out=$(run_ex ':g/hello/p:q')
check 'default ic=1: case-insensitive match' "$(printf 'Hello\nhello')" "$out"

printf 'Hello\nhello\n' > "$TMPFILE"
out=$(run_ex ':ic:g/hello/p:ic:q')
check ':ic toggles to case-sensitive match' 'hello' "$out"

printf 'test\n' > "$TMPFILE"
run_ex ':q' >/dev/null 2>&1; rc=$?
check_exit 'exit code :q -> 0' '0' "$rc"

printf 'test\n' > "$TMPFILE"
run_ex ':q 3' >/dev/null 2>&1; rc=$?
check_exit 'exit code :q 3 -> 3' '3' "$rc"

printf 'test\n' > "$TMPFILE"
out=$(run_ex ':f>nosuch:p ok:q')
check 'xuerr is silent and chain continues' 'ok' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_vi 'dw')
check 'vi dw: delete word' 'world' "$out"

printf 'line1\nline2\nline3\n' > "$TMPFILE"
out=$(run_vi 'dd')
check 'vi dd: delete line' "$(printf 'line2\nline3')" "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_vi 'x')
check 'vi x: delete char' 'ello' "$out"

printf 'line1\nline2\n' > "$TMPFILE"
out=$(run_vi 'J')
check 'vi J: join lines with space' 'line1 line2' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_vi 'gUw')
check 'vi gUw: uppercase word' 'HELLO world' "$out"

printf 'HELLO world\n' > "$TMPFILE"
out=$(run_vi 'guw')
check 'vi guw: lowercase word' 'hello world' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_vi '~')
check 'vi ~: toggle case of char' 'Hello' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_vi 'rH')
check 'vi rH: replace char' 'Hello' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_vi 'yyp')
check 'vi yyp: yank and paste line' "$(printf 'hello\nhello')" "$out"

printf 'world\n' > "$TMPFILE"
out=$(run_vi "$(printf 'ihello \033')")
check 'vi i...<ESC>: insert at cursor' 'hello world' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_vi "$(printf 'A world\033')")
check 'vi A...<ESC>: append at end of line' 'hello world' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':%s/hello/world/:ud:%p:q!')
check 'ex :ud undoes substitute' 'hello' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':%s/hello/world/:ud:rd:%p:q!')
check 'ex :rd redoes after undo' 'world' "$out"

printf 'a\nb\nc\nd\ne\n' > "$TMPFILE"
out=$(run_ex ":2m 97:4m 98:'97,'98p:q")
check 'marks: print range via marks' "$(printf 'b\nc\nd')" "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':;6p:q')
check 'horizontal range ;6 prints from offset 6' 'world' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':;0;5p:q')
check 'horizontal range ;0;5 prints chars 0-4' 'hello' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':f>hello:%s//world/:%p:q!')
check 'empty pattern reuses last keyword' 'world world' "$out"

printf 'cat and dog\n' > "$TMPFILE"
out=$(run_ex ':%s/cat|dog/pet/g:%p:q!')
check 'regex alternation in substitute' 'pet and pet' "$out"

printf 'aaa\nbb\nccc\n' > "$TMPFILE"
out=$(run_ex ':%g/b{2}/p:q')
check 'regex quantifier {2} matches' 'bb' "$out"

printf 'foobar\n' > "$TMPFILE"
out=$(run_ex ':%s/foo(?=bar)/FOO/:%p:q!')
check 'regex lookahead in substitute' 'FOObar' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':%s/(hello)/[\0]/:%p:q!')
check 'substitute backreference \\0 full match' '[hello] world' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':%s/(he)(llo)/[\1]/:%p:q!')
check 'substitute backreference \\1 group 1' '[he] world' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':%s/hello //:%p:q!')
check 'substitute with empty replacement deletes match' 'world' "$out"

printf 'int a;\nvoid b;\nint c;\n' > "$TMPFILE"
out=$(run_ex ':1,2f>void:.p:q')
check 'ranged :f> search then print current line' 'void b;' "$out"

printf 'a\nb\nc\nd\ne\n' > "$TMPFILE"
out=$(run_ex ':2,3g/./p:q')
check 'global command with explicit range' "$(printf 'b\nc')" "$out"

printf 'test\n' > "$TMPFILE"
run_ex ':s/test/done/:bs:q' >/dev/null 2>&1; rc=$?
check_exit ':bs marks buffer saved so :q exits 0' '0' "$rc"

printf 'first\nsecond\nthird\n' > "$TMPFILE"
out=$(run_ex ':$-1p:q')
check 'range arithmetic $-1 prints second-to-last' 'second' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':%!tr a-z A-Z:%p:q!')
check 'pipe through tr a-z A-Z uppercases' 'HELLO WORLD' "$out"

printf 'test\n' > "$TMPFILE"
out=$(run_ex ':97reg hello:p %@97:q')
check 'register expansion %@97 in :p' 'hello' "$out"

# % expands to the current buffer path; %@<#> to a register; %<#> to a buffer;
# %# to the previous buffer. A backslash escapes the following %, and separates a
# path expansion from trailing literal digits (so they are not read as a buffer #).

printf 'test\n' > "$TMPFILE"

# %\<digits>: path expansion, then the \ separates literal trailing digits
out=$(run_ex ':p %\123:q')
check 'C12b %\123 — path then literal digits' "${TMPFILE}123" "$out"

# %\#: # is not a digit, so the \ stays literal and # is not consumed as %#
out=$(run_ex ':p %\#:q')
check 'C12b %\# — path then literal \#' "${TMPFILE}\\#" "$out"

# \%: escaped %, emitted literally with no expansion
out=$(run_ex ':p \%123:q')
check 'C12b \%123 — escaped %, fully literal' '%123' "$out"

# %0: explicit buffer 0 (the current file); \ separates trailing digits
out=$(run_ex ':p %0\132:q')
check 'C12b %0\132 — buffer 0 path then literal digits' "${TMPFILE}132" "$out"

# %@ with a non-digit: not a register ref; emits path plus a literal @
out=$(run_ex ':p %@asd:q')
check 'C12b %@asd — non-digit after %@ emits path + literal @' "${TMPFILE}@asd" "$out"

# %\": \ before " stays literal (only %, :, ! are escapable delimiters here)
out=$(run_ex ':p %\"324:q')
check 'C12b %\"324 — backslash before quote stays literal' "${TMPFILE}\\\"324" "$out"

# %@<#> register ref; \ separates the register number from trailing literal digits
out=$(run_ex ':100reg REGVAL:p %@100\75:q')
check 'C12b %@100\75 — register 100 then literal digits' 'REGVAL75' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':err 3:s/void/x/:p after:q')
check 'err=3 breaks chain on xuerr' '' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':err 4:s/void/x/:p after:q')
check 'err=4 silences error and chain continues' 'after' "$out"

printf 'test\n' > "$TMPFILE"
run_ex ':wq! /dev/null' >/dev/null 2>&1; rc=$?
check_exit ':wq! exits 0' '0' "$rc"

printf 'abc\nxyz\nabc\n' > "$TMPFILE"
out=$(run_ex ':3f<abc:p:q')
check ':f< searches backward' 'abc' "$out"

printf 'int a;\nvoid b;\nint c;\n' > "$TMPFILE"
out=$(run_ex ':g!/int/s/void/VOID/\:p:q!')
check 'g! inverted global chained sub+print' 'VOID b;' "$out"

printf 'a\nb\nc\nd\ne\n' > "$TMPFILE"
out=$(run_ex ':,2+3=1:q')
check 'range arithmetic ,2+3 prints 5' '5' "$out"

printf 'line1\nline2\nline3\n' > "$TMPFILE"
out=$(run_ex ':1ya 97:3ya 98:1pu 98:1pu 97:%p:q!')
check 'yank two registers and paste both' \
	"$(printf 'line1\nline1\nline3\nline2\nline3')" "$out"

printf 'old new\n' > "$TMPFILE"
out=$(run_vi "$(printf 'cwfresh\033')")
check 'vi cw: change word' 'freshnew' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_vi "$(printf 'Cworld\033')")
check 'vi C: change to end of line' 'world' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_vi "$(printf '5lD')")
check 'vi D: delete to end of line' 'hello' "$out"

printf 'line1\n' > "$TMPFILE"
out=$(run_vi "$(printf 'onewline\033')")
check 'vi o: open line below' "$(printf 'line1\nnewline')" "$out"

printf 'line2\n' > "$TMPFILE"
out=$(run_vi "$(printf 'Onewline\033')")
check 'vi O: open line above' "$(printf 'newline\nline2')" "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_vi "$(printf 'sH\033')")
check 'vi s: substitute char' 'Hello' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_vi "$(printf 'Snew\033')")
check 'vi S: substitute line' 'new' "$out"

printf 'line1\nline2\n' > "$TMPFILE"
out=$(run_vi 'yyjP')
check 'vi P: paste above current line' "$(printf 'line1\nline1\nline2')" "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_vi 'xu')
check 'vi u: undo restores deleted char' 'hello' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_vi '>>')
check 'vi >>: indent adds tab' "$(printf '\thello')" "$out"

printf 'foo(bar)qux\n' > "$TMPFILE"
out=$(run_vi 'di(')
check 'vi di(: delete inside parens' 'foo()qux' "$out"

printf 'foo(bar)qux\n' > "$TMPFILE"
out=$(run_vi "$(printf 'ci(new\033')")
check 'vi ci(: change inside parens' 'foo(new)qux' "$out"

printf 'one two three\n' > "$TMPFILE"
out=$(run_vi '2dw')
check 'vi 2dw: delete 2 words' 'three' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_vi 'fwx')
check 'vi fw+x: find char then delete it' 'hello orld' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_vi "$(printf '5lK')")
check 'vi K: split line at cursor' "$(printf 'hello \nworld')" "$out"

# E1: :& takes raw vi input; to use a register, expand it via %@97
printf 'hello world\n' > "$TMPFILE"
check 'E1 :& %@97 — register expansion used as raw vi input' 'world' \
	"$(run_mac ":97reg dw:& %@97:w! $OUTFILE:q!")"

# E2: \:cmd inside & macro; a newline (0x0A) is required to submit the ex cmd
printf 'hello\n' > "$TMPFILE"
check 'E2 :& \:s// — ex cmd from macro (newline submits)' 'world' \
	"$(run_mac "$(printf ':& \\:s/hello/world/\n:w! %s:q' "$OUTFILE")")"

# E3: vi normal &a — executes register 'a' as a non-blocking macro
printf 'hello world\n' > "$TMPFILE"
check 'E3 vi &a — executes register as non-blocking macro' 'world' \
	"$(run_mac ":97reg dw:& &a:w! $OUTFILE:q!")"

# E4: vi normal && — repeats the last & macro
printf 'hello world foo\n' > "$TMPFILE"
check 'E4 vi && — repeats last & macro' 'foo' \
	"$(run_mac ":97reg dw:& &a&&:w! $OUTFILE:q!")"

# ya! 97 frees register 97; pu 97 on a freed register raises "uninitialized
# register" — with err 4 (silence+ignore) the paste is skipped silently.
printf 'line1\nline2\n' > "$TMPFILE"
out=$(run_ex ':1ya 97:ya! 97:err 4:$pu 97:%p:q!')
check 'G1 ya! frees reg; pu 97 silently skipped (err 4)' \
	"$(printf 'line1\nline2')" "$out"

# {#id}?? captures the current error status into id; [#id]?? fires its branch on it
# (??! fires on the inverse). An intervening command that changes the error status
# does NOT override the captured value.
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':f>void:5??:s/hello/world/:5??!p notfound:q!')
check 'H1 ?? id captures fail; intervening success does not override' 'notfound' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':f>hello:5??:f>void:5??p found:q')
check 'H2 ?? id captures success; intervening fail does not override' 'found' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':f>void:??!p not found:q')
check 'I1 ??! — fires branch on failure' 'not found' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':f>hello:??p found:q')
check 'I2 ?? — fires branch on success' 'found' "$out"

# ?! runs the body while the condition fails; 1d modifies buffer state and
# persists across iterations until 'yes' surfaces to line 1.
printf 'no\nno\nyes\nno\n' > "$TMPFILE"
out=$(run_ex ':10?! f>yes\:1??\!\:1??1d\:1???\:??\!:.p:q!')
check 'I3 ?! while: 1d deletes first line each pass until f>yes succeeds' 'yes' "$out"

# seq 0 groups all subsequent changes into a single undo step
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':seq 0:s/hello/step1/:s/step1/step2/:s/step2/final/:seq:ud:%p:q!')
check 'J1 seq 0 — batch changes undo as one step' 'hello' "$out"

# seq -1 disables undo tracking entirely; :ud has no effect
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':seq -1:s/hello/world/:ud:%p:seq:q!')
check 'J2 seq -1 — undo tracking disabled; u has no effect' 'world' "$out"

# pr N redirects :p output to register N; led 0 suppresses double-printing
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':led 0:pr 97:p hello captured:pr 0:led:pu 97:%p:q!')
check 'K1 pr+led 0 — :p output captured into register, then pasted' \
	"$(printf 'hello\nhello captured')" "$out"

printf 'a\nb\nc\nd\n' > "$TMPFILE"
out=$(run_ex ":2,3s/./X/:'91p:q!")
check "L1 '[ marks first changed line" 'X' "$out"

printf 'a\nb\nc\nd\n' > "$TMPFILE"
out=$(run_ex ":2,3s/./X/:'93p:q!")
check "L2 '] marks last changed line" 'X' "$out"

# '* = cursor position saved BEFORE the previous ex command ran
printf 'a\nb\nc\nd\n' > "$TMPFILE"
out=$(run_ex ":3p:'42p:q")
check "L3 '* = cursor saved before previous ex command" \
	"$(printf 'c\na')" "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':f>hello:p %@47:q')
check 'M1 %@47 expands to the previous regex keyword' 'hello' "$out"

# $*5/10 — navigate to 50% of the file (integer arithmetic on last line)
printf 'a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n' > "$TMPFILE"
out=$(run_ex ':$*5/10p:q')
check 'N1 $*5/10 — navigate to 50% of file' 'e' "$out"

# ;5;#+10 — # rebases to the previous semicolon value (5), so +10 = 15;
# = 3 prints the second char offset (the final computed value).
printf 'hello world extra\n' > "$TMPFILE"
out=$(run_ex ':;5;#+10= 3:q')
check 'N2 ;5;#+10= 3 — second char offset via # rebase is 15' '15' "$out"

# f( lands on (; \% passes a literal % (not buffer path) to the & macro
printf 'foo(bar)qux\n' > "$TMPFILE"
out=$(run_vi 'f(\%x')
check 'O1 vi f(\%)x — find (, jump to matching ), delete )' 'foo(barqux' "$out"

rm -f "$OUTFILE"
printf 'test\n' > "$TMPFILE"
run_ex ":97reg hello world:pu 97 \!tr a-z A-Z > $OUTFILE:q" >/dev/null 2>/dev/null
check 'P1 :pu 97 \!cmd — pipe register content to external command' \
	'HELLO WORLD' "$(cat $OUTFILE 2>/dev/null)"

rm -f "$OUTFILE"
printf 'hello world\n' > "$TMPFILE"
run_ex ":1,1w \!tr a-z A-Z > $OUTFILE:q" >/dev/null 2>/dev/null
check 'P2 :w \!cmd — write buffer range to external command' \
	'HELLO WORLD' "$(tr -d '\n' < $OUTFILE 2>/dev/null)"

# 1q inside a nested ??! branch propagates xquit through 2 ex_exec levels but
# must restore it at the outermost so vi keeps running. Without the base case
# in ex_exec, xquit=6 escapes to vi and produces exit code 5.
printf 'hello\n' > "$TMPFILE"
EXINIT=":f>void:??!1q 5:q" "$VI" -sm "$TMPFILE" </dev/null >/dev/null 2>&1; rc=$?
check_exit 'Q1 1q in nested ??! scope does not propagate quit to vi' '0' "$rc"

# R1: :re word sets the keyword; %@47 reflects it
printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':re world:p %@47:q')
check 'R1 :re word — sets keyword; %@47 returns it' 'world' "$out"

# R2: :re word sets the keyword; :g// (empty pattern) reuses it
printf 'hello\nworld\nhello world\n' > "$TMPFILE"
out=$(run_ex ':re hello:g//p:q')
check 'R2 :re word; :g// uses set keyword to match lines' \
	"$(printf 'hello\nhello world')" "$out"

# R3: :re sets keyword without moving the cursor (unlike :f>)
printf 'foo\nbar\nbaz\n' > "$TMPFILE"
out=$(run_ex ':3:re bar:.p:q')
check 'R3 :re does not navigate; cursor stays on current line' 'baz' "$out"

# R4: range form :1re escapes regex-special chars; verify via %@47
# Buffer line 1 is "a.b"; ex_regesc turns "." into "\.".
# (The trailing \n from lbuf_region is included so %@47 output is "a\.b"
# after command-substitution strips the trailing newline.)
printf 'a.b\naXb\n' > "$TMPFILE"
out=$(run_ex ':1re:p %@47:q')
check 'R4 range :re — escapes regex chars; %@47 reflects escaped pattern' 'a\.b' "$out"

printf 'void a;\nint b;\nvoid c;\n' > "$TMPFILE"
out=$(run_ex ':>int>p:q')
check 'S1 >int>p — forward inline-search range address prints found line' 'int b;' "$out"

printf 'void a;\nint b;\nvoid c;\n' > "$TMPFILE"
out=$(run_ex ':3:<int<p:q')
check 'S2 3:<int<p — backward inline-search range address' 'int b;' "$out"

printf 'void a;\nvoid b;\nint c;\n' > "$TMPFILE"
out=$(run_ex ':.,>int>p:q')
check 'S3 .,>int>p — range from current line to forward match' \
	"$(printf 'void a;\nvoid b;\nint c;')" "$out"

printf 'a\nb\nc\nd\ne\nf\ng\n' > "$TMPFILE"
out=$(run_ex ':4:.-2,.+2p:q')
check 'S4 .-2,.+2p — relative arithmetic prints 5 lines around line 4' \
	"$(printf 'b\nc\nd\ne\nf')" "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':;5= 2:q')
check 'T1 ;5= 2 — print character offset 5' '5' "$out"

printf 'a\nb\nc\nd\n' > "$TMPFILE"
out=$(run_ex ":3m 97:'97= 0:q")
check "T2 '97= 0 — print stored row index of mark a" '2' "$out"

printf 'hello world extra padding here\n' > "$TMPFILE"
out=$(run_ex ':;5;+10= 3:q')
check 'T3 ;5;+10= 3 — second offset from initial (0+10=10), not from 5' '10' "$out"

# U1: g/^$/d — remove all blank lines
printf 'a\n\nb\n\nc\n' > "$TMPFILE"
out=$(run_ex ':g/^$/d:%p:q!')
check 'U1 g/^$/d — removes all blank lines' "$(printf 'a\nb\nc')" "$out"

# U2: g/int/g/;$/& — nested global appends text; & requires -e mode
printf 'int a;\nvoid b;\nint c;\n' > "$TMPFILE"
out=$(run_mac ':g/int/g/;$/& A has a semicolon:w! '"$OUTFILE"':q')
check 'U2 g/int/g/;$/& A has a semicolon — nested global appends text' \
	"$(printf 'int a; has a semicolon\nvoid b;\nint c; has a semicolon')" "$out"

# U3: err 1 — print errors but continue chain
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:s/void/x/:p after:q')
check 'U3 err=1 — prints error; chain continues' 'after' "$out"

# U4: 2??.= — unset id tag; branch does not execute
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':2??.=:p reached:q')
check 'U4 2??.= — unset id; branch does not run; chain continues' 'reached' "$out"

# U5: grp 1:%f+int(.):grp — save char position after "int("
printf 'void int(x)\n' > "$TMPFILE"
out=$(run_ex ':grp 1:%f+int(.):grp:;= 2:q')
check 'U5 grp 1:%f+int(.):grp — char offset after "int(" is 8' '8' "$out"

# U6: ;5;#+10= 3 — #+10 is relative to previous offset 5 (not initial)
printf 'hello world extra padding here\n' > "$TMPFILE"
out=$(run_ex ':;5;#+10= 3:q')
check 'U6 ;5;#+10= 3 — #+10 relative to 5; offset = 15' '15' "$out"

# U7: 2,4f>int — ranged :f> restricts search to lines 2-4
printf 'a\nb\nint c;\nd\nint e;\nf\n' > "$TMPFILE"
out=$(run_ex ':2,4f>int:.p:q')
check 'U7 2,4f>int — ranged :f> restricts search to given line range' 'int c;' "$out"

# U8: :%f>marker:??!p no marker\:1q:%s/old/new/g:w
# (U8a removed — Q1 covers the abort case)
# When marker found: ??! else branch skipped; :s and :w execute
printf 'marker here\nold text\n' > "$TMPFILE"
EXINIT=":%f>marker:??!p no marker\:1q:%s/old/new/g:w! $OUTFILE" \
	"$VI" -sm "$TMPFILE" </dev/null >/dev/null 2>&1
check 'U8b marker found — else skipped; :s and :w execute' \
	"$(printf 'marker here\nnew text')" "$(cat $OUTFILE 2>/dev/null)"

# U9: :%!sort — pipe buffer through external command
printf 'c\na\nb\n' > "$TMPFILE"
out=$(run_ex ':%!sort:%p:q!')
check 'U9 :%!sort — pipe buffer through sort; output replaces buffer' \
	"$(printf 'a\nb\nc')" "$out"

# U10: g/int/ya+ 97 — global appends matching lines to register 97
printf 'int a;\nvoid b;\nint c;\n' > "$TMPFILE"
out=$(run_ex ':led 0:g/int/ya+ 97:led:1pu 97:%p:q!')
check 'U10 g/int/ya+ 97 — global appends matching lines to register 97' \
	"$(printf 'int a;\nint a;\nint c;\nvoid b;\nint c;')" "$out"

# U11: substitution backreference \0 captures first matched group
printf 'this has int or void\n' > "$TMPFILE"
out=$(run_ex ':%s/(int)|(void)/pre\0after:%p:q!')
check 'U11 :%s/(int)|(void)/pre\0after — backref \0 replaces first match' \
	'this has preintafter or void' "$out"

# U12: 3,5r \!printf — range selects lines 3-5 from pipe; inserts before current line
printf 'start\n' > "$TMPFILE"
out=$(EXINIT=':led 0:3,5r \!printf '"'"'a\nb\nc\nd\ne\nf\n'"'"':led:%p:q!' \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'U12 3,5r \!printf — read only lines 3-5 from pipe output' \
	"$(printf 'c\nd\ne\nstart')" "$out"

# U13: ;$+1!echo world — insert cmd output after end-of-line
printf 'hello\n' > "$TMPFILE"
out=$(EXINIT=':;$+1!echo world:%p:q!' \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'U13 ;$+1!echo world — cmd output inserted at end; original preserved' \
	"$(printf 'hello\nworld')" "$out"

# V1: AND — both succeed → then
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:1??:f>hello:2??:1,2??p then:q')
check 'V1 1,2?? AND — both succeed → then' 'then' "$out"

# V2: AND — first fails → else
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>nomatch:1??:f>hello:2??:1,2??!p else:q')
check 'V2 1,2?? AND — first fails → else' 'else' "$out"

# V3: AND — second fails → else
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:1??:f>nomatch:2??:1,2??!p else:q')
check 'V3 1,2?? AND — second fails → else' 'else' "$out"

# V4: OR — first succeeds → then
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:1??:f>nomatch:2??:1;2??p then:q')
check 'V4 1;2?? OR — first succeeds → then' 'then' "$out"

# V5: OR — first fails, second succeeds → then
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>nomatch:1??:f>hello:2??:1;2??p then:q')
check 'V5 1;2?? OR — second succeeds → then' 'then' "$out"

# V6: OR — both fail → else
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>nomatch:1??:f>nomatch:2??:1;2??!p else:q')
check 'V6 1;2?? OR — both fail → else' 'else' "$out"

# V7: mixed (1,2;3) — AND group fails, id3 succeeds → then
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>nomatch:1??:f>hello:2??:f>hello:3??:1,2;3??p then:q')
check 'V7 1,2;3?? — AND group fails, id3 succeeds → then' 'then' "$out"

# V8: mixed (1,2;3) — AND group succeeds, id3 fails → then
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:1??:f>hello:2??:f>nomatch:3??:1,2;3??p then:q')
check 'V8 1,2;3?? — AND group succeeds, id3 fails → then' 'then' "$out"

# V9: mixed (1,2;3) — all fail → else
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>nomatch:1??:f>nomatch:2??:f>nomatch:3??:1,2;3??!p else:q')
check 'V9 1,2;3?? — all fail → else' 'else' "$out"

# V10: unset id in AND → nop, chain continues
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:1??:1,99??p then:p reached:q')
check 'V10 1,99?? — id 99 unset → nop, chain continues' 'reached' "$out"

# V11: unset id in OR → nop even if other id is set
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:1??:1;99??p then:p reached:q')
check 'V11 1;99?? — id 99 unset → nop, chain continues' 'reached' "$out"

# V12: single unset id → nop
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:99??p then:p reached:q')
check 'V16 99?? — single unset id → nop, chain continues' 'reached' "$out"

# V13: unset id in AND within complex prefix (1,99;2) → nop
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:1??:f>hello:2??:1,99;2??p then:p reached:q')
check 'V17 1,99;2?? — unset id in AND position → nop' 'reached' "$out"

# V14: unset id in first OR position (99;1) → nop
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:1??:99;1??p then:p reached:q')
check 'V18 99;1?? — unset id in first OR position → nop' 'reached' "$out"

# V15: unset id in middle OR position (1;99;2) → nop
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:1??:f>hello:2??:1;99;2??p then:p reached:q')
check 'V19 1;99;2?? — unset id in middle OR position → nop' 'reached' "$out"

# V16-V19: interleaved 1,2;3,4,5;6 = (1 AND 2) OR (3 AND 4 AND 5) OR 6
# V16: first AND group succeeds → then
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:1??:f>hello:2??:f>nomatch:3??:f>nomatch:4??:f>nomatch:5??:f>nomatch:6??:1,2;3,4,5;6??p then:q')
check 'V16 1,2;3,4,5;6?? — first group (1 AND 2) succeeds → then' 'then' "$out"

# V17: only middle AND group succeeds → then
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>nomatch:1??:f>hello:2??:f>hello:3??:f>hello:4??:f>hello:5??:f>nomatch:6??:1,2;3,4,5;6??p then:q')
check 'V17 1,2;3,4,5;6?? — middle group (3 AND 4 AND 5) succeeds → then' 'then' "$out"

# V18: only last OR operand succeeds → then
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>nomatch:1??:f>nomatch:2??:f>nomatch:3??:f>hello:4??:f>hello:5??:f>hello:6??:1,2;3,4,5;6??p then:q')
check 'V18 1,2;3,4,5;6?? — last operand (6) succeeds → then' 'then' "$out"

# V19: all groups fail → else
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>nomatch:1??:f>nomatch:2??:f>nomatch:3??:f>nomatch:4??:f>nomatch:5??:f>nomatch:6??:1,2;3,4,5;6??!p else:q')
check 'V19 1,2;3,4,5;6?? — all groups fail → else' 'else' "$out"

# W1: command succeeds, ??! captures as failure → else
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:1??!:1??!p else:q')
check 'W1 ??! — success captured as failure → else' 'else' "$out"

# W2: command fails, ??! captures as success → then
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>nomatch:1??!:1??p then:q')
check 'W2 ??! — failure captured as success → then' 'then' "$out"

# W3: inverted capture used in AND with normal capture
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>nomatch:1??!:f>hello:2??:1,2??p then:q')
check 'W3 ??! in AND — NOT(fail) AND success → then' 'then' "$out"

# W4: inverted capture in OR — NOT(success) OR success → then
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:1??!:f>hello:2??:1;2??p then:q')
check 'W4 ??! in OR — NOT(success) OR success → then' 'then' "$out"

# W5: both inverted — NOT(success) AND NOT(success) → else
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:1??!:f>hello:2??!:1,2??!p else:q')
check 'W5 ??! both inverted — NOT(success) AND NOT(success) → else' 'else' "$out"

# W6: invert the status of last command
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>hello:??!:??p hidden:f>nope:??!:??p show:q')
check 'W6 ??! inverted the status of last command' 'show' "$out"

# W7: pass the status of last command
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':err 1:f>nope:??:??!p hidden:q')
check 'W7 ?? pass the status of last command' 'hidden' "$out"

# M1/M2: pure deletion BEFORE mark — mark shifts down, undo restores
printf 'a\nb\nc\nd\ne\n' > "$TMPFILE"
out=$(run_ex ":4m 97:1,2d:'97=1:q")
check 'mark: pure delete before → mark shifts' '2' "$out"
out=$(run_ex ":4m 97:1,2d:ud:'97=1:q")
check 'mark: undo pure delete before → mark restored' '4' "$out"

# M3/M4: pure deletion AFTER mark — mark unchanged, undo leaves mark unchanged
printf 'a\nb\nc\nd\ne\n' > "$TMPFILE"
out=$(run_ex ":2m 97:4,5d:'97=1:q")
check 'mark: pure delete after → mark unchanged' '2' "$out"
out=$(run_ex ":2m 97:4,5d:ud:'97=1:q")
check 'mark: undo pure delete after → mark unchanged' '2' "$out"

# M5: pure deletion AT mark — mark invalidated, undo restores it
printf 'a\nb\nc\nd\ne\n' > "$TMPFILE"
out=$(run_ex ":3m 97:3d:ud:'97=1:q")
check 'mark: undo pure delete at mark → mark restored' '3' "$out"

# M6/M7: replacement COVERING mark (n_ins>0, n_del>0) — mark clamped, undo restores
# :1,3;0;d joins lines 1-3 into one replacement line; mark at line 3 is clamped to 1
printf 'aa\nbb\ncc\ndd\nee\n' > "$TMPFILE"
out=$(run_ex ":3m 97:1,3;0;d:'97=1:q")
check 'mark: replacement covers mark → mark clamped' '1' "$out"
out=$(run_ex ":3m 97:1,3;0;d:ud:'97=1:q")
check 'mark: undo replacement covering mark → mark restored' '3' "$out"

# M8/M9: replacement BEFORE mark — mark adjusts, undo restores
# :1,2;0;d replaces lines 1-2 with one line; mark at line 4 shifts to 3
printf 'aa\nbb\ncc\ndd\n' > "$TMPFILE"
out=$(run_ex ":4m 97:1,2;0;d:'97=1:q")
check 'mark: replacement before → mark adjusts' '3' "$out"
out=$(run_ex ":4m 97:1,2;0;d:ud:'97=1:q")
check 'mark: undo replacement before → mark restored' '4' "$out"

# M10/M11: replacement AFTER mark — mark unchanged, undo leaves mark unchanged
printf 'aa\nbb\ncc\ndd\n' > "$TMPFILE"
out=$(run_ex ":1m 97:3,4;0;d:'97=1:q")
check 'mark: replacement after → mark unchanged' '1' "$out"
out=$(run_ex ":1m 97:3,4;0;d:ud:'97=1:q")
check 'mark: undo replacement after → mark unchanged' '1' "$out"

# For :c the replacement lines are embedded directly in the EXINIT string.
# In -sm mode (xvis & 1) ec_insert uses term_read, which reads from term_push'd
# data (the arg), terminating on "." alone on a line.
# printf converts \n in its format to real newlines; ex_arg stops at ':' (xsep)
# so newlines inside the arg are part of the text, not command separators.
# All tests use a 5-line file: aa bb cc dd ee.

# MC1/MC2: n_ins=2, n_del=3 — :2,4c replaces lines 2-4 with "xx","yy"
# mark at line 4 (row 3) is in lossy zone [pos+n_ins, pos+n_del) = [3,4)
# forward: clamped to pos+n_ins-1 = 2 = line 3; undo: restored to line 4
printf 'aa\nbb\ncc\ndd\nee\n' > "$TMPFILE"
out=$(EXINIT="$(printf ':4m 97:2,4c xx\nyy:'"'"'97=1:q')" \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'mark: :c lossy zone (n_ins=2,n_del=3) → mark clamped' '3' "$out"
out=$(EXINIT="$(printf ':4m 97:2,4c xx\nyy:ud:'"'"'97=1:q')" \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'mark: undo :c lossy zone → mark restored' '4' "$out"

# MC3/MC4: mark inside changed region but before lossy zone (row 2 < pos+n_ins=3)
# not saved; stays at row 2 = line 3 both forward and after undo
printf 'aa\nbb\ncc\ndd\nee\n' > "$TMPFILE"
out=$(EXINIT="$(printf ':3m 97:2,4c xx\nyy:'"'"'97=1:q')" \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'mark: :c mark before lossy zone → mark unchanged' '3' "$out"
out=$(EXINIT="$(printf ':3m 97:2,4c xx\nyy:ud:'"'"'97=1:q')" \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'mark: undo :c; mark was before lossy zone → still unchanged' '3' "$out"

# MC5/MC6: n_ins=1, n_del=3 — :2,4c with 1 line; mark at row 2 falls inside
# lossy zone [pos+n_ins, pos+n_del) = [2,4) → clamped to pos+n_ins-1=1 = line 2;
# undo restores to line 3
printf 'aa\nbb\ncc\ndd\nee\n' > "$TMPFILE"
out=$(EXINIT="$(printf ':3m 97:2,4c xx:'"'"'97=1:q')" \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'mark: :c fully steps over mark (n_ins=1,n_del=3) → mark clamped' '2' "$out"
out=$(EXINIT="$(printf ':3m 97:2,4c xxn:ud:'"'"'97=1:q')" \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'mark: undo :c fully stepping over mark → mark restored' '3' "$out"

# MC9/MC10: mark after changed region (n_ins<n_del) → arithmetic −1; undo +1
printf 'aa\nbb\ncc\ndd\nee\n' > "$TMPFILE"
out=$(EXINIT="$(printf ':5m 97:2,4c xx\nyy:'"'"'97=1:q')" \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'mark: :c before mark (n_ins<n_del) → mark adjusts down' '4' "$out"
out=$(EXINIT="$(printf ':5m 97:2,4c xx\nyy:ud:'"'"'97=1:q')" \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'mark: undo :c before mark (n_ins<n_del) → mark restored' '5' "$out"

# MC11/MC12: n_ins=3, n_del=2 — mark at last deleted row (row 2 = pos+n_del-1)
# row 2 is within new insertion range [pos, pos+n_ins) = [1,4); not in empty lossy
# zone; not saved → mark stays at row 2 = line 3 forward and after undo
printf 'aa\nbb\ncc\ndd\nee\n' > "$TMPFILE"
out=$(EXINIT="$(printf ':3m 97:2,3c xx\nyy\nzz:'"'"'97=1:q')" \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'mark: :c deleted row within new range (n_ins>n_del) → not invalidated' '3' "$out"
out=$(EXINIT="$(printf ':3m 97:2,3c xx\nyy\nzz:ud:'"'"'97=1:q')" \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'mark: undo :c deleted row within new range → mark unchanged' '3' "$out"

# MC13/MC14: n_ins=3, n_del=2 — :2,3c inserts more than deleted; lossy zone empty
# mark after (row 4 = line 5) adjusts +1 → line 6; undo adjusts −1 → line 5
printf 'aa\nbb\ncc\ndd\nee\n' > "$TMPFILE"
out=$(EXINIT="$(printf ':5m 97:2,3c xx\nyy\nzz:'"'"'97=1:q')" \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'mark: :c before mark (n_ins>n_del) → mark adjusts up' '6' "$out"
out=$(EXINIT="$(printf ':5m 97:2,3c xx\nyy\nzz:ud:'"'"'97=1:q')" \
	"$VI" -sm "$TMPFILE" </dev/null 2>/dev/null)
check 'mark: undo :c before mark (n_ins>n_del) → mark restored' '5' "$out"

# ──────────────────────────────────────────────────────────────────────────────
# Substitute escape behavior — :s/pat/repl/ escapes and delimiter edge cases.
# The replacement layer drops a backslash before any char (keeping the char
# literal) EXCEPT digits 0-9 (backreferences; \0 = whole match) and \\ (one \).
# The delimiter and the ':' command separator are themselves escapable with \.
# ──────────────────────────────────────────────────────────────────────────────

printf 'a/b end\n' > "$TMPFILE"
out=$(run_ex ':%s/a\/b/X/:%p:q!')
check 'X1 \/ — escaped delimiter is literal in pattern' 'X end' "$out"

printf 'x\n' > "$TMPFILE"
out=$(run_ex ':%s/x/a\/b/:%p:q!')
check 'X2 \/ — escaped delimiter is literal in replacement' 'a/b' "$out"

printf 'x\n' > "$TMPFILE"
out=$(run_ex ':%s/x/a\\b/:%p:q!')
check 'X3 \\\\ — two backslashes become one literal backslash' 'a\b' "$out"

printf 'x\n' > "$TMPFILE"
out=$(run_ex ':%s/x/a\tb/:%p:q!')
check 'X4 \\t — backslash before ordinary char drops; no tab expansion' 'atb' "$out"

printf 'x\n' > "$TMPFILE"
out=$(run_ex ':%s/x/a\nb/:%p:q!')
check 'X5 \\n — no newline expansion in replacement; literal n' 'anb' "$out"

printf 'x\n' > "$TMPFILE"
out=$(run_ex ':%s/x/a\&b/:%p:q!')
check 'X6 \& — & is literal anyway; backslash drops' 'a&b' "$out"

printf 'ab\n' > "$TMPFILE"
out=$(run_ex ':%s/ab/&/:%p:q!')
check 'X7 & — ampersand is literal, not whole-match (nextvi uses \0)' '&' "$out"

printf 'a.b aXb a.b\n' > "$TMPFILE"
out=$(run_ex ':%s/a\.b/X/g:%p:q!')
check 'X8 \. — escaped dot matches literal dot only' 'X aXb X' "$out"

printf 'a.b axb\n' > "$TMPFILE"
out=$(run_ex ':%s/a.b/X/g:%p:q!')
check 'X9 . — unescaped dot is the any-char metachar' 'X X' "$out"

printf 'a\\b c\n' > "$TMPFILE"
out=$(run_ex ':%s/a\\b/X/:%p:q!')
check 'X10 \\\\ in pattern matches one literal backslash' 'X c' "$out"

printf 'ab\n' > "$TMPFILE"
out=$(run_ex ':%s/(a)(b)/\2\1/:%p:q!')
check 'X11 \1 \2 — backreference group swap' 'ba' "$out"

printf 'ab\n' > "$TMPFILE"
out=$(run_ex ':%s/ab/[\0]/:%p:q!')
check 'X12 \0 — backreference to the whole match' '[ab]' "$out"

printf 'x\n' > "$TMPFILE"
out=$(run_ex ':%s/x/a\1b/:%p:q!')
check 'X13 \1 with no such group — backslash drops; literal digit' 'a1b' "$out"

printf 'a/b c\n' > "$TMPFILE"
out=$(run_ex ':%s,a/b,X,:%p:q!')
check 'X14 , delimiter — / needs no escaping under a comma delimiter' 'X c' "$out"

printf 'a/b/c\n' > "$TMPFILE"
out=$(run_ex ':%s/\//_/g:%p:q!')
check 'X15 \/ g — every escaped delimiter replaced' 'a_b_c' "$out"

printf 'x/ z\n' > "$TMPFILE"
out=$(run_ex ':%s/x\/:%p:q!')
check 'X16 \/ ends pattern with no replacement section — match deleted' ' z' "$out"

# Delimiter/separator interaction: an unescaped ':' ends the s command; a
# backslash before ':' escapes the separator into the replacement text.
printf 'x z\n' > "$TMPFILE"
out=$(run_ex ':%s/x/a:b/:%p:q!')
check 'X17 : — unescaped separator ends replacement at "a"' 'a z' "$out"

printf 'x z\n' > "$TMPFILE"
out=$(run_ex ':%s/x/a\:b/:%p:q!')
check 'X18 \: — escaped separator stays literal in replacement' 'a:b z' "$out"

printf 'x z\n' > "$TMPFILE"
out=$(run_ex ':%s/x/a\\:b/:%p:q!')
check 'X19 \\\\: — \\ collapses to one \; the bare : still separates' 'a\ z' "$out"

printf 'x z\n' > "$TMPFILE"
out=$(run_ex ':%s/x/y\\:%p:q!')
check 'X20 trailing \\ before separator — replacement ends in literal \' 'y\ z' "$out"

printf 'x z\n' > "$TMPFILE"
out=$(run_ex ':%s/x/y\:%p:q!')
check 'X21 trailing \ escapes separator — :%p swallowed; nothing printed' '' "$out"

# ──────────────────────────────────────────────────────────────────────────────
# Search-range escape parity
# Each command leads with an inline-search range address ('>pat>' forward,
# '?pat?' / '? ...' backward) whose backslash runs sit right next to the search
# delimiter — the rarest corner of the escape-halving rule. On a backslash-free
# buffer the signal is whether the address parses to a real (unmatched) search
# → "invalid range", parses twice → two errors, or is fully absorbed → nothing.
# ──────────────────────────────────────────────────────────────────────────────

INVRANGE='invalid range'
INVRANGE2="$(printf 'invalid range\ninvalid range')"

printf 'irrelevant\n' > "$TMPFILE"
out=$(run_ex ':? ??p\\\:1q\:p BAD:q!')
check 'P1 ? ??p\\\:1q\:p BAD — escapes absorb :1q/:p; current line printed' \
	'irrelevant' "$out"

out=$(run_ex ':>\\>:reg:q!')
check 'P2 >\\> — \\ halves to one \; search runs, no match' "$INVRANGE" "$out"

out=$(run_ex ':>\\\>>:reg:q!')
check 'P3 >\\\>> — \> kept literal inside delimiter; search runs, no match' \
	"$INVRANGE" "$out"

out=$(run_ex ':>\\\\>:reg:q!')
check 'P4 >\\\\> — two backslashes; search runs, no match' "$INVRANGE" "$out"

out=$(run_ex ':>\\\\\>>:reg:q!')
check 'P5 >\\\\\>> — odd run keeps delimiter literal; search runs, no match' \
	"$INVRANGE" "$out"

out=$(run_ex ':>\\\\\\>:reg:q!')
check 'P6 >\\\\\\> — three backslashes; search runs, no match' "$INVRANGE" "$out"

out=$(run_ex ':? >\\\?:reg:q!')
check 'P7 ? >\\\? — backward delim after \ run; search runs, no match' \
	"$INVRANGE" "$out"

out=$(run_ex ':? >\\\\?:reg:q!')
check 'P8 ? >\\\\? — even run before delim; search runs, no match' \
	"$INVRANGE" "$out"

out=$(run_ex ':? >\\\\\?:reg:q!')
check 'P9 ? >\\\\\? — odd run keeps ? literal; search runs, no match' \
	"$INVRANGE" "$out"

out=$(run_ex ':? >\\\\\\?:reg:q!')
check 'P10 ? >\\\\\\? — even run before delim; search runs, no match' \
	"$INVRANGE" "$out"

out=$(run_ex ':? ?? 1?\:p:q!')
check 'P11 ? ?? 1?\:p — ex separator escape' \
	"irrelevant" "$out"

out=$(run_ex ':? ?? 1?\\:p:q!')
check 'P11 ? ?? 1?\\:p — ex separator escape' \
	"unknown command
irrelevant" "$out"

printf '\n%s\n' '─── Summary ──────────────────────────────────────────────────────────────────'

printf '\nResults: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
