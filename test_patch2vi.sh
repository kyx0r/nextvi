#!/bin/sh
# Tests for patch2vi relative mode: f> searches and s/ substitutes
# Run from the nextvi source directory
set -e

# The generated scripts' phase switches are read from the environment (a
# replay resolves ${DBG1:+...} etc through getenv too), so an exported one
# silently rewrites what every test runs. INTR=1 is the worst of them: an
# error site opens the script in vi and a pty test then waits for a
# keystroke forever. Start from the documented defaults.
unset DBG1 DBG2 QF1 QF2 INTR

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
	'\.,$f>' '>[^$]*>'

# A single anchored change emits a fallback chain. The whole-hunk
# pattern is multi-line (%f>, mode 0); the single-line fallbacks
# (top context, deleted line) default to mode 1 and search the live
# buffer with .,$f> instead. The chain opens the ? conditional with a
# line-break no-op before the first search (each attempt starts
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
	'?..0?$' ''

check_script "single-line chain fallbacks use .,\$f>" \
	"ctx1
old
end
" \
	"ctx1
new
end
" \
	'\.,$f>' ''

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
echo "=== -I edit-to-script tests ==="

# -I edits a file in the built-in editor and turns the buffer it leaves
# behind into the script; the file itself is never written, so the diff
# comes from patch2vi's own differ, not from disk.
E_P2VI="$PWD/patch2vi"

# run_I <workdir> <excmds> <patch2vi args...>; the session is nextvi's own
# main(), so the ex commands are driven through EXINIT like in vi(1)
run_I() {
	local d="$1" ex="$2"
	shift 2
	EXINIT="$ex" script -qec \
		"sh -c 'cd $d && $E_P2VI -I $*'" /dev/null >/dev/null 2>&1
}

mkdir -p "$TMPDIR/E1"
printf 'one\ntwo\nthree\nfour\nfive\nsix\n' > "$TMPDIR/E1/f.txt"
cp "$TMPDIR/E1/f.txt" "$TMPDIR/E1/orig.txt"
printf 'one\ntwo\nTHREE\nfour\nfive\nsix\n' > "$TMPDIR/E1/want.txt"
printf '/^three$/ { print "THREE"; next }\n{ print }\n' > "$TMPDIR/E1/filt.awk"
run_I "$TMPDIR/E1" '%!awk -f filt.awk:q!' 'f.txt > out.sh'

# the script goes to stdout, the edited file stays untouched
if [ -s "$TMPDIR/E1/out.sh" ] &&
   diff -q "$TMPDIR/E1/f.txt" "$TMPDIR/E1/orig.txt" >/dev/null 2>&1; then
	ok "-I writes a script to stdout, not the file"
else
	fail "-I writes a script to stdout, not the file"
fi

# and applying it reproduces the edit
( cd "$TMPDIR/E1" && VI="$VI" sh out.sh ) >/dev/null 2>&1
if diff -q "$TMPDIR/E1/f.txt" "$TMPDIR/E1/want.txt" >/dev/null 2>&1; then
	ok "-I script applies the edit"
else
	fail "-I script applies the edit"
fi

# the embedded patch is a plain unified diff, byte for byte diff(1)'s
awk '/^=== PATCH2VI PATCH ===$/ { f = 1; next } f' "$TMPDIR/E1/out.sh" |
	tail -n +3 > "$TMPDIR/E1/mine.diff"
diff -u "$TMPDIR/E1/orig.txt" "$TMPDIR/E1/want.txt" |
	tail -n +3 > "$TMPDIR/E1/gnu.diff"
if diff -q "$TMPDIR/E1/gnu.diff" "$TMPDIR/E1/mine.diff" >/dev/null 2>&1; then
	ok "-I diff matches diff -u"
else
	fail "-I diff matches diff -u"
	diff "$TMPDIR/E1/gnu.diff" "$TMPDIR/E1/mine.diff" | sed 's/^/    /'
fi

# every positional argument is a file to open, in order: b0 is the first
# one, the rest are reachable with :b without a single :e
mkdir -p "$TMPDIR/E2"
printf 'p1\np2\n' > "$TMPDIR/E2/p.txt"
printf 'q1\nq2\n' > "$TMPDIR/E2/q.txt"
printf 'r1\nr2\n' > "$TMPDIR/E2/r.txt"
run_I "$TMPDIR/E2" '%s/p1/P1/:b1:%s/q2/Q2/:b2:%s/r1/R1/:q!' \
	'p.txt q.txt r.txt p.txt > out.sh'
if grep -q '^# Patch: p.txt q.txt r.txt$' "$TMPDIR/E2/out.sh"; then
	ok "-I opens every file named on the command line, once each"
else
	fail "-I opens every file named on the command line, once each"
	grep '^# Patch:' "$TMPDIR/E2/out.sh" | sed 's/^/    /'
fi
( cd "$TMPDIR/E2" && VI="$VI" sh out.sh ) >/dev/null 2>&1
printf 'P1\np2\n' > "$TMPDIR/E2/want_p.txt"
printf 'q1\nQ2\n' > "$TMPDIR/E2/want_q.txt"
printf 'R1\nr2\n' > "$TMPDIR/E2/want_r.txt"
if diff -q "$TMPDIR/E2/p.txt" "$TMPDIR/E2/want_p.txt" >/dev/null 2>&1 &&
   diff -q "$TMPDIR/E2/q.txt" "$TMPDIR/E2/want_q.txt" >/dev/null 2>&1 &&
   diff -q "$TMPDIR/E2/r.txt" "$TMPDIR/E2/want_r.txt" >/dev/null 2>&1; then
	ok "-I multi-file script applies to every file"
else
	fail "-I multi-file script applies to every file"
fi

# an unchanged buffer has nothing to convert
mkdir -p "$TMPDIR/E4"
cp "$TMPDIR/E1/orig.txt" "$TMPDIR/E4/f.txt"
run_I "$TMPDIR/E4" 'q' 'f.txt > out.sh'
if [ -s "$TMPDIR/E4/out.sh" ] &&
   ! grep -q '^# Patch:' "$TMPDIR/E4/out.sh"; then
	ok "-I on an untouched buffer emits no patch"
else
	fail "-I on an untouched buffer emits no patch"
fi

# a file that does not exist yet is a creation: /dev/null on the left,
# and -I still leaves the filesystem alone
mkdir -p "$TMPDIR/E3"
printf 'hello\nworld\n' > "$TMPDIR/E3/content.txt"
run_I "$TMPDIR/E3" 'r content.txt:q!' 'new.txt > out.sh'
if [ ! -e "$TMPDIR/E3/new.txt" ] &&
   grep -q '^--- /dev/null$' "$TMPDIR/E3/out.sh"; then
	ok "-I diffs a missing file as a creation"
else
	fail "-I diffs a missing file as a creation"
fi
( cd "$TMPDIR/E3" && VI="$VI" sh out.sh ) >/dev/null 2>&1
if diff -q "$TMPDIR/E3/new.txt" "$TMPDIR/E3/content.txt" >/dev/null 2>&1; then
	ok "-I creation script creates the file"
else
	fail "-I creation script creates the file"
fi

# every buffer of the session lands in one script: two more files reached
# with :e, one of them new, all diffed against disk in the order opened
mkdir -p "$TMPDIR/E5"
printf 'a1\na2\na3\n' > "$TMPDIR/E5/a.txt"
printf 'b1\nb2\nb3\n' > "$TMPDIR/E5/b.txt"
run_I "$TMPDIR/E5" \
	'%s/a2/A2/:e b.txt:%s/b3/B3/:e c.txt:r a.txt:q!' 'a.txt > out.sh'
if grep -q '^# Patch: a.txt b.txt c.txt$' "$TMPDIR/E5/out.sh" &&
   [ ! -e "$TMPDIR/E5/c.txt" ]; then
	ok "-I collects every session buffer"
else
	fail "-I collects every session buffer"
	grep '^# Patch:' "$TMPDIR/E5/out.sh" | sed 's/^/    /'
fi
( cd "$TMPDIR/E5" && VI="$VI" sh out.sh ) >/dev/null 2>&1
printf 'a1\nA2\na3\n' > "$TMPDIR/E5/want_a.txt"
printf 'b1\nb2\nB3\n' > "$TMPDIR/E5/want_b.txt"
# c.txt got a.txt read off disk, that is before the buffer's own edit
printf 'a1\na2\na3\n' > "$TMPDIR/E5/want_c.txt"
if diff -q "$TMPDIR/E5/a.txt" "$TMPDIR/E5/want_a.txt" >/dev/null 2>&1 &&
   diff -q "$TMPDIR/E5/b.txt" "$TMPDIR/E5/want_b.txt" >/dev/null 2>&1 &&
   diff -q "$TMPDIR/E5/c.txt" "$TMPDIR/E5/want_c.txt" >/dev/null 2>&1; then
	ok "-I multi-buffer script applies to every file"
