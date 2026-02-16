#!/bin/sh
# Tests for patch2vi relative mode: f> searches and s/ substitutes
# Run from the nextvi source directory
set -e

VI=${VI:-/bin/vi}
PASS=0
FAIL=0
TMPDIR=$(mktemp -d)

cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

ok() { PASS=$((PASS + 1)); printf "  PASS: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  FAIL: %s\n" "$1"; }

check() {
	# $1=test name $2=orig $3=expected $4=patch2vi flags (default: -r)
	local name="$1" flags="${4:--r}"
	printf '%s' "$2" > "$TMPDIR/orig.txt"
	printf '%s' "$3" > "$TMPDIR/expected.txt"
	diff -u "$TMPDIR/orig.txt" "$TMPDIR/expected.txt" > "$TMPDIR/test.patch" || true
	./patch2vi $flags "$TMPDIR/test.patch" > "$TMPDIR/apply.sh" 2>&1
	chmod +x "$TMPDIR/apply.sh"
	cp "$TMPDIR/orig.txt" "$TMPDIR/result.txt"
	# Fix the filename in the generated script
	sed -i "s|expected.txt.*'|result.txt'|" "$TMPDIR/apply.sh"
	if VI="$VI" "$TMPDIR/apply.sh" >/dev/null 2>&1 &&
	   diff -q "$TMPDIR/result.txt" "$TMPDIR/expected.txt" >/dev/null 2>&1; then
		ok "$name"
	else
		fail "$name"
		echo "    --- expected ---"
		cat "$TMPDIR/expected.txt" | sed 's/^/    /'
		echo "    --- got ---"
		cat "$TMPDIR/result.txt" | sed 's/^/    /'
	fi
}

# Also verify script content (no >pattern> anchors, uses f>)
check_script() {
	# $1=test name $2=orig $3=expected $4=pattern to find $5=pattern to reject
	local name="$1"
	printf '%s' "$2" > "$TMPDIR/orig.txt"
	printf '%s' "$3" > "$TMPDIR/expected.txt"
	diff -u "$TMPDIR/orig.txt" "$TMPDIR/expected.txt" > "$TMPDIR/test.patch" || true
	./patch2vi -r "$TMPDIR/test.patch" > "$TMPDIR/apply.sh" 2>&1
	local found=1 rejected=0
	if [ -n "$4" ] && ! grep -q "$4" "$TMPDIR/apply.sh"; then found=0; fi
	if [ -n "$5" ] && grep -q "$5" "$TMPDIR/apply.sh"; then rejected=1; fi
	if [ $found -eq 1 ] && [ $rejected -eq 0 ]; then
		ok "$name"
	else
		fail "$name"
		if [ $found -eq 0 ]; then echo "    expected pattern '$4' not found"; fi
		if [ $rejected -eq 1 ]; then echo "    rejected pattern '$5' found"; fi
		grep 'EXINIT' "$TMPDIR/apply.sh" | sed 's/^/    /'
	fi
}

cc -O2 -o patch2vi patch2vi.c

echo "=== Script content tests ==="

check_script "single anchor uses %f>" \
	"ctx1
old
end
" \
	"ctx1
new
end
" \
	'%f>' '>[^$]*>'

check_script "multiline anchor uses %;f>" \
	"ctx1
ctx2
ctx3
old
end
" \
	"ctx1
ctx2
ctx3
new
end
" \
	'%;f>' ''

check_script "no backstep in output" \
	"ctx1
old1
ctx2
old2
end
" \
	"ctx1
new1
ctx2
new2
end
" \
	'' '[.][-]1'

check_script "substitute for horizontal edit" \
	"ctx1
ctx2
ctx3
hello old world
end
" \
	"ctx1
ctx2
ctx3
hello new world
end
" \
	's/' ';[0-9][0-9]*c '

check_script "second search uses .,$f>" \
	"aaa
