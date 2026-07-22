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
	# same script without a shell (-e), on its own copy: both paths must
	# produce the same bytes and the same status
	cp "$TMPDIR/orig.txt" "$TMPDIR/result2.txt"
	sed "s|\$VI -e '[^']*'|\$VI -e '$TMPDIR/result2.txt'|" \
		"$TMPDIR/apply.sh" > "$TMPDIR/apply2.sh"
	local sh_rc=0 e_rc=0
	VI="$VI" "$TMPDIR/apply.sh" >/dev/null 2>&1 || sh_rc=$?
	./patch2vi -e "$TMPDIR/apply2.sh" >/dev/null 2>&1 || e_rc=$?
	if [ $sh_rc -ne 0 ] ||
	   ! diff -q "$TMPDIR/result.txt" "$TMPDIR/expected.txt" >/dev/null 2>&1; then
		fail "$name"
		echo "    --- expected ---"
		cat "$TMPDIR/expected.txt" | sed 's/^/    /'
		echo "    --- got ---"
		cat "$TMPDIR/result.txt" | sed 's/^/    /'
	elif [ "$e_rc" != "$sh_rc" ] ||
	     ! diff -q "$TMPDIR/result2.txt" "$TMPDIR/result.txt" >/dev/null 2>&1; then
		fail "$name (-e diverges: status $e_rc vs $sh_rc)"
		echo "    --- sh ---"
		cat "$TMPDIR/result.txt" | sed 's/^/    /'
		echo "    --- -e ---"
		cat "$TMPDIR/result2.txt" | sed 's/^/    /'
	else
		ok "$name"
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
	printf '%s' "$4" > "$TMPDIR/result2.txt"
	sed "s|\$VI -e '[^']*'|\$VI -e '$TMPDIR/result2.txt'|" \
		"$TMPDIR/apply.sh" > "$TMPDIR/apply2.sh"
	local sh_rc=0 e_rc=0
	VI="$VI" "$TMPDIR/apply.sh" >/dev/null 2>&1 || sh_rc=$?
	./patch2vi -e "$TMPDIR/apply2.sh" >/dev/null 2>&1 || e_rc=$?
	if [ $sh_rc -ne 0 ] ||
	   ! diff -q "$TMPDIR/result.txt" "$TMPDIR/drifted.txt" >/dev/null 2>&1; then
		fail "$name"
		echo "    --- want ---"; sed 's/^/    /' "$TMPDIR/drifted.txt"
		echo "    --- got ---"; sed 's/^/    /' "$TMPDIR/result.txt"
	elif [ "$e_rc" != "$sh_rc" ] ||
	     ! diff -q "$TMPDIR/result2.txt" "$TMPDIR/result.txt" >/dev/null 2>&1; then
		fail "$name (-e diverges: status $e_rc vs $sh_rc)"
		echo "    --- sh ---"; sed 's/^/    /' "$TMPDIR/result.txt"
		echo "    --- -e ---"; sed 's/^/    /' "$TMPDIR/result2.txt"
	else
		ok "$name"
	fi
}

# build via the script: it renames vi.c's main() around the compile
./build_patch2vi.sh clean >/dev/null
./build_patch2vi.sh build >/dev/null

echo "=== Script content tests ==="

# A one-line file leaves a single deduped search pattern; anything with
# context emits a multi-pattern fallback chain (%f>) instead.
check_script "single pattern uses .,\$f>" \
	"old
" \
	"new
" \
	'.,\\$f>' '>[^$]*>'

# A single anchored change emits a fallback chain. The whole-hunk
# pattern is multi-line (%f>, mode 0); the single-line fallbacks
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

check_script "multiline anchor uses %f>" \
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
	'%f>' ''

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
echo "=== Verbatim PHASE override tests ==="

# -i/-d open the built-in nextvi on /dev/tty, so these run patch2vi under
# script(1)'s pty. The editor session is driven entirely by P2VI_EX
# (patch2vi's test hook, run like EXINIT after the in-RAM buffers load):
# edits pipe the buffer through an awk filter with ":%!" and quit with
# "q!" - the buffer is read back as-is, there is no file and no saving.
# No keystrokes are sent, so the run is non-interactive and exits as fast
# as it starts. Each group's PHASE 1/PHASE 2 sections hold its verbatim
# ex-body segment bytes; editing them supersedes the structured sections
# (latest-edited wins, tie goes to verbatim).
if command -v script >/dev/null 2>&1; then

# Quit an untouched session: parses back to what was written = unedited
ED_TRUE="q"