else
	fail "-I multi-buffer script applies to every file"
fi

# buffers left untouched contribute nothing, changed ones still do
mkdir -p "$TMPDIR/E6"
printf 'x1\nx2\n' > "$TMPDIR/E6/x.txt"
printf 'y1\ny2\n' > "$TMPDIR/E6/y.txt"
run_I "$TMPDIR/E6" ':e y.txt:%s/y1/Y1/:q!' 'x.txt > out.sh'
if grep -q '^# Patch: y.txt$' "$TMPDIR/E6/out.sh"; then
	ok "-I skips buffers with no edits"
else
	fail "-I skips buffers with no edits"
	grep '^# Patch:' "$TMPDIR/E6/out.sh" | sed 's/^/    /'
fi

# everything after -I is nextvi's own command line: -e runs the session in
# ex mode, where EXINIT does the editing and there is no vi loop at all
mkdir -p "$TMPDIR/E7"
printf 'z1\nz2\n' > "$TMPDIR/E7/z.txt"
printf 'Z1\nz2\n' > "$TMPDIR/E7/want_z.txt"
run_I "$TMPDIR/E7" '%s/z1/Z1/:q!' '-e z.txt > out.sh'
( cd "$TMPDIR/E7" && VI="$VI" sh out.sh ) >/dev/null 2>&1
if diff -q "$TMPDIR/E7/z.txt" "$TMPDIR/E7/want_z.txt" >/dev/null 2>&1; then
	ok "-I passes nextvi flags straight through"
else
	fail "-I passes nextvi flags straight through"
fi

echo ""
echo "=== -E script-update tests ==="

# -E replays a generated script into one session, hands it over, and emits
# the updated script: the old script's own effect plus what the user did,
# diffed against the files on disk. The replay never writes, so the tree
# the new script is measured against is the untouched one.
A="$TMPDIR/Am"
mkdir -p "$A"

# run_A <workdir> <P2VI_EX> <patch2vi args...>; the handover is driven by
# P2VI_EX, since a replayed session is not nextvi's own main()
run_A() {
	local d="$1" ex="$2"
	shift 2
	P2VI_EX="$ex" script -qec \
		"sh -c 'cd $d && $E_P2VI $*'" /dev/null >/dev/null 2>&1
}

printf 'L1\nL2\nL3\n' > "$A/base.txt"
printf -- '--- a/f.txt\n+++ b/f.txt\n@@ -1,3 +1,3 @@\n L1\n L2\n-L3\n+L3x\n' \
	> "$A/x.diff"
cp "$A/base.txt" "$A/f.txt"
"$E_P2VI" -r "$A/x.diff" > "$A/old.sh"

# the user changes a second, disjoint line during the handover
run_A "$A" '%s/^L2$/L2c/:q!' '-E old.sh > new.sh 2> nerr'

if diff -q "$A/f.txt" "$A/base.txt" >/dev/null 2>&1; then
	ok "-E leaves the replayed files on disk alone"
else
	fail "-E leaves the replayed files on disk alone"
fi

printf 'L1\nL2c\nL3x\n' > "$A/want.txt"
cp "$A/base.txt" "$A/f.txt"
( cd "$A" && VI="$VI" sh new.sh ) >/dev/null 2>&1
if diff -q "$A/f.txt" "$A/want.txt" >/dev/null 2>&1; then
	ok "-E script carries the old effect plus the new edit"
else
	fail "-E script carries the old effect plus the new edit"
	tr -d '\r' < "$A/nerr" | sed 's/^/    /'
fi

# an untouched handover is a fixed point: the new script does what the old
# one did, no more
cp "$A/base.txt" "$A/f.txt"
run_A "$A" 'q!' '-E old.sh > same.sh 2> nerr2'
printf 'L1\nL2\nL3x\n' > "$A/want2.txt"
( cd "$A" && VI="$VI" sh same.sh ) >/dev/null 2>&1
if diff -q "$A/f.txt" "$A/want2.txt" >/dev/null 2>&1; then
	ok "-E without edits reproduces the script's own effect"
else
	fail "-E without edits reproduces the script's own effect"
	tr -d '\r' < "$A/nerr2" | sed 's/^/    /'
fi

# a file named after the script is opened on top of the replay's own buffers
# and joins the same output script
cp "$A/base.txt" "$A/f.txt"
printf 'g1\ng2\n' > "$A/g.txt"
cp "$A/g.txt" "$A/g.base"
run_A "$A" '%s/^g1$/G1/:q!' '-E old.sh g.txt > new2.sh 2> nerr3'
printf 'G1\ng2\n' > "$A/wantg.txt"
( cd "$A" && VI="$VI" sh new2.sh ) >/dev/null 2>&1
if diff -q "$A/f.txt" "$A/want2.txt" >/dev/null 2>&1 &&
   diff -q "$A/g.txt" "$A/wantg.txt" >/dev/null 2>&1; then
	ok "-E opens the files named after the script and emits them too"
else
	fail "-E opens the files named after the script and emits them too"
	tr -d '\r' < "$A/nerr3" | sed 's/^/    /'
fi

# the input must be a generated script, not a patch
if "$E_P2VI" -E "$A/x.diff" > /dev/null 2>&1; then
	fail "-E rejects an input that is not a patch2vi script"
else
	ok "-E rejects an input that is not a patch2vi script"
fi

# -o names the output file for any mode: here a plain diff conversion
"$E_P2VI" -r -o "$A/o1.sh" "$A/x.diff" > "$A/o1.out" 2>&1
if [ -s "$A/o1.sh" ] && [ ! -s "$A/o1.out" ] &&
   head -n1 "$A/o1.sh" | grep -q '^#!/bin/sh'; then
	ok "-o writes the script to the named file, not stdout"
else
	fail "-o writes the script to the named file, not stdout"
fi

# what -o writes is a script: a file it creates comes out executable, a file
# it replaces keeps the mode it had
rm -f "$A/o2.sh"
"$E_P2VI" -r -o "$A/o2.sh" "$A/x.diff"
printf 'placeholder\n' > "$A/o3.sh"
chmod 600 "$A/o3.sh"
"$E_P2VI" -r -o "$A/o3.sh" "$A/x.diff"
if [ -x "$A/o2.sh" ] && [ ! -x "$A/o3.sh" ]; then
	ok "-o makes a new script executable, keeps an existing file's mode"
else
	fail "-o makes a new script executable, keeps an existing file's mode"
fi

# -o may name the very script -E is updating: everything is read before
# anything is written, and the file is replaced atomically at the end
cp "$A/base.txt" "$A/f.txt"
cp "$A/old.sh" "$A/self.sh"
run_A "$A" '%s/^L2$/L2c/:q!' '-o self.sh -E self.sh 2> nerr4'
( cd "$A" && VI="$VI" sh self.sh ) >/dev/null 2>&1
if diff -q "$A/f.txt" "$A/want.txt" >/dev/null 2>&1; then
	ok "-E -o on its own script updates it in place"
else
	fail "-E -o on its own script updates it in place"
	tr -d '\r' < "$A/nerr4" | sed 's/^/    /'
fi

# a run that fails leaves the -o file alone and drops its temp
cp "$A/old.sh" "$A/keep.sh"
cp "$A/keep.sh" "$A/keep.want"
printf 'nothing like the script\n' > "$A/drift.txt"
"$E_P2VI" -o "$A/keep.sh" -E "$A/x.diff" >/dev/null 2>&1 || true
if diff -q "$A/keep.sh" "$A/keep.want" >/dev/null 2>&1 &&
   [ ! -e "$A/keep.sh.p2v.tmp" ]; then
	ok "-o leaves the target untouched when the run fails"
else
	fail "-o leaves the target untouched when the run fails"
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
echo "=== replay (-co) tests ==="

# The compat session replays a generated script in ONE editor: buffers
# persist across blocks, nothing is written, and the last block hands the
# session over. The handover is driven by P2VI_EX, which writes the
# session's buffers out so their in-RAM state can be checked - the disk
# copies of the replayed files must stay untouched.
if command -v script >/dev/null 2>&1; then

R="$TMPDIR/rp"
R_P2VI="$PWD/patch2vi"
mkdir -p "$R"

# <script> <P2VI_EX>: replay under a pty, ignoring the exit status (the
# derivation stage after the handover is not implemented yet)
run_pr() {
	P2VI_EX="$2" script -qec \
		"sh -c 'cd $R && $R_P2VI -co $1 /dev/null'" /dev/null \
		> "$R/log" 2>&1 || true
}

