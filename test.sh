#!/bin/sh
# Nextvi test suite - POSIX shell
# Tests ex commands (Section A) and vi normal mode (Section B)

PASS=0
FAIL=0
VI=./vi

check() {
	name="$1" expected="$2" actual="$3"
	if [ "$expected" = "$actual" ]; then
		PASS=$((PASS + 1))
	else
		FAIL=$((FAIL + 1))
		printf 'FAIL: %s\n  expected: |%s|\n  actual:   |%s|\n' \
			"$name" "$expected" "$actual"
	fi
}

check_exit() {
	name="$1" expected="$2" actual="$3"
	if [ "$expected" = "$actual" ]; then
		PASS=$((PASS + 1))
	else
		FAIL=$((FAIL + 1))
		printf 'FAIL: %s\n  expected exit: %s\n  actual exit:   %s\n' \
			"$name" "$expected" "$actual"
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

# ─── Section A: Ex Mode Tests ─────────────────────────────────────────────────

# A1: Print commands ───────────────────────────────────────────────────────────

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

# A2: Substitute ───────────────────────────────────────────────────────────────

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
out=$(run_ex ':s/void/x/:??p found?p not found:q')
check 'conditional else on sub no-match' 'not found' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':s/world/x/:??p found?p not found:q!')
check 'conditional then on sub match' 'found' "$out"

# A3: Delete ───────────────────────────────────────────────────────────────────

printf 'a\nb\nc\nd\ne\n' > "$TMPFILE"
out=$(run_ex ':2,4d:%p:q!')
check 'delete range 2-4' "$(printf 'a\ne')" "$out"

printf 'a\nb\nc\nd\n' > "$TMPFILE"
out=$(run_ex ':3,$d:%p:q!')
check 'delete to end' "$(printf 'a\nb')" "$out"

printf 'line1\n\nline2\n\nline3\n' > "$TMPFILE"
out=$(run_ex ':g/^$/d:%p:q!')
check 'delete empty lines (global)' "$(printf 'line1\nline2\nline3')" "$out"

# A4: Join ─────────────────────────────────────────────────────────────────────

printf 'a\nb\nc\n' > "$TMPFILE"
out=$(run_ex ':%-1j:%p:q!')
check 'join all no padding' 'abc' "$out"

printf 'a\nb\nc\n' > "$TMPFILE"
out=$(run_ex ':%-1jj:%p:q!')
check 'join all with space padding' 'a b c' "$out"

# A5: Registers ────────────────────────────────────────────────────────────────

printf 'hello\nworld\n' > "$TMPFILE"
out=$(run_ex ':1ya a:$pu a:%p:q!')
check 'yank line and paste at end' "$(printf 'hello\nworld\nhello')" "$out"

printf 'line1\nline2\nline3\n' > "$TMPFILE"
out=$(run_ex ':1ya a:2ya ax:1pu a:%p:q!')
check 'append to register then paste' \
	"$(printf 'line1\nline1\nline2\nline2\nline3')" "$out"

printf 'line1\n' > "$TMPFILE"
out=$(run_ex ':97reg hello:$pu a:%p:q!')
check 'put string into register via :reg' "$(printf 'line1\nhello')" "$out"

# A6: Global command ───────────────────────────────────────────────────────────

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

# A7: Conditionals (??) ────────────────────────────────────────────────────────

printf 'test int here\n' > "$TMPFILE"
out=$(run_ex ':f>int:??p found?p not found:q')
check 'conditional then on search found' 'found' "$out"

printf 'test int here\n' > "$TMPFILE"
out=$(run_ex ':f>void:??p found?p not found:q')
check 'conditional else on search not found' 'not found' "$out"

# A8: While loop (?) ───────────────────────────────────────────────────────────

printf 'a\nb\nc\nd\ne\n' > "$TMPFILE"
out=$(run_ex ':4? 1d:%p:q!')
check 'while loop counted delete' 'e' "$out"

printf 'old\nold\n' > "$TMPFILE"
out=$(run_ex ':10? s/old/new/:%p:q!')
check 'while loop breaks on xuerr (sub no-match)' "$(printf 'new\nold')" "$out"

