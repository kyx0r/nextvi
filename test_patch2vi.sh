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
	# Redirect $VI -e arg from expected.txt to result.txt
	sed -i "s|\$VI -e '[^']*'|\$VI -e '$TMPDIR/result.txt'|" "$TMPDIR/apply.sh"
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

# Generate a script from orig->expected, then apply it to a *drifted* file to
# confirm the substitute progression (exact -> grp-absorbing)
# absorbs drift.
# $1=name $2=orig $3=expected(script source) $4=drifted input $5=drifted result
check_drift() {
	local name="$1"
	printf '%s' "$2" > "$TMPDIR/orig.txt"
	printf '%s' "$3" > "$TMPDIR/expected.txt"
	diff -u "$TMPDIR/orig.txt" "$TMPDIR/expected.txt" > "$TMPDIR/test.patch" || true
	./patch2vi -r "$TMPDIR/test.patch" > "$TMPDIR/apply.sh" 2>&1
	chmod +x "$TMPDIR/apply.sh"
	printf '%s' "$4" > "$TMPDIR/result.txt"
	printf '%s' "$5" > "$TMPDIR/drifted.txt"
	sed -i "s|\$VI -e '[^']*'|\$VI -e '$TMPDIR/result.txt'|" "$TMPDIR/apply.sh"
	if VI="$VI" "$TMPDIR/apply.sh" >/dev/null 2>&1 &&
	   diff -q "$TMPDIR/result.txt" "$TMPDIR/drifted.txt" >/dev/null 2>&1; then
		ok "$name"
	else
		fail "$name"
		echo "    --- want ---"; sed 's/^/    /' "$TMPDIR/drifted.txt"
		echo "    --- got ---"; sed 's/^/    /' "$TMPDIR/result.txt"
	fi
}

cc -O2 -o patch2vi patch2vi.c

echo "=== Script content tests ==="

# A one-line file leaves a single deduped search pattern; anything with
# context emits a multi-pattern fallback chain (%;f>) instead.
check_script "single pattern uses .,\$f>" \
	"old
" \
	"new
" \
	'.,\\$f>' '>[^$]*>'

# A single anchored change emits a fallback chain. The whole-hunk
# pattern is multi-line (%;f>, mode 0); the single-line fallbacks
# (top context, deleted line) default to mode 1 and search the live
# buffer with .,$f> instead. The chain opens the ? conditional with a
# ${LB} readability break before the first search (each attempt starts
# on its own source line); emit_search (single pattern) never opens
# with '?', so this stays chain-specific.
check_script "anchored change uses fallback chain" \
	"ctx1
old
end
" \
	"ctx1
new
end
" \
	'?${ESC}${SEP}${LB}' ''

check_script "single-line chain fallbacks use .,\$f>" \
	"ctx1
old
end
" \
	"ctx1
new
end
" \
	'\.,\\$f>' ''

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

check_script "searches reuse register cache" \
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
	'%ya 98' '\\$;f>'

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
echo "=== Grp-capture absorbing substitute tests ==="

# Two changed spots with a stable island between. The progression emits, in
# order: exact (rung 0, the minimal contiguous span) and one grp rung built over
# that SAME span -- the island is wildcarded so it absorbs drift, with no leading
# or trailing "(.*)" (those would just dup the unanchored exact rung). Both edit
# separators X/Y are unique, so the island is a full "(.*)": s/X(.*)Y/P\1Q/.
check_script "two-spot change emits exact rung first" \
	"top
aaaaaaaaaa X bbbbbbbbbb Y cccccccccc
bot
" \
	"top
aaaaaaaaaa P bbbbbbbbbb Q cccccccccc
bot
" \
	's/X bbbbbbbbbb Y/' ''
check_script "two-spot change emits absorbing grp rung" \
	"top
aaaaaaaaaa X bbbbbbbbbb Y cccccccccc
bot
" \
	"top
aaaaaaaaaa P bbbbbbbbbb Q cccccccccc
bot
" \
	'X([.][*])Y' '([.][*])X'

# Same shape applies correctly end-to-end (island preserved verbatim).
check "two-spot grp substitute applies" \
	"top
aaaaaaaaaa X bbbbbbbbbb Y cccccccccc
bot
" \
	"top
aaaaaaaaaa P bbbbbbbbbb Q cccccccccc
bot
"

# Inserting characters between stable anchors (the canonical case). The span is
# "int vi_cn" -> "aint vib_cnc"; the "int vi" run fuzzes to "(in.*vi)" (minimal
# unique head "in" / tail "vi") so it absorbs drift between them, while "_cn" is
# too short and stays literal. No leading/trailing "(.*)": s/(in.*vi)(_cn)/a\1b\2c/.
check_script "insertion between anchors emits absorbing grp" \
	"static int vi_cndir = 1;
" \
	"static aint vib_cncdir = 1;
" \
	'(in[.][*]vi)(_cn)' '([.][*])(int'
check "insertion between anchors applies" \
	"static int vi_cndir = 1;
" \
	"static aint vib_cncdir = 1;
"

# Regression: two insertions bracketing a stable middle. The span is built over
# the changed region only; the unchanged prefix and the short trailing run "|\\"
# are NOT wrapped in "(.*)" absorbers (that would dup the unanchored exact rung).
# Earlier this emitted an external "(.*)...(.*)" form; now it must not -- the grp
# is just the span "(f!.*c!?|)" with insertions injected. The line must be long
# enough that the cost model picks STRAT_REL (s///) over STRAT_RELC (;c).
check_script "short trailing anchor emits span-only grp" \
	"line one stays
line two stays
line three stays
|[@&!dmj]|=\\\\?{0,1}|\\\\?{1,2}[?!]?|b[psx]?|p[uh]?|ac|e[f!]?!?|f[-+><tdp]?|inc|i|sc!?|\\
line five stays
line six stays
line seven stays
" \
	"line one stays