printf 'a\nb\nc\n' > "$R/f1.txt"
printf 'x\ny\nz\n' > "$R/f2.txt"
cp "$R/f1.txt" "$R/f1.orig"
cp "$R/f2.txt" "$R/f2.orig"

# Two blocks over the same files, listed in a DIFFERENT order by each:
# block 2 edits its own b1 (f1.txt), which is the session's buffer 0, and
# its edit only matches if it sees block 1's edit rather than the disk.
{
	printf '#!/bin/sh -e\nVI=${VI:-vi}\n'
	printf 'SEP="$(printf '"'"'\\001'"'"')"\n'
	printf 'ESC="$(printf '"'"'\\002'"'"')"\n'
	printf 'printf '"'"'%%s\\n'"'"' "|sc! ${ESC}${SEP}|:vis 3${SEP}b0${SEP}%%ya 98${SEP}1s/a/A/${SEP}vis 2${SEP}b0${SEP}w${SEP}b1${SEP}w${SEP}2q" > "$P2VIF"\n'
	printf "EXINIT='%%ya 97:? %%@97' \$VI -e 'f1.txt' 'f2.txt' \"\$P2VIF\"\n"
	printf 'printf '"'"'%%s\\n'"'"' "|sc! ${ESC}${SEP}|:vis 3${SEP}b1${SEP}1s/A/AA/${SEP}vis 2${SEP}b1${SEP}w${SEP}2q" > "$P2VIF"\n'
	printf "EXINIT='%%ya 97:? %%@97' \$VI -e 'f2.txt' 'f1.txt' \"\$P2VIF\"\n"
	printf 'exit 0\n'
} > "$R/two.sh"

rm -f "$R/out0" "$R/out1"
run_pr two.sh "b0:w! $R/out0:b1:w! $R/out1:q!"
if [ "$(cat "$R/out0" 2>/dev/null)" = "$(printf 'AA\nb\nc')" ]; then
	ok "replay: a block sees the previous block's edits (b<N> remapped)"
else
	fail "replay: a block sees the previous block's edits (b<N> remapped)"
	cat "$R/out0" 2>/dev/null | sed 's/^/    /'
fi
if diff -q "$R/f1.txt" "$R/f1.orig" >/dev/null 2>&1 &&
   diff -q "$R/f2.txt" "$R/f2.orig" >/dev/null 2>&1; then
	ok "replay writes nothing to disk"
else
	fail "replay writes nothing to disk"
fi

# A real generated script (multi-file, multi-line body) replays the same
# way: its edits land in the buffers, never in the files
printf 'a\nB\nc\n' > "$R/f1.want"
printf 'x\nY\nz\n' > "$R/f2.want"
{
	diff -u "$R/f1.orig" "$R/f1.want" | sed '1s|.*|--- a/f1.txt|;2s|.*|+++ b/f1.txt|'
	diff -u "$R/f2.orig" "$R/f2.want" | sed '1s|.*|--- a/f2.txt|;2s|.*|+++ b/f2.txt|'
} > "$R/d.diff" || true
"$R_P2VI" -r "$R/d.diff" > "$R/real.sh"
sed -i "s|\$VI -e '[^']*' '[^']*'|\$VI -e 'f1.txt' 'f2.txt'|" "$R/real.sh"
rm -f "$R/out0" "$R/out1"
run_pr real.sh "b0:w! $R/out0:b1:w! $R/out1:q!"
if diff -q "$R/out0" "$R/f1.want" >/dev/null 2>&1 &&
   diff -q "$R/out1" "$R/f2.want" >/dev/null 2>&1 &&
   diff -q "$R/f1.txt" "$R/f1.orig" >/dev/null 2>&1; then
	ok "replay applies a generated script in RAM only"
else
	fail "replay applies a generated script in RAM only"
fi

# A script that does not apply must abort the replay, not hand over a
# half-patched session: phase 1/2 failures are made fatal and loud
printf 'zzz\nqqq\n' > "$R/f1.txt"
run_pr real.sh 'q!'
cp "$R/f1.orig" "$R/f1.txt"
if tr -d '\r' < "$R/log" | grep -q "^replay: block 1 failed with status 1"; then
	ok "replay aborts when a replayed block fails"
else
	fail "replay aborts when a replayed block fails"
	tail -3 "$R/log" | sed 's/^/    /'
fi

# Compat derivation (stage B2). -co replays the origin and target, hands the tree to the
# user, and turns the user's merge into a gated compat block emitted after the
# target's own hunk. The gate's probe comes from where the origin actually
# landed (its inserted line), so on an origin tree the block fires and merges,
# while on a clean tree the gate self-skips before any edit and the target
# applies alone.
coderive() {	# <origin.sh> <target.sh> <P2VI_EX>: emit -co result to $R/new.sh
	rm -f "$R/new.sh"
	P2VI_EX="$3" script -qec \
		"sh -c 'cd $R && $R_P2VI -co $1 $2 > $R/new.sh 2>$R/nerr'" \
		/dev/null > /dev/null 2>&1 || true
}

# The compat block and the target's own hunk are independent units that stack:
# patch2vi reasons across neither. origin (x1) inserts PROBE, which gives the
# gate a unique landmark; the target (x2) makes its own change (L3 -> L3x); and
# in the handover the user edits a third, disjoint line (L2 -> L2c) - the compat
# edit. On an origin tree all three compound; on a clean tree only x2 runs.
printf 'L1\nL2\nL3\n' > "$R/draw.orig"
printf -- '--- a/draw.c\n+++ b/draw.c\n@@ -1,3 +1,4 @@\n L1\n+PROBE\n L2\n L3\n' > "$R/x1.diff"
printf -- '--- a/draw.c\n+++ b/draw.c\n@@ -1,3 +1,3 @@\n L1\n L2\n-L3\n+L3x\n' > "$R/x2.diff"
"$R_P2VI" -r "$R/x1.diff" > "$R/x1.sh"
"$R_P2VI" -r "$R/x2.diff" > "$R/x2.sh"
cp "$R/draw.orig" "$R/draw.c"	# pre-origin tree the replay reads
coderive x1.sh x2.sh '%s/^L2$/L2c/:q!'

if grep -q '^# Compat (post) from x1.sh' "$R/new.sh" 2>/dev/null; then
	ok "compat: -co emits a gated post-block from the user's merge"
else
	fail "compat: -co emits a gated post-block from the user's merge"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /'
fi

# origin tree: x1 lands PROBE, the compat block fires (L2 -> L2c) because PROBE
# is present, and x2 applies its own change (L3 -> L3x) - the units stack
cp "$R/draw.orig" "$R/draw.c"
( cd "$R" && VI="$VI" sh x1.sh && VI="$VI" sh new.sh ) >/dev/null 2>&1
if [ "$(cat "$R/draw.c")" = "$(printf 'L1\nPROBE\nL2c\nL3x')" ]; then
	ok "compat: fires on an origin tree, stacks with the target hunk"
else
	fail "compat: fires on an origin tree, stacks with the target hunk"
	sed 's/^/    /' "$R/draw.c"
fi

# clean tree: PROBE is absent, so the gate quits before any edit; only x2 runs
cp "$R/draw.orig" "$R/draw.c"
( cd "$R" && VI="$VI" sh new.sh ) >/dev/null 2>&1
if [ "$(cat "$R/draw.c")" = "$(printf 'L1\nL2\nL3x')" ]; then
	ok "compat: gate no-ops on a clean tree, target applies alone"
else
	fail "compat: gate no-ops on a clean tree, target applies alone"
	sed 's/^/    /' "$R/draw.c"
fi

# an origin whose inserted line is not unique gives no window that tells the
# two trees apart: a hard error, empty output, never a weak gate
printf 'dup\nb\ndup\n' > "$R/u.orig"
printf -- '--- a/u.c\n+++ b/u.c\n@@ -1,3 +1,4 @@\n dup\n b\n+dup\n dup\n' > "$R/u1.diff"
printf -- '--- a/u.c\n+++ b/u.c\n@@ -1,3 +1,3 @@\n dup\n-b\n+B\n dup\n' > "$R/u2.diff"
cp "$R/u.orig" "$R/u.c"
"$R_P2VI" -r "$R/u1.diff" > "$R/u1.sh"
"$R_P2VI" -r "$R/u2.diff" > "$R/u2.sh"
coderive u1.sh u2.sh '%s/^B$/B changed/:q!'	# the target left B, and patterns are case-sensitive
if [ ! -s "$R/new.sh" ] && tr -d '\r' < "$R/nerr" | grep -q 'no probe validates'; then
	ok "compat: an undiscriminating origin insertion is refused"