# A9: External commands ────────────────────────────────────────────────────────

printf 'b\na\nc\n' > "$TMPFILE"
out=$(run_ex ':%!sort:%p:q!')
check 'sort buffer via !' "$(printf 'a\nb\nc')" "$out"

printf '' > "$TMPFILE"
out=$(run_ex ':led 0:r \!printf hello:led:%p:q!')
check 'read from pipe into empty buffer' 'hello' "$out"

# A10: Case sensitivity ────────────────────────────────────────────────────────

# ic defaults to 1 (case-insensitive); :ic toggles it to 0 (case-sensitive)
printf 'Hello\nhello\n' > "$TMPFILE"
out=$(run_ex ':g/hello/p:q')
check 'default ic=1: case-insensitive match' "$(printf 'Hello\nhello')" "$out"

printf 'Hello\nhello\n' > "$TMPFILE"
out=$(run_ex ':ic:g/hello/p:ic:q')
check ':ic toggles to case-sensitive match' 'hello' "$out"

# A11: Exit codes ──────────────────────────────────────────────────────────────

printf 'test\n' > "$TMPFILE"
run_ex ':q' >/dev/null 2>&1; rc=$?
check_exit 'exit code :q -> 0' '0' "$rc"

printf 'test\n' > "$TMPFILE"
run_ex ':q 3' >/dev/null 2>&1; rc=$?
check_exit 'exit code :q 3 -> 3' '3' "$rc"

# A12: Error handling (xuerr silent) ──────────────────────────────────────────

printf 'test\n' > "$TMPFILE"
out=$(run_ex ':f>nosuch:p ok:q')
check 'xuerr is silent and chain continues' 'ok' "$out"

# ─── Section B: Vi Normal Mode Tests ─────────────────────────────────────────

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

# ─── Section C: More Ex Mode Tests ───────────────────────────────────────────

# C1: Undo / Redo ──────────────────────────────────────────────────────────────

printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':%s/hello/world/:u:%p:q!')
check 'ex :u undoes substitute' 'hello' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':%s/hello/world/:u:rd:%p:q!')
check 'ex :rd redoes after undo' 'world' "$out"

# C2: Line marks ───────────────────────────────────────────────────────────────

printf 'a\nb\nc\nd\ne\n' > "$TMPFILE"
out=$(run_ex ":2m a:4m b:'a,'bp:q")
check 'marks: print range via marks' "$(printf 'b\nc\nd')" "$out"

# C3: Horizontal range ─────────────────────────────────────────────────────────

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':;6p:q')
check 'horizontal range ;6 prints from offset 6' 'world' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':;0;5p:q')
check 'horizontal range ;0;5 prints chars 0-4' 'hello' "$out"

# C4: Empty pattern reuse ──────────────────────────────────────────────────────

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':f>hello:%s//world/:%p:q!')
check 'empty pattern reuses last keyword' 'world world' "$out"

# C5: Regex features ───────────────────────────────────────────────────────────

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

# C6: Substitute with empty replacement ────────────────────────────────────────

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':%s/hello //:%p:q!')
check 'substitute with empty replacement deletes match' 'world' "$out"

# C7: Ranged search ────────────────────────────────────────────────────────────

printf 'int a;\nvoid b;\nint c;\n' > "$TMPFILE"
out=$(run_ex ':1,2f>void:.p:q')
check 'ranged :f> search then print current line' 'void b;' "$out"

# C8: Global with explicit range ───────────────────────────────────────────────

printf 'a\nb\nc\nd\ne\n' > "$TMPFILE"
out=$(run_ex ':2,3g/./p:q')
check 'global command with explicit range' "$(printf 'b\nc')" "$out"

# C9: :bs marks buffer saved ───────────────────────────────────────────────────

printf 'test\n' > "$TMPFILE"
run_ex ':s/test/done/:bs:q' >/dev/null 2>&1; rc=$?
check_exit ':bs marks buffer saved so :q exits 0' '0' "$rc"

# C10: Range arithmetic ────────────────────────────────────────────────────────

printf 'first\nsecond\nthird\n' > "$TMPFILE"
out=$(run_ex ':$-1p:q')
check 'range arithmetic $-1 prints second-to-last' 'second' "$out"

