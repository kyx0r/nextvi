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
echo "=== Results ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"
[ $FAIL -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $FAIL