else
	fail "compat: an undiscriminating origin insertion is refused"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /'
fi

# -co replays the origin AND the target into one session before the handover,
# so the target must apply cleanly on the origin-modified tree. That exercises
# the block boundary: the origin leaves its cursor deep in the buffer, and a
# leftover row would steer the target's searches - so every buffer is rewound to
# the top before the next block. origin (x1) inserts PROBE (the gate landmark);
# target (x2) changes a disjoint line (L3 -> L3x); the compat block is emitted
# AFTER the target and gated on PROBE. In the handover the user edits L2 -> L2c.
printf 'L1\nL2\nL3\n' > "$R/po.orig"
printf -- '--- a/po.c\n+++ b/po.c\n@@ -1,3 +1,4 @@\n L1\n+PROBE\n L2\n L3\n' > "$R/p1.diff"
printf -- '--- a/po.c\n+++ b/po.c\n@@ -1,3 +1,3 @@\n L1\n L2\n-L3\n+L3x\n' > "$R/p2.diff"
"$R_P2VI" -r "$R/p1.diff" > "$R/p1.sh"
"$R_P2VI" -r "$R/p2.diff" > "$R/p2.sh"
cp "$R/po.orig" "$R/po.c"	# pre-origin tree the replay reads
coderive p1.sh p2.sh '%s/^L2$/L2c/:q!'
if grep -q '^# Compat (post) from p1.sh' "$R/new.sh" 2>/dev/null; then
	ok "compat: -co emits a gated post-block from the user's merge"
else
	fail "compat: -co emits a gated post-block from the user's merge"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /'
fi

# origin tree: PROBE present, so the target's own change lands (L3 -> L3x) and
# then the postfix compat block fires (L2 -> L2c) - the units stack
cp "$R/po.orig" "$R/po.c"
( cd "$R" && VI="$VI" sh p1.sh && VI="$VI" sh new.sh ) >/dev/null 2>&1
if [ "$(cat "$R/po.c")" = "$(printf 'L1\nPROBE\nL2c\nL3x')" ]; then
	ok "compat: -co fires on an origin tree, stacks with the target hunk"
else
	fail "compat: -co fires on an origin tree, stacks with the target hunk"
	sed 's/^/    /' "$R/po.c"
fi

# clean tree: PROBE absent, so the postfix gate quits before its edit; only the
# target's own change lands
cp "$R/po.orig" "$R/po.c"
( cd "$R" && VI="$VI" sh new.sh ) >/dev/null 2>&1
if [ "$(cat "$R/po.c")" = "$(printf 'L1\nL2\nL3x')" ]; then
	ok "compat: -co postfix gate no-ops on a clean tree"
else
	fail "compat: -co postfix gate no-ops on a clean tree"
	sed 's/^/    /' "$R/po.c"
fi

# A third -co argument is an already written compat patch, applied to the
# post-origin+target tree before the handover: the author does not retype a
# resolution they already have, and the editor still opens on top of it, so the
# derived block covers the pre-applied edits AND whatever the user adds.
coderive3() {	# <origin.sh> <target.sh> <compat> <P2VI_EX>: out $R/new.sh
	rm -f "$R/new.sh"
	P2VI_EX="$4" script -qec \
		"sh -c 'cd $R && $R_P2VI -co $1 $2 $3 > $R/new.sh 2>$R/nerr'" \
		/dev/null > /dev/null 2>&1 || true
}

# origin (b1) inserts PROBE, target (b2) changes A6 -> A6x, and the compat patch
# (written against the tree both leave behind) changes A3 -> A3c. The user quits
# without touching anything, so the derived block is exactly that patch.
printf 'A1\nA2\nA3\nA4\nA5\nA6\n' > "$R/pb.orig"
printf -- '--- a/pb.c\n+++ b/pb.c\n@@ -1,3 +1,4 @@\n A1\n+PROBE\n A2\n A3\n' > "$R/b1.diff"
printf -- '--- a/pb.c\n+++ b/pb.c\n@@ -4,3 +4,3 @@\n A4\n A5\n-A6\n+A6x\n' > "$R/b2.diff"
printf -- '--- a/pb.c\n+++ b/pb.c\n@@ -3,3 +3,3 @@\n A2\n-A3\n+A3c\n A4\n' > "$R/bc.diff"
"$R_P2VI" -r "$R/b1.diff" > "$R/b1.sh"
"$R_P2VI" -r "$R/b2.diff" > "$R/b2.sh"
cp "$R/pb.orig" "$R/pb.c"	# pre-origin tree the replay reads
coderive3 b1.sh b2.sh bc.diff ':q!'
if grep -q '^# Compat (post) from b1.sh' "$R/new.sh" 2>/dev/null; then
	ok "compat: -co derives a block from a pre-applied diff alone"
else
	fail "compat: -co derives a block from a pre-applied diff alone"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /'
fi

cp "$R/pb.orig" "$R/pb.c"
( cd "$R" && VI="$VI" sh b1.sh && VI="$VI" sh new.sh ) >/dev/null 2>&1
if [ "$(cat "$R/pb.c")" = "$(printf 'A1\nPROBE\nA2\nA3c\nA4\nA5\nA6x')" ]; then
	ok "compat: pre-applied diff fires gated on an origin tree"
else
	fail "compat: pre-applied diff fires gated on an origin tree"
	sed 's/^/    /' "$R/pb.c"
fi

cp "$R/pb.orig" "$R/pb.c"
( cd "$R" && VI="$VI" sh new.sh ) >/dev/null 2>&1
if [ "$(cat "$R/pb.c")" = "$(printf 'A1\nA2\nA3\nA4\nA5\nA6x')" ]; then
	ok "compat: pre-applied diff still self-skips on a clean tree"
else
	fail "compat: pre-applied diff still self-skips on a clean tree"
	sed 's/^/    /' "$R/pb.c"
fi

# the same fix in script form is replayed as one more block of the session, and
# the handover is not subverted: the user's own edit (A5 -> A5u) compounds with
# the pre-applied A3 -> A3c in a single derived block
"$R_P2VI" -r "$R/bc.diff" > "$R/bc.sh"
cp "$R/pb.orig" "$R/pb.c"
coderive3 b1.sh b2.sh bc.sh '%s/^A5$/A5u/:q!'
cp "$R/pb.orig" "$R/pb.c"
( cd "$R" && VI="$VI" sh b1.sh && VI="$VI" sh new.sh ) >/dev/null 2>&1
if [ "$(cat "$R/pb.c")" = "$(printf 'A1\nPROBE\nA2\nA3c\nA4\nA5u\nA6x')" ]; then
	ok "compat: pre-applied script plus the user's own edit in one block"
else
	fail "compat: pre-applied script plus the user's own edit in one block"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /'
	sed 's/^/    /' "$R/pb.c"
fi

# a compat patch whose pre-image is not on the tree is a hard error, not a
# silent skip: the author would ship a block they never wrote
printf -- '--- a/pb.c\n+++ b/pb.c\n@@ -3,3 +3,3 @@\n A2\n-NOPE\n+A3c\n A4\n' > "$R/bb.diff"
cp "$R/pb.orig" "$R/pb.c"
coderive3 b1.sh b2.sh bb.diff ':q!'
if [ ! -s "$R/new.sh" ] && tr -d '\r' < "$R/nerr" | grep -q 'does not apply'; then
	ok "compat: a pre-applied diff that does not apply is refused"
else
	fail "compat: a pre-applied diff that does not apply is refused"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /'
fi

# The origin's landing is what a gate probes, and the target can overwrite it:
# origin (c1) changes X3 -> X3o and target (c2) then replaces the whole X2..X4
# region, so the post-origin+target text carries no trace of the origin - it is
# byte-identical to a target-only tree. The probe must come from the
# post-origin, pre-target text, which is what a sensor reads at run time (every
# sensor runs before any body writes).
printf 'X1\nX2\nX3\nX4\nX5\nX6\n' > "$R/pc.orig"
printf -- '--- a/pc.c\n+++ b/pc.c\n@@ -2,3 +2,3 @@\n X2\n-X3\n+X3o\n X4\n' > "$R/c1.diff"
printf -- '--- a/pc.c\n+++ b/pc.c\n@@ -1,5 +1,5 @@\n X1\n-X2\n-X3\n-X4\n+T2\n+T3\n+T4\n X5\n' > "$R/c2.diff"
"$R_P2VI" -r "$R/c1.diff" > "$R/c1.sh"
"$R_P2VI" -r "$R/c2.diff" > "$R/c2.sh"
cp "$R/pc.orig" "$R/pc.c"
coderive c1.sh c2.sh '%s/^X6$/X6c/:q!'
if grep -q '^# Compat (post) from c1.sh' "$R/new.sh" 2>/dev/null; then
	ok "compat: a gate derives though the target overwrote the origin's line"