# C11: External tr pipe ────────────────────────────────────────────────────────

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':%!tr a-z A-Z:%p:q!')
check 'pipe through tr a-z A-Z uppercases' 'HELLO WORLD' "$out"

# C12: Register expansion %@a ──────────────────────────────────────────────────

printf 'test\n' > "$TMPFILE"
out=$(run_ex ':97reg hello:p %@a:q')
check 'register expansion %@a in :p' 'hello' "$out"

# C13: err option ──────────────────────────────────────────────────────────────

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':err 3:s/void/x/:p after:q')
check 'err=3 breaks chain on xuerr' '' "$out"

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':err 4:s/void/x/:p after:q')
check 'err=4 silences error and chain continues' 'after' "$out"

# C14: :wq! exit code ──────────────────────────────────────────────────────────

printf 'test\n' > "$TMPFILE"
run_ex ':wq! /dev/null' >/dev/null 2>&1; rc=$?
check_exit ':wq! exits 0' '0' "$rc"

# C15: :f< reverse search ──────────────────────────────────────────────────────

printf 'abc\nxyz\nabc\n' > "$TMPFILE"
out=$(run_ex ':3f<abc:p:q')
check ':f< searches backward' 'abc' "$out"

# C16: Global inverted with chained command ────────────────────────────────────

printf 'int a;\nvoid b;\nint c;\n' > "$TMPFILE"
out=$(run_ex ':g!/int/s/void/VOID/\:p:q!')
check 'g! inverted global chained sub+print' 'VOID b;' "$out"

# C17: Print line number = with arithmetic ─────────────────────────────────────

printf 'a\nb\nc\nd\ne\n' > "$TMPFILE"
out=$(run_ex ':,2+3=1:q')
check 'range arithmetic ,2+3 prints 5' '5' "$out"

# C18: Multiple registers ──────────────────────────────────────────────────────

printf 'line1\nline2\nline3\n' > "$TMPFILE"
out=$(run_ex ':1ya a:3ya b:1pu b:1pu a:%p:q!')
check 'yank two registers and paste both' \
	"$(printf 'line1\nline1\nline3\nline2\nline3')" "$out"

# ─── Section D: More Vi Mode Tests ────────────────────────────────────────────

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

# ─── Section E: Macro system (:& / vi &a / &&) ────────────────────────────────

# E1: :& takes raw vi input; to use a register, expand it via %@a
printf 'hello world\n' > "$TMPFILE"
check 'E1 :& %@a — register expansion used as raw vi input' 'world' \
	"$(run_mac ":97reg dw:& %@a:w! $OUTFILE:q!")"

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

# ─── Section G: ya! — free a named register ───────────────────────────────────

# ya! a frees register a; pu a on a freed register raises "uninitialized
# register" — with err 4 (silence+ignore) the paste is skipped silently.
printf 'line1\nline2\n' > "$TMPFILE"
out=$(run_ex ':1ya a:ya! a:err 4:$pu a:%p:q!')
check 'G1 ya! frees named reg; pu a silently skipped (err 4)' \
	"$(printf 'line1\nline2')" "$out"

# ─── Section H: ?? id capture ─────────────────────────────────────────────────

# {#id}?? captures the current error status into id; [#id]?? branches on it.
# An intervening command that changes the error status does NOT override it.
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':f>void:5??:s/hello/world/:5??p found?p notfound:q!')
check 'H1 ?? id captures fail; intervening success does not override' 'notfound' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':f>hello:5??:f>void:5??p found?p notfound:q')
check 'H2 ?? id captures success; intervening fail does not override' 'found' "$out"

# ─── Section I: ??! inverted conditional and ?! inverted while loop ────────────

printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':f>void:??!p not found?p found:q')
check 'I1 ??! — fail takes first branch' 'not found' "$out"

printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':f>hello:??!p not found?p found:q')
check 'I2 ??! — success takes second branch' 'found' "$out"