line two stays
line three stays
|[@&!dmj]|=\\\\?{0,1}|\\\\?{1,2}[?!]?|b[psx]?|p[uh]?|ac|e[qf!]?!?|f[-+><tdp]?|inc|i|sc!?|vs|sp|\\
line five stays
line six stays
line seven stays
" \
	'(f![.][*]' '([.][*])(f'
check "short trailing anchor grp applies" \
	"line one stays
line two stays
line three stays
|[@&!dmj]|=\\\\?{0,1}|\\\\?{1,2}[?!]?|b[psx]?|p[uh]?|ac|e[f!]?!?|f[-+><tdp]?|inc|i|sc!?|\\
line five stays
line six stays
line seven stays
" \
	"line one stays
line two stays
line three stays
|[@&!dmj]|=\\\\?{0,1}|\\\\?{1,2}[?!]?|b[psx]?|p[uh]?|ac|e[qf!]?!?|f[-+><tdp]?|inc|i|sc!?|vs|sp|\\
line five stays
line six stays
line seven stays
"

# The middle anchor is flanked by insertions on both sides ("q" before, "vs|sp"
# after), so it cannot become a full "(.*)" (ambiguous). The grp instead keeps
# the MINIMAL head/tail runes that are each unique in the old line (here head
# "f!", tail "c!?|") and wildcards the middle -- "(f!.*c!?|)" -- which absorbs
# drift *inside* the stable region ("inc" -> "incZZ" on disk) while still
# injecting the insertions. The exact rung fails on the drifted interior; only
# the fuzz grp fires. The "f!.*" marker checks a literal-then-wildcard group.
check_script "insertion-flanked middle emits fuzz grp" \
	"line one stays
line two stays
line three stays
|[@&!dmj]|=\\\\?{0,1}|\\\\?{1,2}[?!]?|b[psx]?|p[uh]?|ac|e[f!]?!?|f[-+><tdp]?|inc|i|sc!?|\\
line five stays
line six stays
line seven stays
" \
	"line one stays
line two stays
line three stays
|[@&!dmj]|=\\\\?{0,1}|\\\\?{1,2}[?!]?|b[psx]?|p[uh]?|ac|e[qf!]?!?|f[-+><tdp]?|inc|i|sc!?|vs|sp|\\
line five stays
line six stays
line seven stays
" \
	'f![.][*]' ''
check_drift "fuzz grp absorbs interior drift" \
	"line one stays
line two stays
line three stays
|[@&!dmj]|=\\\\?{0,1}|\\\\?{1,2}[?!]?|b[psx]?|p[uh]?|ac|e[f!]?!?|f[-+><tdp]?|inc|i|sc!?|\\
line five stays
line six stays
line seven stays
" \
	"line one stays
line two stays
line three stays
|[@&!dmj]|=\\\\?{0,1}|\\\\?{1,2}[?!]?|b[psx]?|p[uh]?|ac|e[qf!]?!?|f[-+><tdp]?|inc|i|sc!?|vs|sp|\\
line five stays
line six stays
line seven stays
" \
	"line one stays
line two stays
line three stays
|[@&!dmj]|=\\\\?{0,1}|\\\\?{1,2}[?!]?|b[psx]?|p[uh]?|ac|e[f!]?!?|f[-+><tdp]?|incZZ|i|sc!?|\\
line five stays
line six stays
line seven stays
" \
	"line one stays
line two stays
line three stays
|[@&!dmj]|=\\\\?{0,1}|\\\\?{1,2}[?!]?|b[psx]?|p[uh]?|ac|e[qf!]?!?|f[-+><tdp]?|incZZ|i|sc!?|vs|sp|\\
line five stays
line six stays
line seven stays
"

# The progression absorbs drift: the exact rung fails when the on-disk island
# drifted, but the grp rung's interior "(.*)" absorbs it.
check_drift "grp rung absorbs island drift" \
	"top
aaaaaaaaaa X bbbbbbbbbb Y cccccccccc
bot
" \
	"top
aaaaaaaaaa P bbbbbbbbbb Q cccccccccc
bot
" \
	"top
aaaaaaaaaa X bbZZbbZZbb Y cccccccccc
bot
" \
	"top
aaaaaaaaaa P bbZZbbZZbb Q cccccccccc
bot
"

# A full "(.*)" interior is only safe when both bordering edit separators are
# unique in the old line. When the separator repeats (here both edits are "Z"),
# the bare "Z(.*)Z" split would be ambiguous, so the middle is demoted to a fuzz
# "( b.*b )" whose literal head/tail pin the repeated "Z": s/Z( b.*b )Z/P\1Q/.
# The ambiguous middle wild "Z(.*)Z" must NOT appear.
check_script "repeated separator demotes wild middle" \
	"c1
c2
c3
aaaaaaaaaa Z bbbbbbbbbb Z cccccccccc
c5
c6
c7
" \
	"c1
c2
c3
aaaaaaaaaa P bbbbbbbbbb Q cccccccccc
c5
c6
c7
" \
	'Z( b' 'Z([.][*])Z'
check "repeated separator grp applies" \
	"c1
c2
c3
aaaaaaaaaa Z bbbbbbbbbb Z cccccccccc
c5
c6
c7
" \
	"c1
c2
c3
aaaaaaaaaa P bbbbbbbbbb Q cccccccccc
c5
c6
c7
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

echo ""
echo "=== Insert before first line tests ==="

check "insert before line 1 (relative mode)" \
	"first line
second line
third line
" \
	"new header
first line
second line
third line
" \
	"-r"

echo ""
echo "=== Results ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"
[ $FAIL -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $FAIL