else
	fail "compat: a gate derives though the target overwrote the origin's line"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /'
fi

cp "$R/pc.orig" "$R/pc.c"
( cd "$R" && VI="$VI" sh c1.sh && VI="$VI" sh new.sh ) >/dev/null 2>&1
o1=$(cat "$R/pc.c")
cp "$R/pc.orig" "$R/pc.c"
( cd "$R" && VI="$VI" sh new.sh ) >/dev/null 2>&1
o2=$(cat "$R/pc.c")
if [ "$o1" = "$(printf 'X1\nT2\nT3\nT4\nX5\nX6c')" ] &&
   [ "$o2" = "$(printf 'X1\nT2\nT3\nT4\nX5\nX6')" ]; then
	ok "compat: overwritten-trace gate fires on origin, no-ops on clean"
else
	fail "compat: overwritten-trace gate fires on origin, no-ops on clean"
	printf '%s\n--\n%s\n' "$o1" "$o2" | sed 's/^/    /'
fi

# Cross-file gate: the block edits a file the origin never touched, so no probe
# exists there. "Did this origin land" is a property of the tree, not of one
# file, so the probe comes from a file the origin did change and the gate
# records that path.
printf 'Y1\nY2\nY3\n' > "$R/pd.orig"
printf 'Z1\nZ2\nZ3\n' > "$R/pe.orig"
printf -- '--- a/pd.c\n+++ b/pd.c\n@@ -1,3 +1,4 @@\n Y1\n+YPROBE\n Y2\n Y3\n' > "$R/d1.diff"
printf -- '--- a/pe.c\n+++ b/pe.c\n@@ -1,3 +1,3 @@\n Z1\n-Z2\n+Z2t\n Z3\n' > "$R/d2.diff"
"$R_P2VI" -r "$R/d1.diff" > "$R/d1.sh"
"$R_P2VI" -r "$R/d2.diff" > "$R/d2.sh"
cp "$R/pd.orig" "$R/pd.c"
cp "$R/pe.orig" "$R/pe.c"
coderive d1.sh d2.sh '%s/^Z3$/Z3c/:q!'
if grep -q '^=== GATE 1 present tag [0-9]* probe pd.c ===' "$R/new.sh" 2>/dev/null; then
	ok "compat: a block over an untouched file probes another file"
else
	fail "compat: a block over an untouched file probes another file"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /'
	grep -n '=== GATE' "$R/new.sh" | sed 's/^/    /'
fi

cp "$R/pd.orig" "$R/pd.c"
cp "$R/pe.orig" "$R/pe.c"
( cd "$R" && VI="$VI" sh d1.sh && VI="$VI" sh new.sh ) >/dev/null 2>&1
o1=$(cat "$R/pe.c")
cp "$R/pd.orig" "$R/pd.c"
cp "$R/pe.orig" "$R/pe.c"
( cd "$R" && VI="$VI" sh new.sh ) >/dev/null 2>&1
o2=$(cat "$R/pe.c")
if [ "$o1" = "$(printf 'Z1\nZ2t\nZ3c')" ] &&
   [ "$o2" = "$(printf 'Z1\nZ2t\nZ3')" ]; then
	ok "compat: cross-file gate fires on origin, no-ops on clean"
	cp "$R/new.sh" "$R/xf.sh"
else
	fail "compat: cross-file gate fires on origin, no-ops on clean"
	printf '%s\n--\n%s\n' "$o1" "$o2" | sed 's/^/    /'
fi

# A compat patch is one unified diff, however many files it spans: one
# invocation reshaping two buffers must yield ONE block - one metadata region,
# one gate, one staged body - whose === COMPAT PATCH === carries both files.
printf 'H1\nH2\nH3\n' > "$R/ph.orig"
printf 'I1\nI2\nI3\n' > "$R/pi.orig"
printf -- '--- a/ph.c\n+++ b/ph.c\n@@ -1,3 +1,4 @@\n H1\n+HPROBE\n H2\n H3\n' > "$R/g1.diff"
printf -- '--- a/ph.c\n+++ b/ph.c\n@@ -1,3 +1,3 @@\n H1\n-H2\n+H2t\n H3\n--- a/pi.c\n+++ b/pi.c\n@@ -1,3 +1,3 @@\n I1\n-I2\n+I2t\n I3\n' > "$R/g2.diff"
"$R_P2VI" -r "$R/g1.diff" > "$R/g1.sh"
"$R_P2VI" -r "$R/g2.diff" > "$R/g2.sh"
cp "$R/ph.orig" "$R/ph.c"
cp "$R/pi.orig" "$R/pi.c"
coderive g1.sh g2.sh ':e ph.c:%s/^H3$/H3c/:e pi.c:%s/^I3$/I3c/:q!'
nreg="$(grep -c '^=== PATCH2VI COMPAT post src=' "$R/new.sh" 2>/dev/null || true)"
ngate="$(grep -c '^=== GATE ' "$R/new.sh" 2>/dev/null || true)"
nbody="$(grep -c '^# Compat (post) from g1.sh' "$R/new.sh" 2>/dev/null || true)"
nfile="$(sed -n '/^=== COMPAT PATCH ===$/,/^=== END /p' "$R/new.sh" |
	 grep -c '^--- ' || true)"
if [ "$nreg" = 1 ] && [ "$ngate" = 1 ] && [ "$nbody" = 1 ] && [ "$nfile" = 2 ]; then
	ok "compat: a two-file compat patch is one block, one gate, one diff"
else
	fail "compat: a two-file compat patch is one block, one gate, one diff"
	echo "    regions=$nreg gates=$ngate bodies=$nbody files=$nfile"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /' | head -3
fi

cp "$R/ph.orig" "$R/ph.c"
cp "$R/pi.orig" "$R/pi.c"
( cd "$R" && VI="$VI" sh g1.sh && VI="$VI" sh new.sh ) >/dev/null 2>&1
o1="$(cat "$R/ph.c")|$(cat "$R/pi.c")"
cp "$R/ph.orig" "$R/ph.c"
cp "$R/pi.orig" "$R/pi.c"
( cd "$R" && VI="$VI" sh new.sh ) >/dev/null 2>&1
o2="$(cat "$R/ph.c")|$(cat "$R/pi.c")"
if [ "$o1" = "$(printf 'H1\nHPROBE\nH2t\nH3c|I1\nI2t\nI3c')" ] &&
   [ "$o2" = "$(printf 'H1\nH2t\nH3|I1\nI2t\nI3')" ]; then
	ok "compat: one block edits every file it spans, gated once"
	cp "$R/new.sh" "$R/mf.sh"
else
	fail "compat: one block edits every file it spans, gated once"
	printf '%s\n--\n%s\n' "$o1" "$o2" | sed 's/^/    /'
fi

coderiveq() {	# coderive with QF2=1: the target is expected to miss
	rm -f "$R/new.sh"
	P2VI_EX="$3" script -qec \
		"sh -c 'cd $R && QF2=1 $R_P2VI -co $1 $2 > $R/new.sh 2>$R/nerr'" \
		/dev/null > /dev/null 2>&1 || true
}

# A host group the origin made impossible must not take the rest of the body
# with it. On a tree where some origin is present the host runs best-effort
# through register 211, which reads its flag with a register search - and ex's
# register redirection is global (xfr), so leaving it set sends every later
# multi-line search to the flag instead of the file cache. Origin (e1) deletes
# W2, target (e2) edits W2 in pf.c (a guaranteed miss, so the relaxed chain
# runs) and inserts into pg.c behind a duplicated anchor, whose group searches
# the cache and must still find it.
printf 'W1\nW2\nW3\nW4\n' > "$R/pf.orig"
printf 'V1\nDUP\nV2\nDUP\nV3\n' > "$R/pg.orig"
printf -- '--- a/pf.c\n+++ b/pf.c\n@@ -1,3 +1,2 @@\n W1\n-W2\n W3\n' > "$R/e1.diff"
printf -- '--- a/pf.c\n+++ b/pf.c\n@@ -1,3 +1,3 @@\n W1\n-W2\n+W2t\n W3\n--- a/pg.c\n+++ b/pg.c\n@@ -3,3 +3,4 @@\n V2\n DUP\n+NEW\n V3\n' > "$R/e2.diff"
"$R_P2VI" -r "$R/e1.diff" > "$R/e1.sh"
"$R_P2VI" -r "$R/e2.diff" > "$R/e2.sh"
cp "$R/pf.orig" "$R/pf.c"
cp "$R/pg.orig" "$R/pg.c"
coderiveq e1.sh e2.sh ':e pg.c:%s/^V3$/V3c/:q!'
cp "$R/pf.orig" "$R/pf.c"
cp "$R/pg.orig" "$R/pg.c"
( cd "$R" && VI="$VI" sh e1.sh && VI="$VI" QF2=1 sh new.sh ) >/dev/null 2>&1
if [ "$(cat "$R/pf.c")" = "$(printf 'W1\nW3\nW4')" ] &&
   [ "$(cat "$R/pg.c")" = "$(printf 'V1\nDUP\nV2\nDUP\nNEW\nV3c')" ]; then
	ok "compat: a host miss does not derail the groups after it"