# ?! runs the body while the condition fails; 1d modifies buffer state and
# persists across iterations until 'yes' surfaces to line 1.
printf 'no\nno\nyes\nno\n' > "$TMPFILE"
out=$(run_ex ':10?! f>yes?1d:.p:q!')
check 'I3 ?! while: 1d deletes first line each pass until f>yes succeeds' 'yes' "$out"

# ─── Section J: seq — undo sequencing ─────────────────────────────────────────

# seq 0 groups all subsequent changes into a single undo step
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':seq 0:s/hello/step1/:s/step1/step2/:s/step2/final/:seq:u:%p:q!')
check 'J1 seq 0 — batch changes undo as one step' 'hello' "$out"

# seq -1 disables undo tracking entirely; :u has no effect
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':seq -1:s/hello/world/:u:%p:seq:q!')
check 'J2 seq -1 — undo tracking disabled; u has no effect' 'world' "$out"

# ─── Section K: pr — capture :p output into a register ───────────────────────

# pr N redirects :p output to register N; led 0 suppresses double-printing
printf 'hello\n' > "$TMPFILE"
out=$(run_ex ':led 0:pr 97:ya! a:p hello captured:pr 0:led:pu a:%p:q!')
check 'K1 pr+led 0 — :p output captured into register, then pasted' \
	"$(printf 'hello\nhello captured')" "$out"

# ─── Section L: special marks ─────────────────────────────────────────────────

printf 'a\nb\nc\nd\n' > "$TMPFILE"
out=$(run_ex ":2,3s/./X/:'[p:q!")
check "L1 '[ marks first changed line" 'X' "$out"

printf 'a\nb\nc\nd\n' > "$TMPFILE"
out=$(run_ex ":2,3s/./X/:']p:q!")
check "L2 '] marks last changed line" 'X' "$out"

# '* = cursor position saved BEFORE the previous ex command ran
printf 'a\nb\nc\nd\n' > "$TMPFILE"
out=$(run_ex ":3p:'*p:q")
check "L3 '* = cursor saved before previous ex command" \
	"$(printf 'c\na')" "$out"

# ─── Section M: %@/ — previous regex register ─────────────────────────────────

printf 'hello world\n' > "$TMPFILE"
out=$(run_ex ':f>hello:p %@/:q')
check 'M1 %@/ expands to the previous regex keyword' 'hello' "$out"

# ─── Section N: range arithmetic ──────────────────────────────────────────────

# $*5/10 — navigate to 50% of the file (integer arithmetic on last line)
printf 'a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n' > "$TMPFILE"
out=$(run_ex ':$*5/10p:q')
check 'N1 $*5/10 — navigate to 50% of file' 'e' "$out"

# ;5;#+10 — # rebases to the previous semicolon value (5), so +10 = 15;
# = 3 prints the second char offset (the final computed value).
printf 'hello world extra\n' > "$TMPFILE"
out=$(run_ex ':;5;#+10= 3:q')
check 'N2 ;5;#+10= 3 — second char offset via # rebase is 15' '15' "$out"

# ─── Section O: vi bracket matching (%) ───────────────────────────────────────

# f( lands on (; \% passes a literal % (not buffer path) to the & macro
printf 'foo(bar)qux\n' > "$TMPFILE"
out=$(run_vi 'f(\%x')
check 'O1 vi f(\%)x — find (, jump to matching ), delete )' 'foo(barqux' "$out"

# ─── Section P: :pu and :w with external pipe ─────────────────────────────────

rm -f "$OUTFILE"
printf 'test\n' > "$TMPFILE"
run_ex ":97reg hello world:pu a \!tr a-z A-Z > $OUTFILE:q" >/dev/null 2>/dev/null
check 'P1 :pu a \!cmd — pipe register content to external command' \
	'HELLO WORLD' "$(cat $OUTFILE 2>/dev/null)"

rm -f "$OUTFILE"
printf 'hello world\n' > "$TMPFILE"
run_ex ":1,1w \!tr a-z A-Z > $OUTFILE:q" >/dev/null 2>/dev/null
check 'P2 :w \!cmd — write buffer range to external command' \
	'HELLO WORLD' "$(tr -d '\n' < $OUTFILE 2>/dev/null)"

# ─── Summary ──────────────────────────────────────────────────────────────────

printf '\nResults: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