old1
bbb
old2
end
" \
	"aaa
new1
bbb
new2
end
" \
	'\\$f>' ''

echo ""
echo "=== End-to-end apply tests (relative mode) ==="

check "simple change" \
	"line1
line2
line3
old
line5
" \
	"line1
line2
line3
new
line5
"

check "horizontal substitute" \
	"line1
line2
line3
the old value here
line5
" \
	"line1
line2
line3
the new value here
line5
"

check "multiple groups" \
	"aaa
old1
bbb
ccc
ddd
old2
eee
" \
	"aaa
new1
bbb
ccc
ddd
new2
eee
"

check "pure delete" \
	"keep1
keep2
delete_me
keep3
keep4
" \
	"keep1
keep2
keep3
keep4
"

check "pure add" \
	"keep1
keep2
keep3
" \
	"keep1
keep2
added
keep3
"

check "multi-line delete" \
	"ctx1
ctx2
ctx3
del1
del2
del3
end
" \
	"ctx1
ctx2
ctx3
end
"

check "multi-line add" \
	"ctx1
ctx2
ctx3
end
" \
	"ctx1
ctx2
ctx3
add1
add2
add3
end
"

check "change with more adds than dels" \
	"ctx1
ctx2
ctx3
old1
end
" \
	"ctx1
ctx2
ctx3
new1
new2
new3
end
"

check "change with more dels than adds" \
	"ctx1
ctx2
ctx3
old1
old2
old3
end
" \
	"ctx1
ctx2
ctx3
new1
end
"

check "regex metacharacters in substitute" \
	"ctx1
ctx2
ctx3
foo.*bar+baz
end
" \
	"ctx1
ctx2
ctx3
foo.*qux+baz
end
"

check "backslash in content" \
	"ctx1
ctx2
ctx3
path\\old\\value
end
" \
	"ctx1
ctx2
ctx3
path\\new\\value
end
"

check "slash in substitute" \
	"ctx1
ctx2
ctx3
path/to/old
end
" \
	"ctx1
ctx2
ctx3
path/to/new
end
"

check "dollar sign in content" \
	"ctx1
ctx2
ctx3
price \$old here
end
" \
	"ctx1
ctx2
ctx3
price \$new here
end
"

check "follow context (no preceding ctx)" \
	"old
follow1
follow2
end
" \
	"new
follow1
follow2
end
"

check "single context line anchor" \
	"ctx1
old
end
" \
	"ctx1
new
end
"

echo ""
echo "=== Substitute uniqueness expansion tests ==="

# When a substitute pattern appears multiple times on a line, patch2vi
# should expand the diff region with surrounding context until it's unique.

check "duplicate substring: change second foo" \
	"ctx1
ctx2
ctx3
foo bar foo baz
end
" \
	"ctx1
ctx2
ctx3
foo bar qux baz
end
"

check "triple duplicate: change middle occurrence" \
	"ctx1
ctx2
ctx3
aa foo bb foo cc foo dd
end
" \
	"ctx1
ctx2
ctx3
aa foo bb qux cc foo dd
end
"

check "adjacent duplicates" \
	"ctx1
ctx2
ctx3
abab cdcd abab
end
" \
	"ctx1
ctx2
ctx3
abab cdcd efef
end
"

check "duplicate with regex metacharacters" \
	"ctx1
ctx2
ctx3
a.b+c a.b+c end
end
" \
	"ctx1
ctx2
ctx3
a.b+c a.x+c end
end
"

check_script "expansion includes context for uniqueness" \
	"ctx1
ctx2
ctx3
foo bar foo baz
end
" \
	"ctx1
ctx2
ctx3
foo bar qux baz
end
" \
	's/' ''

# When the diff covers the entire old line (can't make unique), should
# fall back to full-line change instead of substitute
check "full-line fallback when substring not unique" \
	"ctx1
ctx2
ctx3
abab
end
" \
	"ctx1