else
	fail "compat: a host miss does not derail the groups after it"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /'
	sed 's/^/    /' "$R/pf.c" "$R/pg.c"
fi

# Stage B3: storage, round-trip and stacking. The -co script above (new.sh from
# draw.c) carries a pre COMPAT region after exit 0. -d must reproduce the whole
# script - host block and compat region - byte-identically, without re-running
# the origin, driven under a pty and quit with :q.
dregen() {	# <script>: regenerate to $R/dregen.sh via a no-op -d session
	rm -f "$R/dregen.sh"
	P2VI_EX=':q' script -qec \
		"sh -c 'cd $R && $R_P2VI -d $1 > $R/dregen.sh 2>$R/derr'" \
		/dev/null > /dev/null 2>&1 || true
}
dedit() {	# <script> <P2VI_EX>: -d session running <P2VI_EX>, out $R/dedit.sh
	rm -f "$R/dedit.sh"
	P2VI_EX="$2" script -qec \
		"sh -c 'cd $R && $R_P2VI -d $1 > $R/dedit.sh 2>$R/derr'" \
		/dev/null > /dev/null 2>&1 || true
}
# a cross-file gate survives storage: -d reparses the recorded probe path and
# emits the same script, so the sensor keeps searching the origin's file
cp "$R/pd.orig" "$R/pd.c"
cp "$R/pe.orig" "$R/pe.c"
dregen xf.sh
if [ -s "$R/dregen.sh" ] && cmp -s "$R/xf.sh" "$R/dregen.sh"; then
	ok "compat: -d round-trips a cross-file gate byte-identically"
else
	fail "compat: -d round-trips a cross-file gate byte-identically"
	tr -d '\r' < "$R/derr" | sed 's/^/    /'
	diff "$R/xf.sh" "$R/dregen.sh" | head -10 | sed 's/^/    /'
fi

# The built-in differ must not overextend. A span wider than the LCS table budget
# (DIFF_MAX_CELLS) used to degrade to delete-all/insert-all, so two edits far
# apart in a big file came out as a diff rewriting everything between them.
# Patience anchoring splits such a span on its unique common lines instead, so
# the compat patch stays the handful of lines that really changed.
awk 'BEGIN{for (i = 1; i <= 2200; i++) printf "L%04d\n", i}' > "$R/big.orig"
printf -- '--- a/big.c\n+++ b/big.c\n@@ -1,3 +1,4 @@\n L0001\n+BIGPROBE\n L0002\n L0003\n' > "$R/k1.diff"
printf -- '--- a/big.c\n+++ b/big.c\n@@ -1099,3 +1099,3 @@\n L1099\n-L1100\n+L1100t\n L1101\n' > "$R/k2.diff"
"$R_P2VI" -r "$R/k1.diff" > "$R/k1.sh"
"$R_P2VI" -r "$R/k2.diff" > "$R/k2.sh"
cp "$R/big.orig" "$R/big.c"
coderive k1.sh k2.sh '%s/^L0005$/L0005c/:%s/^L2100$/L2100c/:q!'
chg="$(sed -n '/^=== COMPAT PATCH ===$/,/^=== END /p' "$R/new.sh" |
       grep -c '^[-+][^-+]' || true)"
if [ "$chg" -ge 4 ] && [ "$chg" -le 12 ]; then
	ok "compat: the differ splits a huge span instead of rewriting it"
else
	fail "compat: the differ splits a huge span instead of rewriting it"
	echo "    changed lines in COMPAT PATCH=$chg (expected 4)"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /' | head -3
fi

cp "$R/big.orig" "$R/big.c"
( cd "$R" && VI="$VI" sh k1.sh && VI="$VI" sh new.sh ) >/dev/null 2>&1
if [ "$(sed -n '6p;1101p;2101p' "$R/big.c")" = "$(printf 'L0005c\nL1100t\nL2100c')" ]; then
	ok "compat: a split-span compat patch applies on an origin tree"
else
	fail "compat: a split-span compat patch applies on an origin tree"
	sed -n '6p;1101p;2101p' "$R/big.c" | sed 's/^/    /'
fi

# Patterns are case-sensitive: nextvi's ignorecase defaults ON, so a one-byte
# substitution like s/x/w/ meant for "xrows" would land on the "X" of "MAX("
# earlier in the line. The emitted prologue turns it off ("ic 0"); without that
# this tree comes out with "MAw(" and an untouched "xrows".
printf 'A1\n\tfor (k = MAX(0, -t); k < xrows; k++)\nA3\n' > "$R/cs.orig"
printf -- '--- a/cs.c\n+++ b/cs.c\n@@ -1,3 +1,4 @@\n A1\n+CSPROBE\n \tfor (k = MAX(0, -t); k < xrows; k++)\n A3\n' > "$R/n1.diff"
printf -- '--- a/cs.c\n+++ b/cs.c\n@@ -1,3 +1,3 @@\n A1\n \tfor (k = MAX(0, -t); k < xrows; k++)\n-A3\n+A3t\n' > "$R/n2.diff"
"$R_P2VI" -r "$R/n1.diff" > "$R/n1.sh"
"$R_P2VI" -r "$R/n2.diff" > "$R/n2.sh"
cp "$R/cs.orig" "$R/cs.c"
coderive n1.sh n2.sh '%s/xrows/wrows/:q!'
cp "$R/cs.orig" "$R/cs.c"
( cd "$R" && VI="$VI" sh n1.sh && VI="$VI" sh new.sh ) >/dev/null 2>&1
if [ "$(sed -n '3p' "$R/cs.c")" = "$(printf '\tfor (k = MAX(0, -t); k < wrows; k++)')" ]; then
	ok "compat: a one-byte substitution is matched case-sensitively"
else
	fail "compat: a one-byte substitution is matched case-sensitively"
	sed 's/^/    /' "$R/cs.c"
fi

# -d must round-trip a multi-file block: one region back out, byte-identically.
cp "$R/ph.orig" "$R/ph.c"
cp "$R/pi.orig" "$R/pi.c"
dregen mf.sh
if cmp -s "$R/mf.sh" "$R/dregen.sh"; then
	ok "compat: -d round-trips a two-file compat block byte-identically"
else
	fail "compat: -d round-trips a two-file compat block byte-identically"
	diff "$R/mf.sh" "$R/dregen.sh" 2>&1 | sed 's/^/    /' | head
fi

# rebuild the -co script (the earlier -co run clobbered new.sh)
printf 'L1\nL2\nL3\n' > "$R/draw.orig"
printf -- '--- a/draw.c\n+++ b/draw.c\n@@ -1,3 +1,4 @@\n L1\n+PROBE\n L2\n L3\n' > "$R/x1.diff"
printf -- '--- a/draw.c\n+++ b/draw.c\n@@ -1,3 +1,3 @@\n L1\n L2\n-L3\n+L3x\n' > "$R/x2.diff"
"$R_P2VI" -r "$R/x1.diff" > "$R/x1.sh"
"$R_P2VI" -r "$R/x2.diff" > "$R/x2.sh"
cp "$R/draw.orig" "$R/draw.c"
coderive x1.sh x2.sh '%s/^L2$/L2c/:q!'
cp "$R/new.sh" "$R/pr.sh"

cp "$R/draw.orig" "$R/draw.c"
dregen pr.sh
if diff "$R/pr.sh" "$R/dregen.sh" >/dev/null 2>&1; then
	ok "compat: -d round-trips a stored compat block byte-identically"
else
	fail "compat: -d round-trips a stored compat block byte-identically"
	diff "$R/pr.sh" "$R/dregen.sh" | sed 's/^/    /' | head -20
fi