# mkfilter <file> [<section> <old> <new>]...: awk filter that replaces OLD
# with NEW on every line inside sections whose header starts with SECTION
# ("PHASE 2", "EDIT COMMAND (rel)", ...)
mkfilter() {
	local f="$1"; shift
	printf '/^=== .* ===$/ { sect = substr($0, 5, length($0) - 8); print; next }\n' > "$f"
	while [ $# -gt 0 ]; do
		printf 'index(sect, "%s") == 1 { gsub(/%s/, "%s") }\n' \
			"$1" "$2" "$3" >> "$f"
		shift 3
	done
	printf '{ print }\n' >> "$f"
}

# edex <file> [<section> <old> <new>]...: P2VI_EX commands that pipe the
# buffer through a mkfilter awk program, then quit (discard flag: the
# edited buffer is read from RAM, never written)
edex() {
	mkfilter "$@"
	printf '%%!awk -f %s:q!' "$1"
}

# run_i <script-out> <excmds> <flags> <input>: patch2vi under a pty with
# the embedded editor driven by P2VI_EX
run_i() {
	P2VI_EX="$2" script -qec \
		"./patch2vi $3 '$4' > '$1' 2> '$TMPDIR/i_err.txt'" \
		/dev/null >/dev/null 2>&1
	chmod +x "$1"
}

# apply_i <script> <input-copy> <expected>: run the script on a fresh copy
apply_i() {
	cp "$TMPDIR/i_orig.txt" "$TMPDIR/result.txt"
	sed -i "s|\$VI -e '[^']*'|\$VI -e '$TMPDIR/result.txt'|" "$1"
	VI="$VI" "$1" >/dev/null 2>&1 &&
		diff -q "$TMPDIR/result.txt" "$2" >/dev/null 2>&1
}

printf 'ctx1\nprintf("foo");\nend\n' > "$TMPDIR/i_orig.txt"
printf 'ctx1\nprintf("bar");\nend\n' > "$TMPDIR/i_bar.txt"
printf 'ctx1\nprintf("baz");\nend\n' > "$TMPDIR/i_baz.txt"
printf 'ctx1\nprintf("qux");\nend\n' > "$TMPDIR/i_qux.txt"
printf 'ctx1\nprintf("BBB");\nend\n' > "$TMPDIR/i_bbb.txt"
diff -u "$TMPDIR/i_orig.txt" "$TMPDIR/i_bar.txt" > "$TMPDIR/i.patch" || true

# Unedited -i applies like -r
run_i "$TMPDIR/i1.sh" "$ED_TRUE" -ri "$TMPDIR/i.patch"
if apply_i "$TMPDIR/i1.sh" "$TMPDIR/i_bar.txt"; then
	ok "unedited -i applies"
else
	fail "unedited -i applies"
fi

# Editing a PHASE 2 blob overrides the generated segment and is recorded
# as a verbatim delta
run_i "$TMPDIR/i2.sh" "$(edex "$TMPDIR/ed1.awk" "PHASE 2" bar baz)" \
	-ri "$TMPDIR/i.patch"
if grep -q "verbatim mark" "$TMPDIR/i2.sh" &&
   apply_i "$TMPDIR/i2.sh" "$TMPDIR/i_baz.txt"; then
	ok "verbatim PHASE edit applies and is recorded"
else
	fail "verbatim PHASE edit applies and is recorded"
fi

# -d re-applies the stored verbatim override and reaches a fixed point
run_i "$TMPDIR/i3.sh" "$ED_TRUE" -rd "$TMPDIR/i2.sh"
run_i "$TMPDIR/i4.sh" "$ED_TRUE" -rd "$TMPDIR/i3.sh"
if diff -q "$TMPDIR/i3.sh" "$TMPDIR/i4.sh" >/dev/null 2>&1 &&
   apply_i "$TMPDIR/i3.sh" "$TMPDIR/i_baz.txt"; then
	ok "-d verbatim fixed point"
else
	fail "-d verbatim fixed point"
fi

# A structured edit on a group holding a stored override discards the
# override (preserved in .rej) and takes effect itself
run_i "$TMPDIR/i5.sh" "$(edex "$TMPDIR/ed2.awk" "EDIT COMMAND (rel)" bar qux)" \
	-rd "$TMPDIR/i3.sh"
if grep -q "discards verbatim override" "$TMPDIR/i_err.txt" &&
   ! grep -q "verbatim mark" "$TMPDIR/i5.sh" &&
   apply_i "$TMPDIR/i5.sh" "$TMPDIR/i_qux.txt"; then
	ok "structured edit discards stored override"
else
	fail "structured edit discards stored override"
fi
rm -f "$TMPDIR"/*.p2v.rej

# Both edited in one session: verbatim wins, structured edit is shadowed
run_i "$TMPDIR/i6.sh" \
	"$(edex "$TMPDIR/ed3.awk" "EDIT COMMAND (rel)" bar AAA "PHASE 2" bar BBB)" \
	-ri "$TMPDIR/i.patch"
if grep -q "shadowed by verbatim" "$TMPDIR/i_err.txt" &&
   apply_i "$TMPDIR/i6.sh" "$TMPDIR/i_bbb.txt"; then
	ok "verbatim edit shadows structured edit"
else
	fail "verbatim edit shadows structured edit"
fi

# A custom_text (group body) edit is kept in the delta even when a verbatim
# PHASE edit wins the session: custom_text doubles as the group-locator
# regex for starred LEVEL 2/4 matching, so it must survive the override.
run_i "$TMPDIR/i7.sh" \
	"$(edex "$TMPDIR/ed4.awk" "GROUP" foo XYZ "PHASE 2" bar BBB)" \
	-ri "$TMPDIR/i.patch"
if grep -q "verbatim mark" "$TMPDIR/i7.sh" &&
   grep -q "=== custom_text ===" "$TMPDIR/i7.sh" &&
   apply_i "$TMPDIR/i7.sh" "$TMPDIR/i_bbb.txt"; then
	ok "custom_text kept alongside verbatim override"
else
	fail "custom_text kept alongside verbatim override"
fi

# ... and it survives an unedited -d replay together with the override
run_i "$TMPDIR/i8.sh" "$ED_TRUE" -rd "$TMPDIR/i7.sh"
if grep -q "verbatim mark" "$TMPDIR/i8.sh" &&
   grep -q "=== custom_text ===" "$TMPDIR/i8.sh" &&
   apply_i "$TMPDIR/i8.sh" "$TMPDIR/i_bbb.txt"; then
	ok "custom_text + override -d fixed point"
else
	fail "custom_text + override -d fixed point"
fi

echo ""
echo "=== -E edit-to-script tests ==="

# -E edits a file in the built-in editor and turns the buffer it leaves
# behind into the script; the file itself is never written, so the diff
# comes from patch2vi's own differ, not from disk.
E_P2VI="$PWD/patch2vi"

# run_E <workdir> <excmds> <patch2vi args...>
run_E() {
	local d="$1" ex="$2"
	shift 2
	P2VI_EX="$ex" script -qec \
		"sh -c 'cd $d && $E_P2VI -E $*'" /dev/null >/dev/null 2>&1
}

mkdir -p "$TMPDIR/E1"
printf 'one\ntwo\nthree\nfour\nfive\nsix\n' > "$TMPDIR/E1/f.txt"
cp "$TMPDIR/E1/f.txt" "$TMPDIR/E1/orig.txt"
printf 'one\ntwo\nTHREE\nfour\nfive\nsix\n' > "$TMPDIR/E1/want.txt"
printf '/^three$/ { print "THREE"; next }\n{ print }\n' > "$TMPDIR/E1/filt.awk"
run_E "$TMPDIR/E1" '%!awk -f filt.awk:q!' f.txt out.sh

# the edited file stays untouched and the script is ready to run
if [ -x "$TMPDIR/E1/out.sh" ] &&
   diff -q "$TMPDIR/E1/f.txt" "$TMPDIR/E1/orig.txt" >/dev/null 2>&1; then
	ok "-E writes an executable script, not the file"
else
	fail "-E writes an executable script, not the file"
fi

# and applying it reproduces the edit
( cd "$TMPDIR/E1" && VI="$VI" ./out.sh ) >/dev/null 2>&1
if diff -q "$TMPDIR/E1/f.txt" "$TMPDIR/E1/want.txt" >/dev/null 2>&1; then
	ok "-E script applies the edit"
else
	fail "-E script applies the edit"
fi

# the embedded patch is a plain unified diff, byte for byte diff(1)'s
awk '/^=== PATCH2VI PATCH ===$/ { f = 1; next } f' "$TMPDIR/E1/out.sh" |
	tail -n +3 > "$TMPDIR/E1/mine.diff"
diff -u "$TMPDIR/E1/orig.txt" "$TMPDIR/E1/want.txt" |
	tail -n +3 > "$TMPDIR/E1/gnu.diff"
if diff -q "$TMPDIR/E1/gnu.diff" "$TMPDIR/E1/mine.diff" >/dev/null 2>&1; then
	ok "-E diff matches diff -u"
else
	fail "-E diff matches diff -u"
	diff "$TMPDIR/E1/gnu.diff" "$TMPDIR/E1/mine.diff" | sed 's/^/    /'
fi

# without a second argument the script goes to stdout
mkdir -p "$TMPDIR/E2"
cp "$TMPDIR/E1/orig.txt" "$TMPDIR/E2/f.txt"
cp "$TMPDIR/E1/filt.awk" "$TMPDIR/E2/filt.awk"
run_E "$TMPDIR/E2" '%!awk -f filt.awk:q!' 'f.txt > gen.sh'
chmod +x "$TMPDIR/E2/gen.sh"
( cd "$TMPDIR/E2" && VI="$VI" ./gen.sh ) >/dev/null 2>&1
if diff -q "$TMPDIR/E2/f.txt" "$TMPDIR/E1/want.txt" >/dev/null 2>&1; then
	ok "-E emits to stdout without an output file"
else
	fail "-E emits to stdout without an output file"
fi

# an unchanged buffer has nothing to convert
mkdir -p "$TMPDIR/E4"
cp "$TMPDIR/E1/orig.txt" "$TMPDIR/E4/f.txt"
run_E "$TMPDIR/E4" 'q' f.txt out.sh
if [ -x "$TMPDIR/E4/out.sh" ] &&
   ! grep -q '^# Patch:' "$TMPDIR/E4/out.sh"; then
	ok "-E on an untouched buffer emits no patch"
else
	fail "-E on an untouched buffer emits no patch"
fi

# a file that does not exist yet is a creation: /dev/null on the left,
# and -E still leaves the filesystem alone
mkdir -p "$TMPDIR/E3"
printf 'hello\nworld\n' > "$TMPDIR/E3/content.txt"
run_E "$TMPDIR/E3" 'r content.txt:q!' new.txt out.sh
if [ ! -e "$TMPDIR/E3/new.txt" ] &&
   grep -q '^--- /dev/null$' "$TMPDIR/E3/out.sh"; then
	ok "-E diffs a missing file as a creation"
else
	fail "-E diffs a missing file as a creation"
fi
( cd "$TMPDIR/E3" && VI="$VI" ./out.sh ) >/dev/null 2>&1
if diff -q "$TMPDIR/E3/new.txt" "$TMPDIR/E3/content.txt" >/dev/null 2>&1; then
	ok "-E creation script creates the file"
else
	fail "-E creation script creates the file"
fi

else
	echo "  SKIP: script(1) not available"
fi

echo ""
echo "=== -e multi-block tests ==="

# Two blocks in one script: the shell spawns a $VI per block, so block 2
# starts with empty registers. Block 2 gates its edit on a search served
# from register 98, which only block 1 filled - leaking that state (or
# block 1's buffer list, which would make b0 the wrong file) rewrites
# f1.txt instead and the comparison against the shell run fails.
mkdir -p "$TMPDIR/mb/sh" "$TMPDIR/mb/e"
for d in "$TMPDIR/mb/sh" "$TMPDIR/mb/e"; do
	printf 'alpha\nbeta\n' > "$d/f1.txt"
	printf 'gamma\ndelta\n' > "$d/f2.txt"
done
cat > "$TMPDIR/mb/two.sh" <<'EOS'
#!/bin/sh -e
VI=${VI:-vi}
SEP="$(printf '\001')"
( : > /tmp/p2vi.$$ ) 2>/dev/null && P2VIF=/tmp/p2vi.$$ || P2VIF=./p2vi.$$
trap 'rm -f "$P2VIF"' EXIT
printf '%s\n' "b0:%ya 98:1s/alpha/ALPHA/:w:2q" > "$P2VIF"
EXINIT='%ya 97:? %@97' $VI -e 'f1.txt' "$P2VIF"
printf '%s\n' "b0:fr 98:%f> beta:??1s/[ag][lm][pa][hm][aa]/LEAK/:w:2q" > "$P2VIF"
EXINIT='%ya 97:? %@97' $VI -e 'f2.txt' "$P2VIF"
exit 0
EOS
chmod +x "$TMPDIR/mb/two.sh"
mb_sh_rc=0 mb_e_rc=0
mb_p2vi="$PWD/patch2vi"
( cd "$TMPDIR/mb/sh" && VI="$VI" ../two.sh ) >/dev/null 2>&1 || mb_sh_rc=$?
( cd "$TMPDIR/mb/e" && "$mb_p2vi" -e ../two.sh ) >/dev/null 2>&1 || mb_e_rc=$?
if [ "$mb_sh_rc" = 0 ] && [ "$mb_e_rc" = 0 ] &&
   diff -r "$TMPDIR/mb/sh" "$TMPDIR/mb/e" >/dev/null 2>&1 &&
   grep -q ALPHA "$TMPDIR/mb/e/f1.txt" && grep -q gamma "$TMPDIR/mb/e/f2.txt"; then
	ok "-e runs every block with its own editor state"
else
	fail "-e runs every block with its own editor state (status $mb_e_rc vs $mb_sh_rc)"
	diff -r "$TMPDIR/mb/sh" "$TMPDIR/mb/e" | sed 's/^/    /'
fi

echo ""
echo "=== Results ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"
[ $FAIL -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $FAIL