ctx2
ctx3
acab
end
"

check "duplicate at start of line" \
	"ctx1
ctx2
ctx3
xx yy xx zz
end
" \
	"ctx1
ctx2
ctx3
xx yy QQ zz
end
"

check "duplicate at end of line" \
	"ctx1
ctx2
ctx3
aa xx bb xx
end
" \
	"ctx1
ctx2
ctx3
aa QQ bb xx
end
"

check "UTF-8 duplicate expansion" \
	"ctx1
ctx2
ctx3
café bon café fin
end
" \
	"ctx1
ctx2
ctx3
café bon thé fin
end
"

echo ""
echo "=== Pure insertion substitute tests ==="

# When a line change is a pure insertion (old_text empty), patch2vi must
# expand to include surrounding context so s// never has an empty pattern.

check "pure insertion at end of args" \
	"ctx1
ctx2
ctx3
static void led_redraw(char *cs, int r, int orow, int crow, int ctop, int flg)
end
" \
	"ctx1
ctx2
ctx3
static void led_redraw(char *cs, int r, int orow, int crow, int ctop, int flg, int ai_max)
end
"

check_script "pure insertion produces non-empty s/ pattern" \
	"ctx1
ctx2
ctx3
static void led_redraw(char *cs, int r, int orow, int crow, int ctop, int flg)
end
" \
	"ctx1
ctx2
ctx3
static void led_redraw(char *cs, int r, int orow, int crow, int ctop, int flg, int ai_max)
end
" \
	's/.' 's//'

check "pure insertion at start of line" \
	"ctx1
ctx2
ctx3
world
end
" \
	"ctx1
ctx2
ctx3
hello world
end
"

check "pure insertion in middle" \
	"ctx1
ctx2
ctx3
foo bar
end
" \
	"ctx1
ctx2
ctx3
foo baz bar
end
"

check "empty old line changed to non-empty" \
	"ctx1
ctx2
ctx3

end
" \
	"ctx1
ctx2
ctx3
something
end
"

check_script "empty old line uses c not s/" \
	"ctx1
ctx2
ctx3

end
" \
	"ctx1
ctx2
ctx3
something
end
" \
	'c ' 's/'

echo ""
echo "=== End-to-end apply tests (block mode) ==="

check "block mode simple" \
	"line1
line2
line3
old
line5
" \
	"line1
line2
line3
new
line5
" \
	"-rb"

check "block mode multiple groups" \
	"aaa
old1
bbb
ccc
ddd
old2
eee
" \
	"aaa
new1
bbb
ccc
ddd
new2
eee
" \
	"-rb"

echo ""
echo "=== Consecutive substitute positioning tests ==="

# When two substitutes are emitted in a row, the second must have
# proper positioning. Previously empty context lines between changes
# caused has_rel/emit_rel_pos mismatch: has_rel=1 but no output.

check "consecutive substitutes with empty line between" \
	"ctx1
ctx2
ctx3
hello old1 world

goodbye old2 world
end
" \
	"ctx1
ctx2
ctx3
hello new1 world

goodbye new2 world
end
"

check "consecutive substitutes no context between" \
	"ctx1
ctx2
ctx3
the old1 value
the old2 value
end
" \
	"ctx1
ctx2
ctx3
the new1 value
the new2 value
end
"

check "consecutive substitutes with empty line between (block mode)" \
	"ctx1
ctx2
ctx3
hello old1 world

goodbye old2 world
end
" \
	"ctx1
ctx2
ctx3
hello new1 world

goodbye new2 world
end
" \
	"-rb"

check "consecutive substitutes no context between (block mode)" \
	"ctx1
ctx2
ctx3
the old1 value
the old2 value
end
" \
	"ctx1
ctx2
ctx3
the new1 value
the new2 value
end
" \
	"-rb"

echo ""
echo "=== Results ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"
[ $FAIL -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $FAIL