# Re-running -co on a script that already carries a post block appends a NEW
# block at the group tail (existing block untouched); the user reshapes a
# different line. The origin+target replay applies the host (L3 -> L3x) and the
# stored post block (L2 -> L2c), so the new edit targets the post-replay line L3x.
cp "$R/draw.orig" "$R/draw.c"
coderive x1.sh pr.sh '%s/^L3x$/L3z/:q!'
if [ "$(grep -c '^=== PATCH2VI COMPAT post src=' "$R/new.sh")" = 2 ] &&
   grep -q '^+L2c$' "$R/new.sh" && grep -q '^+L3z$' "$R/new.sh"; then
	ok "compat: re-running -co stacks a new block, keeps the existing one"
else
	fail "compat: re-running -co stacks a new block, keeps the existing one"
	grep -n 'PATCH2VI COMPAT' "$R/new.sh" | sed 's/^/    /'
fi

# The stacked script fires both compat units plus the host on an origin tree,
# and only the host on a clean tree (both gates quit before any edit).
cp "$R/new.sh" "$R/stack.sh"
cp "$R/draw.orig" "$R/draw.c"
( cd "$R" && VI="$VI" sh x1.sh && VI="$VI" sh stack.sh ) >/dev/null 2>&1
origin_ok=0
[ "$(sed -n 1,3p "$R/draw.c")" = "$(printf 'L1\nPROBE\nL2c')" ] && origin_ok=1
cp "$R/draw.orig" "$R/draw.c"
( cd "$R" && VI="$VI" sh stack.sh ) >/dev/null 2>&1
if [ "$origin_ok" = 1 ] && [ "$(cat "$R/draw.c")" = "$(printf 'L1\nL2\nL3x')" ]; then
	ok "compat: stacked script fires on origin, gates no-op on clean"
else
	fail "compat: stacked script fires on origin, gates no-op on clean"
	sed 's/^/    /' "$R/draw.c"
fi

# Post stacking: re-running -co on a target that already carries a post block
# derives the NEW block on top of that block's output. The origin+target replay
# applies the stored post block (L2 -> L2c), so L2c exists before handover; the
# new session edits L2c -> L2cc. Without replaying the target's own blocks the
# buffer would still read L2, the edit would match nothing, and -co would fail
# with "no compat derived" / empty output.
cp "$R/draw.orig" "$R/draw.c"
coderive x1.sh pr.sh '%s/^L2c$/L2cc/:q!'
if [ -s "$R/new.sh" ] &&
   [ "$(grep -c '^=== PATCH2VI COMPAT post src=' "$R/new.sh")" = 2 ] &&
   grep -q '^-L2c$' "$R/new.sh" && grep -q '^+L2cc$' "$R/new.sh"; then
	ok "compat: -co stacks the new block atop the existing post block"
else
	fail "compat: -co stacks the new block atop the existing post block"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /'
	grep -n 'PATCH2VI COMPAT\|^[-+]L2' "$R/new.sh" 2>/dev/null | sed 's/^/    /'
fi

# The doubly-stacked script applies all three units in order on an origin tree:
# the host (L3 -> L3x), post block 1 (L2 -> L2c), post block 2 (L2c -> L2cc).
cp "$R/new.sh" "$R/stack2.sh"
cp "$R/draw.orig" "$R/draw.c"
( cd "$R" && VI="$VI" sh x1.sh && VI="$VI" sh stack2.sh ) >/dev/null 2>&1
if [ "$(cat "$R/draw.c")" = "$(printf 'L1\nPROBE\nL2cc\nL3x')" ]; then
	ok "compat: post stack applies all blocks in order on an origin tree"
else
	fail "compat: post stack applies all blocks in order on an origin tree"
	sed 's/^/    /' "$R/draw.c"
fi

# Part A: a stored === COMPAT DELTA === shapes the emitted compat body and
# survives -d regen (no editor UI needed). Rebuild a single-block pr.sh, then
# hand-edit its empty COMPAT DELTA to flip group 1's strategy to abs. -d must
# re-emit the compat body from that delta - an absolute "3c L2c" replacing the
# relative "f> ^L2$" search anchor - and re-deriving it stays byte-identical.
cp "$R/draw.orig" "$R/draw.c"
coderive x1.sh x2.sh '%s/^L2$/L2c/:q!'
cp "$R/new.sh" "$R/pr.sh"
awk '
/^=== COMPAT DELTA ===$/ {
	print; getline;			# drop the empty "=== END ==="
	print "=== DELTA draw.c ===";
	print "=== GROUP 1 ===";
	print "-L2"; print "+L2c";
	print "=== END ===";
	print "=== LEVEL 2 ===";
	print "=== strategy ===";
	print "abs";
	print "=== END ===";
	print "=== END ===";
	print "=== END ===";
	next
}
{ print }
' "$R/pr.sh" > "$R/edited.sh"

cp "$R/draw.orig" "$R/draw.c"
dregen edited.sh
cp "$R/dregen.sh" "$R/edd.sh"
if grep -q '3c L2c' "$R/edd.sh" &&
   ! sed -n '/# Compat (post)/,/EXINIT/p' "$R/edd.sh" | grep -q 'f> \^L2\$'; then
	ok "compat: a stored COMPAT DELTA reshapes the emitted body (-d, no UI)"
else
	fail "compat: a stored COMPAT DELTA reshapes the emitted body (-d, no UI)"
	sed -n '/# Compat (post)/,/EXINIT/p' "$R/edd.sh" | sed 's/^/    /' | head
fi

# the delta-shaped block still applies on an origin tree (abs line numbers
# line up post-origin) and no-ops on a clean tree; -d of it is stable
cp "$R/draw.orig" "$R/draw.c"
( cd "$R" && VI="$VI" sh x1.sh && VI="$VI" sh edd.sh ) >/dev/null 2>&1
deltaA_ok=0
[ "$(sed -n 1,3p "$R/draw.c")" = "$(printf 'L1\nPROBE\nL2c')" ] && deltaA_ok=1
cp "$R/draw.orig" "$R/draw.c"
dregen edd.sh
if [ "$deltaA_ok" = 1 ] && diff "$R/edd.sh" "$R/dregen.sh" >/dev/null 2>&1; then
	ok "compat: delta-shaped block applies on origin and -d is stable"
else
	fail "compat: delta-shaped block applies on origin and -d is stable"
	[ "$deltaA_ok" = 1 ] || { echo "    apply:"; sed 's/^/    /' "$R/draw.c"; }
	diff "$R/edd.sh" "$R/dregen.sh" | sed 's/^/    /' | head
fi

# Part B: the in-editor surface. Under -d each compat block opens as its own
# editable buffer (host = b0, compat block c = b<c+1>); edits read back into
# that block's === COMPAT DELTA === and, via Part A, into its emitted body,
# with the host buffer untouched. Edit compat buffer b1's COMMAND STRATEGY to
# select abs (uncomment #abs); the effect must match Test A's hand-edit.
cp "$R/draw.orig" "$R/draw.c"
coderive x1.sh x2.sh '%s/^L2$/L2c/:q!'
cp "$R/new.sh" "$R/pr.sh"
cp "$R/draw.orig" "$R/draw.c"
dedit pr.sh 'b1:%s/^#abs$/abs/:q!'
host_pr="$(sed -n '/# Patch:/,/\$P2VIF/p' "$R/pr.sh")"
host_ed="$(sed -n '/# Patch:/,/\$P2VIF/p' "$R/dedit.sh")"
if sed -n '/# Compat (post)/,/EXINIT/p' "$R/dedit.sh" | grep -q '3c L2c' &&
   sed -n '/=== COMPAT DELTA/,/=== COMPAT PATCH/p' "$R/dedit.sh" |
	grep -q '^abs$' &&
   [ "$host_pr" = "$host_ed" ]; then
	ok "compat: editing a block's buffer under -d shapes its body + storage"
else
	fail "compat: editing a block's buffer under -d shapes its body + storage"
	grep -v snapshot "$R/derr" | sed 's/^/    /' | head
fi

# the edited block still applies on an origin tree and -d is stable
cp "$R/dedit.sh" "$R/bedd.sh"
cp "$R/draw.orig" "$R/draw.c"
( cd "$R" && VI="$VI" sh x1.sh && VI="$VI" sh bedd.sh ) >/dev/null 2>&1
bB_ok=0
[ "$(sed -n 1,3p "$R/draw.c")" = "$(printf 'L1\nPROBE\nL2c')" ] && bB_ok=1
cp "$R/draw.orig" "$R/draw.c"
dregen bedd.sh
if [ "$bB_ok" = 1 ] && diff "$R/bedd.sh" "$R/dregen.sh" >/dev/null 2>&1; then
	ok "compat: buffer-edited block applies on origin and -d is stable"
else
	fail "compat: buffer-edited block applies on origin and -d is stable"
	[ "$bB_ok" = 1 ] || { echo "    apply:"; sed 's/^/    /' "$R/draw.c"; }
fi

# A session touching no buffer reproduces a multi-block script byte-identically
# (per-buffer read-back across host + N compat buffers). Build a two-block
# script first, then a no-op -d must round-trip it.
cp "$R/draw.orig" "$R/draw.c"
coderive x1.sh pr.sh '%s/^L3x$/L3z/:q!'	# stacks a 2nd pre block
cp "$R/new.sh" "$R/two.sh"
if [ "$(grep -c '^=== PATCH2VI COMPAT post' "$R/two.sh")" = 2 ]; then
	cp "$R/draw.orig" "$R/draw.c"
	dregen two.sh
	if diff "$R/two.sh" "$R/dregen.sh" >/dev/null 2>&1; then
		ok "compat: -d untouched reproduces a two-block script byte-identically"
	else
		fail "compat: -d untouched reproduces a two-block script byte-identically"
		diff "$R/two.sh" "$R/dregen.sh" | sed 's/^/    /' | head
	fi
else
	fail "compat: -d untouched reproduces a two-block script byte-identically"
	echo "    could not build a two-block script"
fi

# Two blocks over one file open two DISTINCT buffers (indexed names): editing
# only b1 routes to the first block; the second block is left untouched.
cp "$R/draw.orig" "$R/draw.c"
dedit two.sh 'b1:%s/^#abs$/abs/:q!'
blk1="$(awk '/=== PATCH2VI COMPAT/{n++} n==1 && /^abs$/{print "hit"}' "$R/dedit.sh")"
blk2="$(awk '/=== PATCH2VI COMPAT/{n++} n==2 && /^abs$/{print "hit"}' "$R/dedit.sh")"
if [ "$blk1" = "hit" ] && [ -z "$blk2" ]; then
	ok "compat: editing b1 routes to block 1 only, block 2 untouched"
else
	fail "compat: editing b1 routes to block 1 only, block 2 untouched"
	echo "    blk1=[$blk1] blk2=[$blk2]"
	grep -v snapshot "$R/derr" | sed 's/^/    /' | head
fi

# Stacked blocks must not share a gate anchor tag. xanchor is global and a
# lookup ORs every entry recorded under the id, so one shared tag would make
# either sensor answer for both blocks. Tag numbering continues across blocks
# (next_gate_tag), so the two stored gates carry different ids.
tags="$(sed -n 's/^=== GATE .* tag \([0-9]*\).*$/\1/p' "$R/two.sh" | sort)"
if [ "$(echo "$tags" | wc -l)" = 2 ] &&
   [ "$(echo "$tags" | sort -u | wc -l)" = 2 ]; then
	ok "compat: stacked blocks get distinct gate tags"
else
	fail "compat: stacked blocks get distinct gate tags"
	echo "    tags=[$(echo "$tags" | tr '\n' ' ')]"
fi

# Two blocks of one origin answer the same question, so derivation gives them
# identical gates; a hand-edit that widens only one makes them fire on different
# trees while the runtime subset test still reads a single per-origin flag. -d
# reports that, without refusing the script (a deliberate widening is the
# author's call) and without leaking the warning into the output.
awk 'BEGIN{n=0} /^=== PATCH2VI COMPAT/{n++} {if (n==2 && $0=="PROBE") print "PROBE2"; else print}' \
	"$R/two.sh" > "$R/badgate.sh"
cp "$R/draw.orig" "$R/draw.c"
dregen badgate.sh
warn="$(tr -d '\r' < "$R/derr" | grep -c 'carry different gates' || true)"
cp "$R/draw.orig" "$R/draw.c"
dregen two.sh
quiet="$(tr -d '\r' < "$R/derr" | grep -c 'carry different gates' || true)"
if [ "$warn" = 1 ] && [ "$quiet" = 0 ] && [ -s "$R/dregen.sh" ]; then
	ok "compat: -d reports blocks of one origin whose gates disagree"
else
	fail "compat: -d reports blocks of one origin whose gates disagree"
	echo "    warn=$warn quiet=$quiet"
fi

# Every invocation derives its own gate: no block inherits a stored one, not even
# from a block of the same origin. Widen the single-block script's gate by hand to
# a line both trees carry, stack a second block on it, and the new block must
# carry the freshly derived probe, leaving the hand-widened one untouched. (A
# dedup pass over a stack is the author's job - concatenate the regions' patches
# and run them through patch2vi again.)
sed 's/^PROBE$/L1/' "$R/pr.sh" > "$R/wide.sh"
cp "$R/draw.orig" "$R/draw.c"
coderive x1.sh wide.sh '%s/^L3x$/L3w/:q!'
probes="$(sed -n '/^=== GATE /{n;p;}' "$R/new.sh" | tr '\n' ' ')"
if [ "$probes" = "L1 PROBE " ]; then
	ok "compat: each invocation derives its own gate, inheriting none"
else
	fail "compat: each invocation derives its own gate, inheriting none"
	echo "    probes=[$probes]"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /' | head -3
fi

# Mixed origins, the subset matrix. Two independent origins over one file: A
# inserts PA at the top, B inserts PB at the bottom; the target's own hunk is a
# third, disjoint line. A's compat block is derived first, B's on top of it, so
# a single script carries two blocks with two different origins. Each of the
# four trees (none / A / B / A+B) must run exactly the blocks whose origin is
# present - the case a shared gate tag gets wrong, since an absent origin's
# block then fires off the other's sensor.
printf 'L1\nL2\nL3\nL4\n' > "$R/m.orig"
printf -- '--- a/m.c\n+++ b/m.c\n@@ -1,4 +1,5 @@\n L1\n+PA\n L2\n L3\n L4\n' > "$R/a1.diff"
printf -- '--- a/m.c\n+++ b/m.c\n@@ -1,4 +1,5 @@\n L1\n L2\n L3\n L4\n+PB\n' > "$R/b1.diff"
printf -- '--- a/m.c\n+++ b/m.c\n@@ -1,4 +1,4 @@\n L1\n L2\n-L3\n+L3x\n L4\n' > "$R/mx.diff"
"$R_P2VI" -r "$R/a1.diff" > "$R/a1.sh"
"$R_P2VI" -r "$R/b1.diff" > "$R/b1.sh"
"$R_P2VI" -r "$R/mx.diff" > "$R/mx.sh"
cp "$R/m.orig" "$R/m.c"
coderive a1.sh mx.sh '%s/^L2$/L2a/:q!'		# block from origin A
cp "$R/new.sh" "$R/mix1.sh"
cp "$R/m.orig" "$R/m.c"
coderive b1.sh mix1.sh '%s/^L4$/L4b/:q!'	# block from origin B, stacked
cp "$R/new.sh" "$R/mix2.sh"
mixrun() {	# <origin scripts> -> tree of m.c after mix2.sh, '|'-joined
	cp "$R/m.orig" "$R/m.c"
	for s in $1; do ( cd "$R" && VI="$VI" sh "$s" ) >/dev/null 2>&1; done
	( cd "$R" && VI="$VI" sh mix2.sh ) >/dev/null 2>&1
	tr '\n' '|' < "$R/m.c"
}
if [ "$(grep -c '^=== PATCH2VI COMPAT post src=' "$R/mix2.sh" 2>/dev/null)" = 2 ] &&
   grep -q '^# Compat (post) from a1.sh' "$R/mix2.sh" &&
   grep -q '^# Compat (post) from b1.sh' "$R/mix2.sh"; then
	ok "compat: two origins stack into one script"
else
	fail "compat: two origins stack into one script"
	tr -d '\r' < "$R/nerr" | sed 's/^/    /'
fi
t_none="$(mixrun '')"
t_a="$(mixrun 'a1.sh')"
t_b="$(mixrun 'b1.sh')"
t_ab="$(mixrun 'a1.sh b1.sh')"
if [ "$t_none" = 'L1|L2|L3x|L4|' ] && [ "$t_a" = 'L1|PA|L2a|L3x|L4|' ] &&
   [ "$t_b" = 'L1|L2|L3x|L4b|PB|' ] && [ "$t_ab" = 'L1|PA|L2a|L3x|L4b|PB|' ]; then
	ok "compat: mixed origins fire per subset (none/A/B/A+B)"
else
	fail "compat: mixed origins fire per subset (none/A/B/A+B)"
	echo "    none=[$t_none] A=[$t_a] B=[$t_b] A+B=[$t_ab]"
fi

fi

echo ""
echo "=== Results ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"
[ $FAIL -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $FAIL
