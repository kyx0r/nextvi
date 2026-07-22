/*
 * patch2vi - Convert unified diff patches to shell scripts using Nextvi ex commands
 *
 * Usage: patch2vi [-arih] [-d[N]] [-er TAG] [-ew TAG] [input.patch]
 *        patch2vi -e script.sh
 *        patch2vi [-ari]E [nextvi-opts...]
 *
 * Uses raw ex mode (:vis 3) with dynamic separator and escape character
 * selection via :sc! to avoid conflicts with : % ! \ characters in
 * patch content and to minimize the escaping the script needs. The
 * escape character is the next unused byte after the separator and is
 * exported as $ESC next to $SEP; if no byte is free, the default
 * backslash escape paths are kept.
 *
 * The generated script uses EXINIT with ex commands to apply changes.
 * The user can then modify the script to add regex-based matching for
 * more robust patch application.
 *
 * Nextvi is embedded whole: vi.c (and through it every editor module)
 * is compiled into this translation unit, and interactive mode runs the
 * built-in editor on in-memory buffers (see edit_buffers) - no temp
 * files, no argv, no EXINIT. build_patch2vi.sh renames nextvi's main()
 * out of the way for the build and restores it after. The same embedded
 * editor also executes generated scripts without a shell (-e), one
 * editor lifetime per script block, and turns a plain editing session
 * into a script (-E): that session is nextvi's own main() (nextvi_main()
 * after the rename), so everything past -E is a nextvi command line -
 * its flags, its files, EXINIT - and every buffer it leaves behind is
 * diffed against its
 * disk copy by the built-in differ to produce the input the converter
 * normally reads. Nothing is written to disk; quitting is what emits.
 */

#include "vi.c"

/* nextvi's own main(), renamed for this build by build_patch2vi.sh; -E
 * runs a whole editing session through it, flags, EXINIT and all */
int nextvi_main(int argc, char *argv[]);

#define MAX_LINE 8192
#define MAX_OPS 65536

typedef struct {
	int type;       /* 'd'=delete, 'a'=add, 'c'=context */
	int oline;      /* line number in original file */
	char *text;     /* line content (all op types: adds insert it,
			 * deletes/context anchor searches and diffs) */
	int hunk_lo;    /* 1-based first original line of the enclosing @@ hunk */
	int hunk_hi;    /* 1-based last original line of the enclosing @@ hunk */
} op_t;

struct group_s;

typedef struct {
	char *path;
	op_t ops[MAX_OPS];
	int nops;
	struct group_s *groups;  /* heap-allocated, set by build_file_groups */
	int ngroups;
	int is_new;              /* patch creates this file (--- /dev/null) */
	char *orig_path;         /* "---" path, holds pre-patch content (file-aware) */
} file_patch_t;

static file_patch_t files[256];
static int nfiles;
static const char *cur_file_path;  /* set per-file for error messages */
static int relative_mode;  /* 0=absolute, 1=relative search (-r) */
static int interactive_mode; /* 1=interactive editing of search patterns (-i) */
/* -1=per-group stored levels, 0=off, 1-5=forced level */
static int delta_mode;
/* patch (or previously generated script) path, NULL = stdin */
static const char *input_file;
static const char *end_tag_rd = "=== END ===";
static const char *end_tag_wr = "=== END ===";

/* The emit layer builds everything in memory. Function (not macro) wrappers
 * around the sbuf appenders let call sites sit in unbraced if/else bodies,
 * and sb_printf is printf into an sbuf (C99 vsnprintf, no streams). */
static void sb_str(sbuf *sb, const char *s)
{
	sbuf_str(sb, s)
}

static void sb_chr(sbuf *sb, int c)
{
	sbuf_chr(sb, c)
}

static void sb_mem(sbuf *sb, const char *s, int len)
{
	sbuf_mem(sb, s, len)
}

static void sb_printf(sbuf *sb, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (sb->s_n + n + 1 >= sb->s_sz) {
		sb->s_sz = NEXTSZ(sb->s_n, n + 1) + 1;
		sb->s = erealloc(sb->s, sb->s_sz);
	}
	va_start(ap, fmt);
	vsnprintf(sb->s + sb->s_n, n + 1, fmt, ap);
	va_end(ap);
	sb->s_n += n;
}

/* Number of f> anchor search strategies (SEARCH PATTERN slots), tried
 * strict-to-loose with first match wins. See default_pat_lines() for the
 * per-slot pattern composition. NFUZZ extra slots hold file-validated
 * relaxed (fuzzed) variants generated after the exact strategies; NGRP holds
 * the file-validated :grp-capture window (pattern 7, see gen_grp_window); NWIN
 * holds the file-validated global straddle window (pattern 8, mode 3, see
 * gen_win_window); NWIN2 holds a second straddle window with anchors one step
 * farther out (pattern 9, mode 3, same gen_win_window with skip=1). NSEARCH is
 * the total SEARCH PATTERN capacity per group. */
#define NPAT 5
#define NFUZZ 1   /* max file-validated fuzzed candidates per group (loosest kept) */
#define NGRP 1    /* file-validated :grp-capture window (TEXT.*? + last captured) */
#define NWIN 1    /* file-validated "top.*(bottom)" straddle window (pattern 8) */
#define NWIN2 1   /* second straddle window, anchors farther out (pattern 9) */
#define GRP_SLOT (NPAT + NFUZZ)         /* 0-based slot index of the grp window */
#define WIN_SLOT (NPAT + NFUZZ + NGRP)  /* 0-based slot index of the straddle window */
#define WIN2_SLOT (NPAT + NFUZZ + NGRP + NWIN)  /* 0-based slot index of the farther straddle window */
#define NSEARCH (NPAT + NFUZZ + NGRP + NWIN + NWIN2)  /* must stay <= 9: section numbers are 1 digit */

/* Scratch line mark reserved for pattern 8's save/restore of the cursor around
 * its global search; edit marks start at 1 (see next_mark_id callers). */
#define WIN_SAVE_MARK 0

/* Compatibility-block gate (-pr/-po). A compat block only applies to a tree
 * that carries the origin script's change; the gate is the question "is the
 * origin change that causes this collision present here?", asked as an exact
 * multi-line literal search before any edit and answered by quitting (q!0)
 * when the answer is no. Probes come from the origin's own landing (see
 * derive_gates): its inserted lines for GATE_PRESENT, its removed lines for
 * GATE_ABSENT. */
#define GATE_MAXLINES 8   /* longest probe window: locality beats length */
#define GATE_MAXPROBES 2  /* probe sections per block, ANDed in order */

enum {
	GATE_ALWAYS = 0,  /* no probe: the block is unconditional */
	GATE_PRESENT,     /* quit when the probe is missing (??!) */
	GATE_ABSENT,      /* quit when the probe is found (??) */
};

typedef struct {
	char **lines;     /* probe window, owned */
	int nlines;
	int polarity;     /* GATE_ALWAYS / GATE_PRESENT / GATE_ABSENT */
	int mode;         /* search mode: 0 = register window, 1 = single line */
	int tag;          /* allocated ?? capture id */
	int pre_escaped;  /* 1 = user-edited regex (exarg escaping only) */
} gate_t;

/* Per-group delta: structured customizations from interactive editing */
typedef struct {
	int group_idx;      /* 1-based */
	int level;          /* 1-5 comparison strictness, default 2 */
	int has_star;
	char **del_lines;    /* original patch del lines (used for raw comparison) */
	int ndel_lines, del_cap;
	char **add_lines;    /* original patch add lines */
	int nadd_lines, add_cap;
	char **custom_text;   /* user-edited text (replaces default -/+ lines as-is) */
	int ncustom_text, custom_text_cap;
	char **pre_ctx;     /* context lines before change (for levels 3/5) */
	int npre_ctx, pre_cap;
	char **post_ctx;    /* context lines after change (for levels 3/5) */
	int npost_ctx, post_cap;
	int strategy;       /* STRAT_DEFAULT = not recorded */
	char **pattern[NSEARCH];  /* SEARCH PATTERN 1-NSEARCH fallbacks */
	int npattern[NSEARCH], pat_cap[NSEARCH];
	int pat_off[NSEARCH];      /* per-pattern OFFSET marker value */
	int pat_has_off[NSEARCH];
	int pat_mode[NSEARCH];     /* per-pattern MODE: 0 = %f>, 1 = .,$f>,
				    * 2 = grp, 3 = global grp straddle */
	int pat_has_mode[NSEARCH];
	char **abs_cmd;
	int nabs, abs_cap;
	char **rel_cmd;
	int nrel, rel_cap;
	char **relc_cmd;
	int nrelc, relc_cap;
	/* Verbatim PHASE override: the exact ex-body bytes this group
	 * contributes to phase 1/phase 2 (no trailing newline). NULL = none.
	 * When set it supersedes every structured field above at emit time. */
	char *ph1, *ph2;
	int ovr_mark;       /* mark id the blobs reference */
	int ovr_esc;        /* dyn_esc byte at capture time (0 = backslash) */
} grp_delta_t;

typedef struct {
	char *filepath;
	grp_delta_t *grps;
	int ngrps;
	int gcap;
} file_delta_t;

static file_delta_t out_deltas[256];   /* output: captured from editor */
static int nout_deltas;

static file_delta_t in_deltas[256];    /* input: read from script */
static int nin_deltas;

enum strategy {
	STRAT_DEFAULT = 0,  /* use global mode default */
	STRAT_ABS,          /* absolute line numbers (;c for single-line diffs) */
	STRAT_REL,          /* f> regex search (s// for single-line diffs) */
	STRAT_RELC,         /* f> regex search + ;c horizontal edit */
};

/* Map "abs"/"rel"/"relc" → strategy (n=length to compare). */
static int strat_from_name(const char *s, int n)
{
	if (n == 3 && !strncmp(s, "abs", 3))
		return STRAT_ABS;
	if (n == 4 && !strncmp(s, "relc", 4))
		return STRAT_RELC;
	if (n == 3 && !strncmp(s, "rel", 3))
		return STRAT_REL;
	return STRAT_DEFAULT;
}

/* Detect substitute command: 's' followed by non-alphanumeric delimiter. */
static int is_substitute(const char *s)
{
	if (s[0] != 's' || !s[1])
		return 0;
	unsigned char c = s[1];
	return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		 (c >= '0' && c <= '9'));
}

/* Raw input lines for embedding in output */
static char *raw_lines[MAX_OPS * 4];
static int nraw = 0;

/* Track which bytes appear in patch content */
static unsigned char byte_used[256];

/* Dynamic ex escape byte set via :sc (like the separator); exported to
 * the script as $ESC. 0 = no free byte, keep the default backslash
 * escape paths. With a dynamic escape, backslash is no longer special
 * to ex_arg, so content and regex escapes pass through unmodified.
 * The ? conditional/while no longer delimiter-scans its argument
 * (it relies on capture tags), so ? never needs escaping inside a
 * ? block; only the separator does. */
static int dyn_esc;

static void *ecalloc(size_t n, size_t sz)
{
	void *p = calloc(n, sz);
	if (!p) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return p;
}

static void add_raw(const char *line)
{
	if (nraw >= (int)(sizeof(raw_lines) / sizeof(raw_lines[0]))) {
		fprintf(stderr, "too many input lines\n");
		exit(1);
	}
	raw_lines[nraw++] = uc_dup(line);
}

/* Remove trailing newline */
static void chomp(char *s)
{
	int n = strlen(s);
	while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r'))
		s[--n] = '\0';
}

/* Backslash-escape every char of s that appears in set. */
static char *escape_chars(const char *s, const char *set)
{
	sbuf_smake(sb, strlen(s) + 8)
	for (; *s; s++) {
		if (strchr(set, *s))
			sbuf_chr(sb, '\\')
		sbuf_chr(sb, *s)
	}
	sbufn_ret(sb, sb->s)
}

#define REGEX_META "\\^$.*+?[(){|"
static char *escape_regex(const char *s)
{
	return escape_chars(s, REGEX_META);
}

/* Append a string to a dynamic array */
static void arr_append(char ***arr, int *n, int *cap, const char *s)
{
	if (*n >= *cap) {
		*cap = *cap ? *cap * 2 : 4;
		*arr = erealloc(*arr, *cap * sizeof(char *));
	}
	(*arr)[(*n)++] = uc_dup(s);
}

static int lines_equal(char **a, int na, char **b, int nb)
{
	if (na != nb)
		return 0;
	for (int i = 0; i < na; i++)
		if (strcmp(a[i], b[i]) != 0)
			return 0;
	return 1;
}

/*
 * File-aware anchor validation. patch2vi normally compiles blind, emitting a
 * strict-to-loose fallback chain that nextvi resolves at apply time. When the
 * pre-patch original is readable (it usually is - the script applies in the same
 * tree), we count each candidate anchor's occurrences and sort the proven-unique
 * one to the front. The full chain is still emitted, so the script stays portable
 * and drift-tolerant; file access only improves ordering.
 */
static char **orig_lines;   /* pre-patch original, NULL if unreadable */
static int n_orig_lines;

static void load_orig_file(const char *path)
{
	orig_lines = NULL;
	n_orig_lines = 0;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	int cap = 0;
	char buf[MAX_LINE];
	while (fgets(buf, sizeof buf, f)) {
		int len = strlen(buf);
		if (len && buf[len - 1] == '\n')
			buf[len - 1] = '\0';
		arr_append(&orig_lines, &n_orig_lines, &cap, buf);
	}
	fclose(f);
}

static void free_orig_file(void)
{
	for (int i = 0; i < n_orig_lines; i++)
		free(orig_lines[i]);
	free(orig_lines);
	orig_lines = NULL;
	n_orig_lines = 0;
}

/* True if window[0..n) matches orig_lines exactly at 0-based index idx. */
static int window_at(char **window, int n, int idx)
{
	if (idx < 0 || n <= 0 || idx + n > n_orig_lines)
		return 0;
	for (int j = 0; j < n; j++)
		if (strcmp(orig_lines[idx + j], window[j]) != 0)
			return 0;
	return 1;
}

/* Count exact consecutive-line matches of window[0..n) in orig_lines.
 * Sets *first to the 0-based start of the first match (-1 if none). */
static int count_window(char **window, int n, int *first)
{
	*first = -1;
	if (!orig_lines || n <= 0 || n > n_orig_lines)
		return 0;
	int cnt = 0;
	for (int i = 0; i + n <= n_orig_lines; i++) {
		int ok = 1;
		for (int j = 0; j < n; j++)
			if (strcmp(orig_lines[i + j], window[j]) != 0) {
				ok = 0;
				break;
			}
		if (ok) {
			if (*first < 0)
				*first = i;
			cnt++;
		}
	}
	return cnt;
}

/* Count consecutive-line matches of a substring window in orig_lines: each
 * win[j] must occur as a substring of the aligned original line (an empty
 * win[j] matches any line, mirroring ".*.*"). This is the match semantics of
 * the pattern-7 ".*TEXT.*" window. Sets *first to the 0-based start of the
 * first match (-1 if none). */
static int count_window_substr(char **win, int n, int *first)
{
	*first = -1;
	if (!orig_lines || n <= 0 || n > n_orig_lines)
		return 0;
	int cnt = 0;
	for (int i = 0; i + n <= n_orig_lines; i++) {
		int ok = 1;
		for (int j = 0; j < n; j++)
			if (win[j][0] && !strstr(orig_lines[i + j], win[j])) {
				ok = 0;
				break;
			}
		if (ok) {
			if (*first < 0)
				*first = i;
			cnt++;
		}
	}
	return cnt;
}

/* Count orig lines in the 0-based inclusive range [from..to] that contain s as
 * a substring. Used to prove a pattern-8 bottom anchor is unambiguous below the
 * hunk: greedy ".*(bottom)" captures the last occurrence, so the chosen line
 * must be the only one carrying that text from its position to EOF. */
static int count_substr_range(const char *s, int from, int to)
{
	int cnt = 0;
	if (from < 0)
		from = 0;
	if (to > n_orig_lines - 1)
		to = n_orig_lines - 1;
	for (int i = from; i <= to; i++)
		if (s[0] && strstr(orig_lines[i], s))
			cnt++;
	return cnt;
}

/*
 * Specificity of an exact-line anchor window: a measure of how strongly it
 * discriminates, used to order the fallback chain strict to loose. Contiguous
 * literal runs disambiguate far better than the same number of characters
 * scattered between wildcards, so each line (one contiguous run) contributes
 * its length squared. Per-line length is capped so a pathological line cannot
 * overflow or swamp the uniqueness bonus added by the caller. */
#define SPEC_LINE_CAP 1000
#define SPEC_MAX (1 << 26)
static int specificity_score(char **lines, int n)
{
	long s = 0;
	for (int i = 0; i < n; i++) {
		int len = (int)strlen(lines[i]);
		if (len > SPEC_LINE_CAP)
			len = SPEC_LINE_CAP;
		s += (long)len * len;
		if (s > SPEC_MAX) {
			s = SPEC_MAX;
			break;
		}
	}
	return (int)s;
}

/* Uniqueness bonus dominates any specificity score so a proven anchor always
 * sorts ahead of an unproven one of the same shape (used when picking the
 * loosest file-validated fuzz window in gen_fuzz_windows). */
#define UNIQUE_BONUS (1 << 28)   /* exactly one match, at the right place */

/*
 * File-validated fuzzed (relaxed) anchors. With the original readable we can
 * relax an exact anchor into a drift-tolerant regex, verifying the relaxed form
 * still resolves uniquely to the right place. A fuzzed window replaces selected
 * runes with '.' (the nextvi one-rune wildcard). Length-preserving: each '.' is
 * one rune, so a fuzzed line matches only same-rune-length lines - it tolerates
 * in-place character drift (renamed equal-length token, changed digit), nothing
 * else. A candidate is kept only if it still matches uniquely at the expected
 * location. Without the file, no fuzzed anchors are emitted.
 */

/* Count runes in the first len bytes of s. */
static int rune_count_n(const char *s, int len)
{
	int n = 0;
	for (int i = 0; i < len; i++)
		if ((s[i] & 0xC0) != 0x80)
			n++;
	return n;
}

/* A fuzzed line: base text plus a per-rune wildcard mask (1 = becomes '.'). */
typedef struct {
	const char *base;     /* borrowed plain text */
	unsigned char *mask;  /* nrune bytes, owned */
	int nrune;
} fline_t;

/* True if orig matches the fuzzed line: same rune count, and every unmasked
 * rune is byte-identical (masked runes match any single rune). */
static int match_fuzzy_line(const char *orig, const fline_t *f)
{
	const char *o = orig, *b = f->base;
	for (int i = 0; i < f->nrune; i++) {
		if (!*o)
			return 0;
		int ol = uc_len(o), bl = uc_len(b);
		if (!f->mask[i] && (ol != bl || memcmp(o, b, ol) != 0))
			return 0;
		o += ol;
		b += bl;
	}
	return *o == 0;
}

/* Count consecutive-line matches of a fuzzed window in orig_lines; *first =
 * 0-based start of the first match (-1 if none). */
static int count_window_fuzzy(fline_t *win, int n, int *first)
{
	*first = -1;
	if (!orig_lines || n <= 0 || n > n_orig_lines)
		return 0;
	int cnt = 0;
	for (int i = 0; i + n <= n_orig_lines; i++) {
		int ok = 1;
		for (int j = 0; j < n; j++)
			if (!match_fuzzy_line(orig_lines[i + j], &win[j])) {
				ok = 0;
				break;
			}
		if (ok) {
			if (*first < 0)
				*first = i;
			cnt++;
		}
	}
	return cnt;
}

/* Specificity of a fuzzed window: contiguous unmasked literal runs (in bytes)
 * contribute length squared, exactly like specificity_score but skipping
 * wildcards, so a more relaxed window scores strictly lower. */
static int fuzzy_spec(fline_t *win, int n)
{
	long s = 0;
	for (int j = 0; j < n; j++) {
		const char *b = win[j].base;
		int run = 0;
		for (int i = 0; i < win[j].nrune; i++) {
			int bl = uc_len(b);
			if (!win[j].mask[i]) {
				run += bl;
			} else {
				if (run > SPEC_LINE_CAP)
					run = SPEC_LINE_CAP;
				s += (long)run * run;
				run = 0;
			}
			b += bl;
		}
		if (run > SPEC_LINE_CAP)
			run = SPEC_LINE_CAP;
		s += (long)run * run;
		if (s > SPEC_MAX) {
			s = SPEC_MAX;
			break;
		}
	}
	return (int)s;
}

/* Build the pre-escaped regex for a fuzzed line: masked runes emit '.', literal
 * runes are regex-escaped. */
static char *fuzzy_regex(const fline_t *f)
{
	sbuf_smake(sb, strlen(f->base) + 8)
	const char *b = f->base;
	for (int i = 0; i < f->nrune; i++) {
		int bl = uc_len(b);
		if (f->mask[i]) {
			sbuf_chr(sb, '.')
		} else {
			for (int k = 0; k < bl; k++) {
				if (b[k] && strchr(REGEX_META, b[k]))
					sbuf_chr(sb, '\\')
				sbuf_chr(sb, b[k])
			}
		}
		b += bl;
	}
	sbufn_ret(sb, sb->s)
}

static unsigned hash_str(const char *s, unsigned h)
{
	while (*s)
		h = h * 131u + (unsigned char)*s++;
	return h;
}

/* Deterministic per-position pseudo-value, content-seeded so the fuzzing of a
 * given hunk is reproducible across runs. */
static unsigned hash_pos(unsigned seed, int i)
{
	unsigned h = seed ^ 0x9e3779b9u;
	h ^= (unsigned)i * 2654435761u;
	h ^= h >> 13;
	h *= 0x85ebca6bu;
	h ^= h >> 16;
	return h;
}

/* Highest fuzz level tried; level 0 is the lightest relaxation, the top level
 * approaches the ~80% wildcard budget the anchor is allowed (FUZZ_MASK_MAX). */
#define FUZZ_MAXLVL 8
#define FUZZ_MASK_MAX 800   /* per-mille: mask up to ~80% of runes (~20% literal) */

/* Fill mask[0..nrune) for fuzz level lvl over a window-global rune index that
 * starts at *gi (advanced by nrune). Each level drops runes by a content-seeded
 * threshold that grows with lvl but never past FUZZ_MASK_MAX, so the loosest
 * variant wildcards ~80% of its runes (~20% kept literal). At least one rune is
 * always kept literal. */
static void fuzz_mask(unsigned char *mask, int nrune, int lvl, unsigned seed,
		      int *gi)
{
	int thr = (lvl + 1) * FUZZ_MASK_MAX / (FUZZ_MAXLVL + 1);
	int kept = 0;
	for (int i = 0; i < nrune; i++) {
		int g = (*gi)++;
		int drop = (int)(hash_pos(seed, g) % 1000) < thr;
		mask[i] = drop ? 1 : 0;
		if (!drop)
			kept++;
	}
	if (!kept && nrune > 0)
		mask[0] = 0;  /* never wildcard an entire line away */
}

/* arr_append a slice of src[0..sn) into dst. */
static void arr_clone(char ***dst, int *dn, int *dc, char **src, int sn)
{
	for (int i = 0; i < sn; i++)
		arr_append(dst, dn, dc, src[i]);
}

/* Join an array of strings with '\n' into a single allocated string. */
static char *join_lines(char **lines, int nlines)
{
	sbuf_smake(sb, 128)
	for (int i = 0; i < nlines; i++) {
		if (i)
			sbuf_chr(sb, '\n')
		sbuf_str(sb, lines[i])
	}
	sbufn_ret(sb, sb->s)
}

/* Build the default display text (as it appears in the editor buffer) from patch del/add lines.
 * Returns e.g. "-line1\n-line2\n+line3\n+line4\n" */
static char *build_default_text(char **del, int ndel, char **add, int nadd)
{
	sbuf_smake(sb, 128)
	for (int i = 0; i < ndel; i++) {
		sbuf_chr(sb, '-')
		sbuf_str(sb, del[i])
		sbuf_chr(sb, '\n')
	}
	for (int i = 0; i < nadd; i++) {
		sbuf_chr(sb, '+')
		sbuf_str(sb, add[i])
		sbuf_chr(sb, '\n')
	}
	sbufn_ret(sb, sb->s)
}

/* True if gd's stored del/add lines match the supplied content (or weren't recorded).
 * Used when * is absent (non-regex path) — always compares del_lines/add_lines. */
static int grp_content_matches(grp_delta_t *gd, char **del, int ndel,
			       char **add, int nadd)
{
	if (gd->ndel_lines == 0 && gd->nadd_lines == 0)
		return 1;
	if (gd->ndel_lines != ndel || gd->nadd_lines != nadd)
		return 0;
	return lines_equal(gd->del_lines, gd->ndel_lines, del, ndel)
	       && lines_equal(gd->add_lines, gd->nadd_lines, add, nadd);
}

/* Match gd's custom_text as one regex against the combined patch default text. */
static int grp_content_regex_matches(grp_delta_t *gd, char **del, int ndel,
				     char **add, int nadd)
{
	if (gd->ncustom_text == 0)
		return 1;
	char *pat = join_lines(gd->custom_text, gd->ncustom_text);
	char *target = build_default_text(del, ndel, add, nadd);
	rset *rs = rset_smake(pat, 0);
	int ok = rs && rset_match(rs, target, 0);
	rset_free(rs);
	free(pat);
	free(target);
	return ok;
}

/* True if gd's stored full hunk (pre_ctx + del + add + post_ctx) matches the supplied content. */
static int grp_full_hunk_matches(grp_delta_t *gd,
				 char **pre_ctx, int npre_ctx,
				 char **del_texts, int ndel,
				 char **add_texts, int nadd,
				 char **post_ctx, int npost_ctx)
{
	if (!lines_equal(gd->pre_ctx, gd->npre_ctx, pre_ctx, npre_ctx))
		return 0;
	if (gd->ndel_lines != ndel || gd->nadd_lines != nadd)
		return 0;
	if (!lines_equal(gd->del_lines, gd->ndel_lines, del_texts, ndel))
		return 0;
	if (!lines_equal(gd->add_lines, gd->nadd_lines, add_texts, nadd))
		return 0;
	if (!lines_equal(gd->post_ctx, gd->npost_ctx, post_ctx, npost_ctx))
		return 0;
	return 1;
}

static grp_delta_t *find_grp_delta(file_delta_t *fd, int idx,
				   char **del_texts, int ndel,
				   char **add_texts, int nadd,
				   char **pre_ctx, int npre_ctx,
				   char **post_ctx, int npost_ctx,
				   int force_level)
{
	for (int i = 0; fd && i < fd->ngrps; i++) {
		grp_delta_t *gd = &fd->grps[i];
		int lvl = force_level > 0 ? force_level : gd->level;
		if (lvl == 0)
			lvl = 2;  /* default for old format deltas */

		if (lvl == 4) {
			/* Level 4: content match (like lvl 2), no index check */
			if (gd->has_star && gd->level == 4
			    && grp_content_regex_matches(gd, del_texts, ndel, add_texts, nadd))
				return gd;
			if (grp_content_matches(gd, del_texts, ndel, add_texts, nadd))
				return gd;
			continue;
		}

		if (lvl == 5) {
			/* Level 5: full hunk match, no index check */
			if (grp_full_hunk_matches(gd, pre_ctx, npre_ctx,
						  del_texts, ndel,
						  add_texts, nadd,
						  post_ctx, npost_ctx))
				return gd;
			continue;
		}

		/* Levels 1, 2, 3: group index must match first */
		if (gd->group_idx != idx)
			continue;

		if (lvl == 1)
			return gd;
		if (lvl == 2) {
			if (gd->has_star && gd->level == 2
			    && grp_content_regex_matches(gd, del_texts, ndel, add_texts, nadd))
				return gd;
			if (grp_content_matches(gd, del_texts, ndel, add_texts, nadd))
				return gd;
		}
		if (lvl == 3 && grp_full_hunk_matches(gd, pre_ctx, npre_ctx,
						      del_texts, ndel,
						      add_texts, nadd,
						      post_ctx, npost_ctx))
			return gd;
	}
	return NULL;
}

/* Count how many times substring needle appears in haystack (including overlaps) */
static int count_occurrences(const char *haystack, const char *needle)
{
	int count = 0;
	const char *p = haystack;
	while ((p = strstr(p, needle)) != NULL) {
		count++;
		p++;
	}
	return count;
}

/* Count occurrences of s[start..end) as a substring of s. */
static int range_occurs(const char *s, int start, int end)
{
	int len = end - start;
	char tmp[len + 1];
	memcpy(tmp, s + start, len);
	tmp[len] = '\0';
	return count_occurrences(s, tmp);
}

/* Step os back 1 byte and over UTF-8 continuation bytes, mirroring on ns. */
static int diff_expand_left(const char *old, int *os, int *ns)
{
	if (*os <= 0)
		return 0;
	int prev = *os;
	(*os)--;
	while (*os > 0 && (old[*os] & 0xC0) == 0x80)
		(*os)--;
	*ns -= (prev - *os);
	return 1;
}

/* Step oe forward 1 byte and over UTF-8 continuation bytes, mirroring on ne. */
static int diff_expand_right(const char *old, int olen, int *oe, int *ne)
{
	if (*oe >= olen)
		return 0;
	int prev = *oe;
	(*oe)++;
	while (*oe < olen && (old[*oe] & 0xC0) == 0x80)
		(*oe)++;
	*ne += (*oe - prev);
	return 1;
}

/*
 * Compare two lines and find the differing portion.
 * Returns 1 if suitable for horizontal edit, 0 otherwise.
 * Sets *old_text to the differing old-side span and *new_text to its
 * replacement (both allocated).
 */
static int find_line_diff(const char *old, const char *new,
			  char **old_text, char **new_text)
{
	int old_len = strlen(old);
	int new_len = strlen(new);

	/* Find common prefix (in bytes), snapped back to a rune boundary so
	 * runes sharing lead bytes (e.g. é vs è) are never split */
	int prefix = 0;
	while (old[prefix] && new[prefix] && old[prefix] == new[prefix])
		prefix++;
	while (prefix > 0 && (old[prefix] & 0xC0) == 0x80)
		prefix--;

	/* Find common suffix (in bytes), but don't overlap with prefix;
	 * snap forward so the cut lands on a rune boundary */
	int suffix = 0;
	while (suffix < old_len - prefix && suffix < new_len - prefix &&
	       old[old_len - 1 - suffix] == new[new_len - 1 - suffix])
		suffix++;
	while (suffix > 0 && (old[old_len - suffix] & 0xC0) == 0x80)
		suffix--;

	/* Calculate the differing regions */
	int old_diff_start = prefix;
	int old_diff_end = old_len - suffix;
	int new_diff_start = prefix;
	int new_diff_end = new_len - suffix;

	/* Require at least 50% of the line to be common */
	int common = prefix + suffix;
	if (common < old_len / 2 && common < new_len / 2)
		return 0;

	/* Don't bother with horizontal edit if most of line changes */
	int old_diff_len = old_diff_end - old_diff_start;
	int new_diff_len = new_diff_end - new_diff_start;
	if (old_diff_len > old_len / 2 && new_diff_len > new_len / 2)
		return 0;

	/* Pure insertion: old_text is empty, expand to get searchable context.
	 * Empty search pattern in s// is never valid. */
	if (old_diff_end == old_diff_start) {
		diff_expand_left(old, &old_diff_start, &new_diff_start);
		diff_expand_right(old, old_len, &old_diff_end, &new_diff_end);
		if (old_diff_end == old_diff_start)
			return 0;
	}

	/* Expand diff region until old_text is unique on the line.
	 * Since prefix and suffix are shared between old and new,
	 * expanding symmetrically keeps both regions aligned. */
	while (old_diff_end - old_diff_start > 0) {
		if (range_occurs(old, old_diff_start, old_diff_end) <= 1)
			break;
		/* Prefer left expansion, then right if still not unique. */
		int expanded = diff_expand_left(old, &old_diff_start, &new_diff_start);
		if (!expanded || range_occurs(old, old_diff_start, old_diff_end) > 1)
			expanded |= diff_expand_right(old, old_len, &old_diff_end, &new_diff_end);
		if (!expanded)
			break;
		if (old_diff_start == 0 && old_diff_end == old_len)
			break;
	}

	/* Recalculate diff lengths after expansion */
	old_diff_len = old_diff_end - old_diff_start;
	new_diff_len = new_diff_end - new_diff_start;

	/* Extract the old text */
	*old_text = emalloc(old_diff_len + 1);
	memcpy(*old_text, old + old_diff_start, old_diff_len);
	(*old_text)[old_diff_len] = '\0';

	/* Extract the new text */
	*new_text = emalloc(new_diff_len + 1);
	memcpy(*new_text, new + new_diff_start, new_diff_len);
	(*new_text)[new_diff_len] = '\0';

	return 1;
}

/* Mark all bytes in a string as used */
static void mark_bytes_used(const char *s)
{
	for (; *s; s++)
		byte_used[(unsigned char)*s] = 1;
}

/* Find an unused byte to use as separator.
 * Prefer non-printable bytes so printable chars stay available
 * for ex commands and patterns. */
static int find_unused_byte(void)
{
	for (int c = 1; c < 256; c++)
		if (!byte_used[c])
			return c;
	return -1;  /* All bytes used - very unlikely */
}

/* List all unused bytes suitable as separators */
static void list_unused_bytes(sbuf *out)
{
	int n = 0;
	sb_printf(out, "# Available separators:");
	int range_start = -1;
	for (int c = 1; c <= 256; c++) {
		int unused = (c < 256) && !byte_used[c];
		if (unused && range_start < 0) {
			range_start = c;
		} else if (!unused && range_start >= 0) {
			int range_end = c - 1;
			if (range_end == range_start)
				sb_printf(out, " 0%03o", range_start);
			else
				sb_printf(out, " 0%03o-0%03o", range_start, range_end);
			n++;
			range_start = -1;
		}
	}
	if (!n)
		sb_printf(out, " (none)");
	sb_chr(out, '\n');
}

/* Parse a hunk header: @@ -old_start,old_count +new_start,new_count @@ */
static int parse_hunk_header(const char *line, int *old_start, int *old_count)
{
	if (strncmp(line, "@@ -", 4) != 0)
		return 0;
	const char *p = line + 4;
	*old_start = atoi(p);
	*old_count = 1;
	while (*p && *p != ',' && *p != ' ')
		p++;
	if (*p == ',') {
		p++;
		*old_count = atoi(p);
	}
	/* Verify '+' field exists to confirm it's a valid hunk header */
	while (*p && *p != '+')
		p++;
	return *p == '+';
}

/* Escape a string for shell double-quoted string */
static void emit_escaped_line(sbuf *out, const char *s)
{
	for (; *s; s++) {
		unsigned char c = *s;
		/* the dynamic escape byte never occurs in content; emit it
		 * as the readable ${ESC} expansion */
		if (dyn_esc && c == (unsigned char)dyn_esc) {
			sb_str(out, "${ESC}");
			continue;
		}
		/* Shell double-quote escapes: $, `, ", \ */
		if (c == '\\' || c == '$' || c == '`' || c == '"') {
			sb_chr(out, '\\');
		}
		sb_chr(out, c);
	}
}

static void emit_escaped_text(sbuf *out, const char *s);

/* the body register, and the EXINIT that yanks the body buffer into it
 * and runs it; -e fills the register itself and needs neither */
#define P2VI_REG 97
#define P2VI_VICALL "EXINIT='%ya 97:? %@97'"
/* separator: shell expands ${SEP} in double-quoted EXINIT */
#define EMIT_SEP(out) sb_str(out, "${SEP}")
/* escaped separator inside ??! block: <esc><sep> for ex_arg */
#define EMIT_ESCSEP(out) \
	sb_str(out, dyn_esc ? "${ESC}${SEP}" : "\\\\${SEP}")
/* triply-escaped separator inside a ?? then-arg nested in a ? cond:
 * <esc><esc><esc><sep> */
#define EMIT_ESC3SEP(out) \
	sb_str(out, dyn_esc ? "${ESC}${ESC}${ESC}${SEP}" : "\\\\\\\\\\\\${SEP}")

/*
 * Ex commands emitted by patch2vi and their default range (no address given):
 *
 * All commands default to the current line (xrow) when no range is given,
 * per ex_region(): beg = xrow, end = xrow+1 when vaddr == 0.
 *
 * Commands that advance xrow (and thus affect subsequent relative addresses):
 *   i (insert)     - ec_insert: inserts after the addressed line ("0i" is
 *                    above line 1); xrow = beg + inserted_lines - 1
 *   c (change)     - ec_insert: xrow = end + inserted_lines - deleted - 1
 *   d (delete)     - ec_delete: xrow = beg (or last line if past end)
 *   f>/f+/f- (find)- ec_find: xrow = matched line, xoff = match position
 *   (bare address) - ec_print (!*cmd && *loc): xrow = end - 1
 *                    This is how +N / -N move xrow without a command.
 *
 * Commands that do NOT advance xrow:
 *   s (substitute) - ec_substitute: does not modify xrow/xoff
 *   p (print)      - ec_print: an explicit command letter skips the
 *                    xrow = end - 1 bare-address path (only used for debug)
 *
 * Commands used for setup/teardown (no range relevance):
 *   vis (visual)   - ec_print: sets xvis mode
 *   wq (write+quit)- ec_write: writes file and quits
 *   q! (quit)      - ec_quit: quits without saving
 *   sc! (specials) - ec_specials: sets ex separator character
 *   ??! (while)    - ec_while: conditional execution (error check)
 *   m (mark)       - ec_mark: sets a line mark, does not move xrow.
 *                    "+2m 0" marks cursor+2 as mark <0>; the mark
 *                    auto-adjusts in lbuf_replace() when edits above
 *                    it insert or delete lines.
 *   'N             - mark address: "'0c" edits the marked line.
 *
 * When emitting relative-mode positions (offset from search result),
 * +N / -N are equivalent to .+N / .-N since +/- default to current line.
 */

/* Emit content lines for an a/c/i ex command. */
static void emit_content(sbuf *out, char **texts, int ntexts)
{
	for (int i = 0; i < ntexts; i++) {
		emit_escaped_text(out, texts[i]);
		sb_chr(out, '\n');
	}
}

/* Emit ex commands for inserting text after line N.
 * "Ni" inserts after line N; "0i" inserts above the first line.
 * New files have an empty buffer with no addressable line, so the
 * insert is emitted bare ("i"). */
static void emit_insert_after(sbuf *out, int line, char **texts, int ntexts,
			      int is_new)
{
	if (ntexts == 0)
		return;

	if (is_new)
		sb_printf(out, "i ");
	else if (line <= 0)
		sb_printf(out, "0i ");
	else
		sb_printf(out, "%di ", line);
	emit_content(out, texts, ntexts);
	EMIT_SEP(out);
}

/* Emit ex commands for deleting lines from N to M inclusive */
static void emit_delete(sbuf *out, int from, int to)
{
	if (from == to)
		sb_printf(out, "%dd", from);
	else
		sb_printf(out, "%d,%dd", from, to);
	EMIT_SEP(out);
}

/* Emit ex command for horizontal (character-level) edit */
static void emit_horizontal_change(sbuf *out, int line, int char_start,
				   int char_end,
				   const char *new_text)
{
	if (!*new_text) {
		if (char_start == char_end)
			return;
		sb_printf(out, "%d;%d;%dd", line, char_start, char_end);
	} else if (char_start == char_end) {
		sb_printf(out, "%d;%dc ", line, char_start);
		emit_escaped_text(out, new_text);
	} else {
		sb_printf(out, "%d;%d;%dc ", line, char_start, char_end);
		emit_escaped_text(out, new_text);
	}
	EMIT_SEP(out);
}

/* Emit ex commands for changing lines (delete and insert) */
static void emit_change(sbuf *out, int from, int to, char **texts, int ntexts)
{
	if (ntexts == 0) {
		emit_delete(out, from, to);
		return;
	}

	if (from == to)
		sb_printf(out, "%dc ", from);
	else
		sb_printf(out, "%d,%dc ", from, to);
	emit_content(out, texts, ntexts);
	EMIT_SEP(out);
}

/*
 * Relative mode emit functions - use regex patterns instead of line numbers.
 *
 * Two-phase emission per file:
 *
 * Phase 1 (resolve): the whole buffer is yanked once into register <b>
 * (the find register, selected by fr 98) right after the file is opened.
 * All groups' searches then run top-to-bottom against this cache with
 * no edits in between, so the register stays byte-identical to the
 * buffer for the entire phase. Each group's target line is recorded
 * with a line mark ("+<off>m <id>", ids count up from 0 skipping
 * nextvi's special mark ids). Each group gets up to NSEARCH fallback
 * patterns tried strict-to-loose, first match wins (see
 * emit_fallback_chain; slots past NPAT are the file-validated
 * fuzz/grp/straddle windows). Exact defaults per default_pat_lines():
 * 1 = whole hunk (pre ctx +
 * deleted lines + post ctx), 2 = deleted lines + post ctx, 3 = top
 * context anchors only, 4 = deleted lines only, 5 = post ctx only.
 * Searches use %f> (first search of a file) / %f+ (subsequent)
 * against the register cache: with the fr option set,
 * a range maps the match back to a buffer position, and f+ continues
 * one char past the previous match start (the cursor).
 * When only one pattern survives dedup, single-line patterns
 * search the buffer directly with ";0 fr .,$f> ^pattern$ .. fr 98"
 * (.,$f+ after the first search): ;0 resets xoff to the line start
 * and the ^...$ anchors disambiguate repeated text. ABS-strategy
 * groups mark their original line number directly - the buffer is
 * pristine in this phase, so no cumulative line-delta correction is
 * needed.
 *
 * Phase 2 (commit): edits are emitted addressing the marks ('0c, '0d,
 * '0,#+Nc, '0s/../../, '0;A;Bc ...). Marks auto-adjust as edits above
 * them shift lines, so groups apply forward in patch order. Because
 * every search ran before the first edit, any failed anchor aborts
 * with the file completely untouched.
 *
 * All search paths use ex_arg escaping uniformly.
 *
 * Error checking: each search is followed by ??! to detect failure,
 * print debug info, and quit before corrupting the file. Each phase-2
 * edit gets the same check with a FAIL m<mark id> message.
 */

/* Emit ??! error check after a command that may fail.
 * loc: location text in the FAIL message ("path:line" for phase-1
 * searches, "path:line:m<id>" for phase-2 edits at a mark).
 * phase selects the DBG<n>/QF<n> variable set; INTR is shared.
 * tags (optional, may be NULL) prefixes the conditional with a DNF
 * capture-id expression so it branches on recorded statuses instead
 * of the last command's. */
static void emit_err_check_loc(sbuf *out, const char *loc, int phase,
			       const char *tags)
{
	if (tags)
		sb_str(out, tags);
	/* "?" "?!" split: "??!" in one literal is the trigraph for '|' */
	sb_printf(out, "?" "?!${DBG%d:-ya!112", phase);
	EMIT_ESCSEP(out);
	sb_printf(out, "prp");
	EMIT_ESCSEP(out);
	sb_printf(out, "p FAIL %s", loc);
	EMIT_ESCSEP(out);
	sb_printf(out, "pr");
	sb_str(out, "${INTR}");
	sb_printf(out, "${QF%d}}", phase);
	EMIT_SEP(out);
}

/* Phase-1 error check: FAIL <path>:<line> */
static void emit_err_check(sbuf *out, int line)
{
	char loc[MAX_LINE];
	snprintf(loc, sizeof(loc), "%s:%d",
		 cur_file_path ? cur_file_path : "?", line);
	emit_err_check_loc(out, loc, 1, NULL);
}

/* Phase-1 fallback chain check: one <0;1;..>??! over all capture tags
 * (DNF OR); the inverted branch fires only when every pattern's
 * capture recorded a failure. */
static void emit_err_check_pats(sbuf *out, const int *pids, int ntags, int line)
{
	char loc[MAX_LINE];
	char tags[NSEARCH * 8];
	int p = 0;
	for (int t = 0; t < ntags && p < (int)sizeof(tags); t++)
		p += snprintf(tags + p, sizeof(tags) - p,
			      t ? ";%d" : "%d", pids[t]);
	snprintf(loc, sizeof(loc), "%s:%d",
		 cur_file_path ? cur_file_path : "?", line);
	emit_err_check_loc(out, loc, 1, tags);
}

/* Phase-2 error check: FAIL <path>:<line>:m<id> (mark id of the edited
 * group). mark_id < 0 means no mark (new-file insert, custom abs command). */
static void emit_err_check_mark(sbuf *out, int line, int mark_id)
{
	char loc[MAX_LINE];
	char mark[16] = "m";
	if (mark_id >= 0)
		snprintf(mark, sizeof(mark), "m%d", mark_id);
	snprintf(loc, sizeof(loc), "%s:%d:%s",
		 cur_file_path ? cur_file_path : "", line, mark);
	emit_err_check_loc(out, loc, 2, NULL);
}

/* Phase-2 substitute-chain check: one <0;1;..>??! over all rung tags (DNF
 * OR); the inverted branch fires only when every substitute variant in the
 * progression recorded a match failure. Mirrors emit_err_check_pats but at a
 * mark (phase 2). */
static void emit_err_check_subs(sbuf *out, const int *sids, int nrungs,
				int line, int mark_id)
{
	char loc[MAX_LINE];
	char mark[16] = "m";
	char tags[NSEARCH * 8];
	int p = 0;
	if (mark_id >= 0)
		snprintf(mark, sizeof(mark), "m%d", mark_id);
	for (int t = 0; t < nrungs && p < (int)sizeof(tags); t++)
		p += snprintf(tags + p, sizeof(tags) - p,
			      t ? ";%d" : "%d", sids[t]);
	snprintf(loc, sizeof(loc), "%s:%d:%s",
		 cur_file_path ? cur_file_path : "", line, mark);
	emit_err_check_loc(out, loc, 2, tags);
}

/* Double backslashes for ex_arg level escaping.
 * ex_arg treats \\ as escaped \, so \\\\ is needed to preserve \\.
 * With a dynamic escape byte, backslash is not special to ex_arg and
 * passes through as-is (the escape byte never occurs in content). */
static char *escape_exarg(const char *s)
{
	return escape_chars(s, dyn_esc ? "" : "\\");
}

/* Emit text that passes through ex_arg then shell double-quotes.
 * ex_arg consumes \\ -> \, so backslashes need doubling for ex_arg,
 * then doubling again for shell. */
static void emit_escaped_text(sbuf *out, const char *s)
{
	char *exarg_esc = escape_exarg(s);
	emit_escaped_line(out, exarg_esc);
	free(exarg_esc);
}

/* Emit f> search with error check, then mark the target line.
 * Single-line patterns search the buffer directly (fr .. fr 98)
 * from the cursor's line: ";0" first resets xoff to the line start,
 * then ".,$f> ^pattern$" - the ^...$ anchors plus the .,$ range
 * disambiguate repeated text. The first search of a file uses
 * .,$f>, subsequent ones .,$f+.
 * Multi-line patterns run against the cached find register via
 * %f> (first search of a file) or %f+ (subsequent: f+ skips one
 * char from the previous match start so identical anchors find
 * the next occurrence; the % range maps the match back to a
 * buffer position).
 * pre_escaped: 0 = anchors are raw text (apply regex+exarg escape),
 *              1 = anchors are pre-escaped regex (apply exarg only).
 * mode: 1 = direct buffer search (.,$f>, fr/fr 98, ^...$ anchors);
 *       0 = register-cache search (%f>); 2 = grp register search, like 0 but
 *       bracketed with "grp 1 .. grp 0" so the find lands on the captured
 *       group (pattern 7); 3 = global grp straddle window (pattern 8), like 2
 *       but the cursor is saved to mark WIN_SAVE_MARK and reset to the top
 *       ("1;0") so the search runs globally, then restored after marking.
 *       Defaults to 1 for single-line patterns, 0 otherwise; the OFFSET MODE
 *       marker can override.
 * After the search, "+<offset>m <mark_id>" marks the target line
 * without moving the cursor. */
static void emit_search(sbuf *out, char **anchors, int nanchors,
			int offset, int mark_id,
			int target_line, int pre_escaped, int first, int mode)
{
	int single = mode == 1;
	int g3 = mode == 3;
	int grp = mode == 2 || g3;
	if (g3) {
		/* pattern-8 global window: save the cursor, jump to the top of the
		 * buffer so the register-cache search scans from offset 0 */
		sb_printf(out, "m %d", WIN_SAVE_MARK);
		EMIT_SEP(out);
		sb_str(out, "1;0");
		EMIT_SEP(out);
	}
	if (grp) {
		/* grp window: bracket the register-cache search with
		 * "grp 1 .. grp 0" so the find lands on the captured group */
		sb_str(out, "grp 1");
		EMIT_SEP(out);
	}
	if (single) {
		/* reset xoff to 0 so the .,$ region starts at the
		 * current line's first column */
		sb_str(out, ";0");
		EMIT_SEP(out);
		sb_str(out, "fr");
		EMIT_SEP(out);
		sb_str(out, first ? ".,\\$f> " : ".,\\$f+ ");
		/* pre-escaped (interactive) patterns carry their own ^...$
		 * from the displayed default; the user may have removed them */
		if (!pre_escaped)
			sb_chr(out, '^');
	} else
		/* a global window (g3) always searches forward from the reset top,
		 * so it forces f> regardless of whether it is the file's first search */
		sb_str(out, (g3 || first) ? "%f> " : "%f+ ");
	for (int i = 0; i < nanchors; i++) {
		if (pre_escaped) {
			char *e = escape_exarg(anchors[i]);
			emit_escaped_line(out, e);
			free(e);
		} else {
			char *r = escape_regex(anchors[i]);
			char *e = escape_exarg(r);
			emit_escaped_line(out, e);
			free(e);
			free(r);
		}
		if (i < nanchors - 1)
			sb_chr(out, '\n');
	}
	if (single && !pre_escaped)
		sb_str(out, "\\$");  /* $ anchor, shell-escaped */
	/* Ensure trailing newline when last anchor is empty */
	if (nanchors > 0 && !anchors[nanchors - 1][0])
		sb_chr(out, '\n');
	EMIT_SEP(out);
	emit_err_check(out, target_line);
	if (grp) {
		/* reset the search group. Must come AFTER the error check:
		 * grp 0 succeeds and would otherwise overwrite xpret, masking
		 * a failed f> search from the ??! check above. */
		sb_str(out, "grp 0");
		EMIT_SEP(out);
	}
	if (single) {
		sb_str(out, "fr 98");
		EMIT_SEP(out);
	}
	sb_str(out, "${LB}\n");
	EMIT_SEP(out);
	if (offset)
		sb_printf(out, "%+d", offset);
	sb_printf(out, "m %d", mark_id);
	EMIT_SEP(out);
	if (g3) {
		/* restore the cursor saved before the global search so the next
		 * group's incremental search continues from the same position */
		sb_printf(out, "'%d", WIN_SAVE_MARK);
		EMIT_SEP(out);
	}
}

/* Next mark id, skipping nextvi's internal special mark ids:
 * <'> 39 <*> 42 <[> 91 <]> 93 <`> 96 are rewritten by the editor
 * itself (<*> on every ex command, <[>/<]> on every change). */
/* Mark ids reserved per file by verbatim overrides (their blobs reference a
 * fixed id), so regenerated groups can't collide with them. */
static int *reserved_marks;
static int nreserved_marks, reserved_marks_cap;

static void reserve_mark(int id)
{
	if (nreserved_marks >= reserved_marks_cap) {
		reserved_marks_cap = reserved_marks_cap ? reserved_marks_cap * 2 : 8;
		reserved_marks = erealloc(reserved_marks,
					  reserved_marks_cap * sizeof(int));
	}
	reserved_marks[nreserved_marks++] = id;
}

static int mark_is_reserved(int id)
{
	for (int i = 0; i < nreserved_marks; i++)
		if (reserved_marks[i] == id)
			return 1;
	return 0;
}

static int next_mark_id(int *n)
{
	while (*n == '\'' || *n == '*' || *n == '[' || *n == ']' || *n == '`'
	       || mark_is_reserved(*n))
		(*n)++;
	return (*n)++;
}

typedef struct group_s {
	int del_start, del_end;  /* 0 if no deletes */
	char **add_texts;
	char **del_texts;        /* deleted line contents */
	int ndel;
	int nadd;
	int add_after;  /* line to add after (for pure adds) */
	/* For relative mode: */
	int anchor_offset;       /* lines from anchor to first change */
	char *anchors[3];        /* up to 3 consecutive preceding context lines */
	int nanchors;            /* count of anchor lines */
	char *follow_ctx;        /* first following context line */
	int follow_offset;       /* lines from first change to follow_ctx */
	/* For interactive mode (--ri): */
	char **all_pre_ctx;      /* all context lines before change */
	int nall_pre_ctx;
	char **post_ctx;         /* post-change context lines (up to 3) */
	int npost_ctx;
	int block_change_idx;    /* index of first del/change line in block */
	/* Edited SEARCH PATTERN 1-NSEARCH sections (pre-escaped regex) */
	char **custom_pat[NSEARCH];
	int ncustom_pat[NSEARCH];
	int custom_pat_off[NSEARCH];     /* per-section +N first-line override */
	int custom_pat_has_off[NSEARCH];
	int custom_pat_mode[NSEARCH];    /* per-section MODE override (0-3) */
	int custom_pat_has_mode[NSEARCH];
	int custom_offset;       /* offset from EDIT COMMAND +N (patterns 1-2) */
	/* Per-group strategy selection (interactive mode) */
	int strategy;            /* enum strategy */
	int has_line_diff;       /* whether find_line_diff() succeeded */
	char *ld_old_text;       /* expanded diff text for s// */
	char *ld_new_text;       /* expanded replacement text for s// */
	int ldc_start, ldc_end; /* minimal char positions for ;c */
	char *ldc_new_text;      /* minimal replacement text for ;c */
	/* Per-group custom edit commands from EDIT COMMAND sections.
	 * lines[0] = "cmd [inline-content]", lines[1..] = extra content lines.
	 * Content is raw text (NOT pre-escaped); escaping applied at emit time.
	 * Substitute format: lines[0] = "s/pat/repl/" (pre-escaped, exarg layer). */
	char **custom_abs_lines;
	char **custom_relc_lines;
	char **custom_rel_lines;
	int custom_abs_nlines;
	int custom_relc_nlines;
	int custom_rel_nlines;
	/* Enclosing @@ hunk's original-line span (1-based, 0 if unknown); used by
	 * gen_win_window to anchor strictly outside the diff's shown region. */
	int hunk_lo, hunk_hi;
	/* Two-phase emission state, set in phase 1, read in phase 2 */
	int res_strat;           /* resolved strategy */
	int mark_id;             /* line mark id, -1 = no mark */
	int insert_i;            /* pure add: insert before mark ('N-1i) vs after ('Ni) */
	/* Verbatim segments: generated bytes (gen_group_segments) and user
	 * overrides substituted for them at emit time; no trailing newline. */
	char *ph1_gen, *ph2_gen;
	char *ph1_ovr, *ph2_ovr;
	int ovr_mark;            /* forced mark id for override blobs */
	int ovr_esc;             /* escape regime the override was captured under */
} group_t;

/* Emit a line with exarg + shell escaping only (no regex escaping).
 * Used for user-edited regex lines in interactive mode. */
static void emit_escaped_exarg_only(sbuf *out, const char *s)
{
	char *e = escape_exarg(s);
	emit_escaped_line(out, e);
	free(e);
}

/* One fallback search pattern (phase 1) */
typedef struct {
	char **lines;
	int nlines;
	int pre_escaped;  /* 1 = user regex (exarg only), 0 = raw text */
	int offset;       /* lines from match start to the target line */
	int off_final;    /* 1 = offset from OFFSET marker, no adjustment */
	int mode;         /* search mode: 0 = %f> register, 1 = .,$f> buffer,
			   * 2 = grp register search (bracketed grp 1 .. grp 0),
			   * 3 = global grp straddle (cursor saved/reset/restored) */
	int pid;          /* fixed pattern id (source slot + 1, 1-9): emitted as
			   * the capture tag and OK1 anchor id so a failure maps
			   * to its real pattern regardless of which slots survived */
} pat_spec_t;

/* Default (non-edited) lines for fallback pattern pi, ordered strict to
 * loose (first match wins at apply time):
 *   0 = whole hunk: pre-ctx anchors + deleted lines + following ctx,
 *   1 = deleted lines + following ctx (bottom-anchored, no pre-ctx) -
 *       used when the pre-context is ambiguous but the trailing context
 *       disambiguates,
 *   2 = top context anchors only (the historical single pattern),
 *   3 = deleted lines only,
 *   4 = following ctx only (pure bottom anchor) - used when the whole
 *       pre-context/deleted region is volatile but the line after the
 *       hunk is a stable landmark.
 * Strategies 1 and 4 are deletion/change oriented (they need deleted
 * lines); for pure adds they return 0 and are dropped. Redundant slots
 * (e.g. no following context makes 1 == 3 and 4 empty) are dropped by
 * the caller's dedup.
 * raw[] receives borrowed pointers (3 + ndel + 3 entries max).
 * Returns the line count; *off = lines from match start to target. */
static int default_pat_lines(group_t *g, int pi, char **raw, int *off)
{
	int n = 0;
	int has_del = g->ndel > 0 && !(g->ndel == 1 && !g->del_texts[0][0]);
	int has_post = g->npost_ctx > 0 || (g->follow_ctx && g->follow_ctx[0]);
	*off = 0;
	if (pi == 3) {
		if (!has_del)
			return 0;
		for (int i = 0; i < g->ndel; i++)
			raw[n++] = g->del_texts[i];
		return n;
	}
	if (pi == 1) {
		/* deleted lines + following ctx; match starts on the first
		 * deleted line, which is the target (off = 0). Only distinct
		 * from strategy 3 when following context exists. */
		if (!has_del || !has_post)
			return 0;
		for (int i = 0; i < g->ndel; i++)
			raw[n++] = g->del_texts[i];
		if (g->npost_ctx > 0)
			for (int i = 0; i < g->npost_ctx; i++)
				raw[n++] = g->post_ctx[i];
		else
			raw[n++] = g->follow_ctx;
		return n;
	}
	if (pi == 4) {
		/* following ctx only; the post context sits g->ndel lines
		 * below the first deleted line (the target), since the
		 * search-time buffer holds the pre-edit content. */
		if (!has_del || !has_post)
			return 0;
		if (g->npost_ctx > 0)
			for (int i = 0; i < g->npost_ctx; i++)
				raw[n++] = g->post_ctx[i];
		else
			raw[n++] = g->follow_ctx;
		*off = -(g->ndel);
		return n;
	}
	if (pi == 2) {
		if (g->nanchors >= 2 ||
		    (g->nanchors == 1 && g->anchors[0] && g->anchors[0][0])) {
			for (int i = 0; i < g->nanchors; i++)
				raw[n++] = g->anchors[i];
			*off = g->nanchors - 1 + g->anchor_offset;
		} else if (g->follow_ctx && g->follow_ctx[0]) {
			raw[n++] = g->follow_ctx;
			*off = -(g->follow_offset);
		} else if (g->ndel > 0 && g->del_texts[0][0]) {
			raw[n++] = g->del_texts[0];
		}
		return n;
	}
	/* pi == 0: whole hunk */
	for (int i = 0; i < g->nanchors; i++)
		raw[n++] = g->anchors[i];
	int top = n;
	for (int i = 0; i < g->ndel; i++)
		raw[n++] = g->del_texts[i];
	if (g->npost_ctx > 0) {
		for (int i = 0; i < g->npost_ctx; i++)
			raw[n++] = g->post_ctx[i];
	} else if (g->follow_ctx) {
		raw[n++] = g->follow_ctx;
	}
	if (top)
		*off = g->nanchors - 1 + g->anchor_offset;
	else if (!g->ndel && n)
		*off = -(g->follow_offset);
	return n;
}

/* One file-validated fuzzed anchor window: pre-escaped regex lines plus the
 * offset/mode needed to emit it like an exact pattern. */
typedef struct {
	char **lines;   /* owned: nlines malloc'd regex strings */
	int nlines;
	int offset;     /* lines from match start to the target line */
	int mode;       /* 0 = %f>, 1 = .,$f>, 2 = grp, 3 = global straddle */
	int score;      /* ordering key (specificity + UNIQUE_BONUS) */
} fuzzwin_t;

/* Append file-validated window w to ps[nps] with pid; off_final preserves its
 * offset through the pure-add shift. Returns the new nps. */
static int push_win_pat(pat_spec_t *ps, int nps, fuzzwin_t *w, int pid,
			int off_final)
{
	ps[nps].lines = w->lines;
	ps[nps].nlines = w->nlines;
	ps[nps].pre_escaped = 1;
	ps[nps].offset = w->offset;
	ps[nps].off_final = off_final;
	ps[nps].mode = w->mode;
	ps[nps].pid = pid;
	return nps + 1;
}

/*
 * Generate up to max file-validated fuzzed (relaxed) anchor windows for group
 * g into out[]. Relaxes the whole-hunk window at increasing fuzz levels and
 * keeps each variant the original file proves still resolves to exactly one
 * place - the right one. Requires orig_lines loaded and the hunk pristine
 * (its deleted lines sit at their expected position on disk); otherwise none
 * are produced. Each out[i].lines is owned by the caller. Returns the count.
 */
static int gen_fuzz_windows(group_t *g, fuzzwin_t *out, int max)
{
	if (!orig_lines || max <= 0 || g->del_start <= 0 ||
	    !window_at(g->del_texts, g->ndel, g->del_start - 1))
		return 0;
	char **base = emalloc((g->ndel + 7) * sizeof(char *));
	int doff0;
	int bn = default_pat_lines(g, 0, base, &doff0);
	if (bn <= 0) {
		free(base);
		return 0;
	}
	int expected = (g->del_start - 1) - doff0;
	unsigned seed = 0;
	for (int i = 0; i < bn; i++)
		seed = hash_str(base[i], seed);
	fline_t *win = emalloc(bn * sizeof(*win));
	/* Collect every distinct file-validated variant, strictest (low fuzz
	 * level) first, then keep only the last `max` - the loosest ones. The
	 * level span is fixed (independent of max) so reducing how many we keep
	 * never narrows how loose we are willing to relax. */
	fuzzwin_t cand[FUZZ_MAXLVL + 1];
	int nc = 0;
	for (int lvl = 0; lvl <= FUZZ_MAXLVL; lvl++) {
		int any = 0, gi = 0, masked = 0, total = 0;
		for (int j = 0; j < bn; j++) {
			int nr = uc_slen(base[j]);
			unsigned char *m = emalloc(nr ? nr : 1);
			fuzz_mask(m, nr, lvl, seed, &gi);
			for (int k = 0; k < nr; k++)
				if (m[k])
					any = 1, masked++;
			total += nr;
			win[j].base = base[j];
			win[j].mask = m;
			win[j].nrune = nr;
		}
		/* Keep at least ~20% of runes literal: a window relaxed past
		 * four runes in five is too thin an anchor to trust, even if it
		 * still validates uniquely on this particular file. */
		int too_loose = total > 0 && masked * 5 > total * 4;
		int first, cnt = any && !too_loose
				 ? count_window_fuzzy(win, bn, &first) : 0;
		if (any && !too_loose && cnt == 1 && first == expected) {
			char **lines = emalloc(bn * sizeof(char *));
			for (int j = 0; j < bn; j++)
				lines[j] = fuzzy_regex(&win[j]);
			int dup = 0;
			for (int p = 0; p < nc; p++)
				if (lines_equal(lines, bn, cand[p].lines,
						cand[p].nlines)) {
					dup = 1;
					break;
				}
			if (dup) {
				for (int j = 0; j < bn; j++)
					free(lines[j]);
				free(lines);
			} else {
				cand[nc].lines = lines;
				cand[nc].nlines = bn;
				cand[nc].offset = doff0;
				cand[nc].mode = bn == 1 ? 1 : 0;
				cand[nc].score = fuzzy_spec(win, bn) + UNIQUE_BONUS;
				nc++;
			}
		}
		for (int j = 0; j < bn; j++)
			free(win[j].mask);
	}
	free(win);
	free(base);
	/* Keep the last `max` (loosest); free the stricter ones we drop. */
	int keep = nc < max ? nc : max;
	int drop = nc - keep;
	for (int i = 0; i < drop; i++) {
		for (int j = 0; j < cand[i].nlines; j++)
			free(cand[i].lines[j]);
		free(cand[i].lines);
	}
	for (int i = 0; i < keep; i++)
		out[i] = cand[drop + i];
	return keep;
}

static void free_fuzz_windows(fuzzwin_t *w, int n)
{
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < w[i].nlines; j++)
			free(w[i].lines[j]);
		free(w[i].lines);
	}
}

/*
 * Pattern 7: a :grp-capture window (mode 2). The top of the hunk - preceding
 * context anchors plus the first deleted line on change/delete - becomes
 * "TEXT.*?" line by line, the final line captured "(TEXT)". A ":grp 1" search
 * lands on that captured line; the trailing non-greedy ".*?" on leading lines
 * absorbs text added after the anchors (inserted token, widened line) without
 * shifting the target. Unanchored, so no leading ".*".
 *
 * Voided (returns 0) when degenerate: fewer than two lines (a bare "(text)" just
 * duplicates the exact single-line strategies), or an empty captured last line
 * (zero-width "()" resolves anywhere).
 *
 * The captured last line IS the target at offset 0: change/delete captures the
 * first deleted line (edited in place), pure insert captures the last anchor
 * (phase-2 "'Ni" appends after it). File-validated like the fuzzed windows:
 * emitted only when the wrapped window resolves uniquely to the expected place.
 * Returns 1, fills *out (owned lines), else 0.
 */
static int gen_grp_window(group_t *g, fuzzwin_t *out)
{
	if (!orig_lines || g->nanchors < 1 || !g->anchors[g->nanchors - 1])
		return 0;
	int has_del = g->ndel > 0 && !(g->ndel == 1 && !g->del_texts[0][0]);
	int n = g->nanchors + (has_del ? 1 : 0);
	char **raw = emalloc(n * sizeof(char *));
	for (int i = 0; i < g->nanchors; i++)
		raw[i] = g->anchors[i];
	if (has_del)
		raw[g->nanchors] = g->del_texts[0];
	/* The grp window only earns its slot when it has at least one leading
	 * ".*?" anchor to absorb interior drift; a bare "(text)" is just a
	 * redundant single-line search the exact strategies already cover. An
	 * empty captured last line would emit "()" - a zero-width grp match that
	 * resolves anywhere - so reject that too. */
	if (n < 2 || !raw[n - 1][0]) {
		free(raw);
		return 0;
	}
	/* The captured last line must land on the target: change/delete -> the
	 * first deleted line at del_start-1; pure insert -> the last anchor at
	 * add_after-1. The window starts n-1 lines above it. */
	int last = has_del ? g->del_start - 1 : g->add_after - 1;
	int first, cnt;
	if (last < 0 || last - (n - 1) < 0) {
		free(raw);
		return 0;
	}
	cnt = count_window_substr(raw, n, &first);
	if (cnt != 1 || first != last - (n - 1)) {
		free(raw);
		return 0;
	}
	int sc = specificity_score(raw, n);
	char **lines = emalloc(n * sizeof(char *));
	for (int i = 0; i < n; i++) {
		char *e = escape_regex(raw[i]);
		int cap = i == n - 1;
		/* The search is unanchored, so a leading ".*" is redundant; each
		 * non-final line takes a trailing non-greedy ".*?" to absorb text
		 * added after the anchor without over-consuming, and the captured
		 * last line needs nothing extra. */
		int len = strlen(e) + 5;   /* "(" + ")" or ".*?", + NUL */
		char *s = emalloc(len);
		snprintf(s, len, cap ? "(%s)" : "%s.*?", e);
		lines[i] = s;
		free(e);
	}
	free(raw);
	out->lines = lines;
	out->nlines = n;
	/* The mark sits on the captured last line (offset 0) in both shapes:
	 * change/delete edits at del_start, and a pure insert's phase-2 "'Ni"
	 * already appends after the marked last anchor, so no +1 is needed. */
	out->offset = 0;
	out->mode = 2;   /* grp register search */
	out->score = sc;
	return 1;
}

/* How far above/below a hunk gen_win_window will look for a unique anchor block
 * before giving up. Bounds the O(scan * file) validation cost per group. */
#define WIN_SCAN 200

/* Lines per straddle anchor block: each side of the window is a block of this
 * many consecutive non-empty original lines (a 3-line block is far more
 * discriminating than a single line, so the global search false-matches less). */
#define WIN_ANCHOR 3

/* True if orig_lines[s .. s+WIN_ANCHOR) are all in-range and non-empty: the
 * precondition for using that block as a straddle anchor. */
static int anchor_block_at(int s)
{
	if (s < 0 || s + WIN_ANCHOR > n_orig_lines)
		return 0;
	for (int j = 0; j < WIN_ANCHOR; j++)
		if (!orig_lines[s + j][0])
			return 0;
	return 1;
}

/*
 * Pattern 8: global "top.*(bottom)" straddle window (mode 3). Both anchors lie
 * OUTSIDE the diff's shown region: the nearest unique WIN_ANCHOR-line block above
 * the enclosing @@ hunk (top) and below it (bottom). These lines exist only in
 * the original, never in the diff, so this requires reading the original. Regex
 * "t1\nt2\nt3.*(b1)\nb2\nb3": each block's lines newline-joined (consecutive-line
 * match), one greedy ".*" between them absorbing the whole hunk (in multi-line
 * mode "." spans newlines). Only b1 is captured, so ":grp 1" lands on it and the
 * target is a negative offset back up. The search runs globally from the file top
 * (emit brackets it with mark-0 save / "1;0" reset / "'0" restore).
 *
 * Pattern 9 reuses this with skip=1: skip the first qualifying block on each side
 * (advancing a WHOLE block so the windows stay disjoint), giving a wider, looser
 * straddle that sits last in the chain. skip=0 reproduces pattern 8.
 *
 * File-validated (original readable and pristine): top block unique; bottom block
 * unique AND its captured first line carries its text nowhere to EOF, so greedy
 * ".*" lands on exactly it. Change/delete marks the first deleted line; pure
 * insert marks add_after (phase-2 "'Ni" appends after it). Returns 1, fills *out
 * (owned lines), else 0.
 */
static int gen_win_window(group_t *g, fuzzwin_t *out, int skip)
{
	if (!orig_lines)
		return 0;
	/* 0-based line the mark must land on (the target). For a change/delete it is
	 * the first deleted line; for a pure insert it is the existing line the new
	 * text is appended after. Pristine: the deleted lines must still be present
	 * (change/delete), or the added lines must NOT yet be present and the
	 * insertion boundary must match the original (pure insert) - else the file is
	 * not the pre-patch original and the offset would be wrong. */
	int hunk_top;
	if (g->del_start > 0) {
		if (!window_at(g->del_texts, g->ndel, g->del_start - 1))
			return 0;
		hunk_top = g->del_start - 1;
	} else {
		if (g->nadd <= 0 || g->add_after < 1 || g->add_after > n_orig_lines ||
		    g->nanchors < 1 ||
		    strcmp(orig_lines[g->add_after - 1], g->anchors[g->nanchors - 1]) != 0 ||
		    window_at(g->add_texts, g->nadd, g->add_after))
			return 0;
		hunk_top = g->add_after - 1;
	}
	/* The anchors must lie OUTSIDE the diff's shown region, not on its context
	 * lines (those are exactly what may drift and what the other strategies
	 * already key on). Skip past the whole enclosing @@ hunk - including all its
	 * shown context - so top/bottom come only from the original file beyond it.
	 * Fall back to the deleted range if the span is unknown. */
	int span_lo = g->hunk_lo > 0 ? g->hunk_lo - 1 : hunk_top;
	int span_hi = g->hunk_hi > 0 ? g->hunk_hi - 1
		      : g->del_end > 0 ? g->del_end - 1
		      : hunk_top;
	if (span_lo > hunk_top)
		span_lo = hunk_top;
	if (span_hi < hunk_top)
		span_hi = hunk_top;
	/* nearest unique non-empty WIN_ANCHOR-line block ending strictly above the
	 * hunk's shown region; skip past the first `skip` qualifying blocks for a
	 * farther anchor. `it` is the block start (top line). When skipping, advance a
	 * WHOLE block (s -= WIN_ANCHOR - 1, plus the loop's own s--) so pattern 9's
	 * block does not overlap pattern 8's - they must be disjoint, not shifted by
	 * one line. */
	int it = -1, first, seen = 0;
	for (int s = span_lo - WIN_ANCHOR, d = 0; s >= 0 && d < WIN_SCAN; s--, d++) {
		if (!anchor_block_at(s))
			continue;
		if (count_window(&orig_lines[s], WIN_ANCHOR, &first) == 1) {
			if (seen++ < skip) {
				s -= WIN_ANCHOR - 1;
				continue;
			}
			it = s;
			break;
		}
	}
	/* nearest unique non-empty WIN_ANCHOR-line block starting strictly below the
	 * hunk's shown region, with its captured first line unambiguous as a substring
	 * from that line to EOF (so greedy ".*" lands on it); skip past the first
	 * `skip` qualifying blocks for a farther anchor (advancing a whole block so the
	 * windows stay disjoint). `ib` is the captured line. */
	int ib = -1;
	seen = 0;
	for (int s = span_hi + 1, d = 0; s + WIN_ANCHOR <= n_orig_lines && d < WIN_SCAN;
	     s++, d++) {
		if (!anchor_block_at(s))
			continue;
		if (count_window(&orig_lines[s], WIN_ANCHOR, &first) == 1 &&
		    count_substr_range(orig_lines[s], s + 1, n_orig_lines - 1) == 0) {
			if (seen++ < skip) {
				s += WIN_ANCHOR - 1;
				continue;
			}
			ib = s;
			break;
		}
	}
	if (it < 0 || ib < 0)
		return 0;
	/* Build "t1\nt2\nt3.*(b1)\nb2\nb3": each block's lines are joined with a
	 * literal newline (consecutive-line match, in multi-line search mode), so the
	 * blocks only STRENGTHEN the anchoring - there is exactly ONE ".*", the single
	 * gap that absorbs the hunk between the two blocks. Only the first bottom line
	 * is captured "(b1)" so grp lands on it and the offset reference stays at ib. */
	char *e;
	sbuf_smake(sb, 256)
	for (int j = 0; j < WIN_ANCHOR; j++) {           /* t1\nt2\nt3 */
		e = escape_regex(orig_lines[it + j]);
		if (j)
			sbuf_chr(sb, '\n')
		sbuf_str(sb, e)
		free(e);
	}
	for (int j = 0; j < WIN_ANCHOR; j++) {           /* .*(b1)\nb2\nb3 */
		e = escape_regex(orig_lines[ib + j]);
		if (j) {
			sbuf_chr(sb, '\n')
			sbuf_str(sb, e)
		} else {
			sbuf_str(sb, ".*(")               /* one ".*", capture b1 */
			sbuf_str(sb, e)
			sbuf_chr(sb, ')')
		}
		free(e);
	}
	sbufn_null(sb)
	char **lines = emalloc(sizeof(char *));
	lines[0] = sb->s;
	out->lines = lines;
	out->nlines = 1;
	out->offset = hunk_top - ib;   /* negative: target sits above the bottom anchor */
	out->mode = 3;                 /* global grp straddle window */
	/* Loosest of all strategies (content-blind inside the hunk): score 0 so the
	 * stable sort keeps it last in the fallback chain. */
	out->score = 0;
	return 1;
}

/* Emit one fallback pattern as the f> argument inside a ? conditional.
 * The conditional nesting consumes one more escape layer than a
 * top-level search: with the default backslash escape, every backslash
 * is doubled again for the extra ex_arg layer. The ? conditional no
 * longer delimiter-scans its argument, so literal/quantifier ? pass
 * through untouched. With a dynamic escape, backslash is not special to
 * ex_arg, so the regex needs no extra escaping at all. */
static void emit_chain_pattern(sbuf *out, pat_spec_t *p)
{
	int wrap = p->nlines == 1 && !p->pre_escaped;
	if (wrap)
		sb_chr(out, '^');
	for (int i = 0; i < p->nlines; i++) {
		char *r = p->pre_escaped ? NULL : escape_regex(p->lines[i]);
		char *x;
		if (dyn_esc) {
			x = uc_dup(r ? r : p->lines[i]);
		} else {
			char *e = escape_exarg(r ? r : p->lines[i]);
			x = escape_chars(e, "\\");
			free(e);
		}
		emit_escaped_line(out, x);
		free(x);
		free(r);
		if (i < p->nlines - 1)
			sb_chr(out, '\n');
	}
	if (wrap)
		sb_str(out, "\\$");  /* $ anchor, shell-escaped */
	/* Ensure trailing newline when last line is empty */
	if (p->nlines > 0 && !p->lines[p->nlines - 1][0])
		sb_chr(out, '\n');
}

/* Emit a compat block's gate: search the probe, capture the result under the
 * gate's own tag, and on the polarity's failing side quit before any edit.
 *
 *   ?<esc><sep>f> <probe><esc><sep><tag>??<esc><sep><tag>??!<esc3><sep>q!0<sep>
 *
 * With the register cache active (fr 98) a bare "f> " has no location, so
 * ec_find takes its existence-test branch and the cursor never moves - the
 * groups that follow search from exactly where they would have without a
 * gate. Without the cache the search must be located ("%f> ") and the cursor
 * is put back with "1;0". A mode-1 probe searches the buffer directly, from
 * the top, and restores the cache afterwards.
 *
 * The quit is q!0, never a counted Nq: ex_exec drops one xqprop level per
 * conditional arm, so a counted quit inside the ?? arm would be absorbed and
 * the block would apply unconditionally - a silent failure in the dangerous
 * direction. q!0 sets xquit = -1, unwinds regardless of depth, skips the
 * trailing writes and exits 0, so "sh -e" carries on to the next block. */
static void emit_gate(sbuf *out, gate_t *g, int use_cache)
{
	pat_spec_t ps;
	if (g->polarity == GATE_ALWAYS || g->nlines <= 0)
		return;
	memset(&ps, 0, sizeof(ps));
	ps.lines = g->lines;
	ps.nlines = g->nlines;
	ps.pre_escaped = g->pre_escaped;
	ps.mode = g->mode;
	sb_chr(out, '?');
	EMIT_ESCSEP(out);
	sb_str(out, "${LB}\n");
	EMIT_ESCSEP(out);
	if (g->mode == 1) {
		/* search the live buffer from the top: ";0" resets xoff, "fr"
		 * drops the register so f> reads the buffer itself */
		sb_str(out, "1;0");
		EMIT_ESCSEP(out);
		sb_str(out, "fr");
		EMIT_ESCSEP(out);
		sb_str(out, ".,\\$f> ");
	} else
		sb_str(out, use_cache ? "f> " : "%f> ");
	emit_chain_pattern(out, &ps);
	EMIT_ESCSEP(out);
	sb_printf(out, "%d??", g->tag);
	if (g->mode == 1) {
		if (use_cache) {
			EMIT_ESCSEP(out);
			sb_str(out, "fr 98");
		}
		EMIT_ESCSEP(out);
		sb_str(out, "1;0");
	} else if (!use_cache) {
		/* the located search moved the cursor; the groups below expect
		 * to start at the top of the file */
		EMIT_ESCSEP(out);
		sb_str(out, "1;0");
	}
	EMIT_ESCSEP(out);
	sb_printf(out, "%d??%s", g->tag,
		  g->polarity == GATE_PRESENT ? "!" : "");
	/* the quit sits inside the tag's then-arg, one level deeper */
	EMIT_ESC3SEP(out);
	sb_str(out, "q!0");
	EMIT_SEP(out);
	sb_str(out, "${LB}\n");
	EMIT_SEP(out);
}

/* Phase 1 fallback chain: try each pattern in order, first match wins.
 * All attempts are nested into a single ? conditional, chained with
 * escaped separators; per pattern n (capture tag n):
 *   %f> <pat>\:<n>??\:<n>??[+off]m <id>\\\:${OK1}p OK <loc>:a<n>\\\:1q\:
 * (the ${OK1} success report only on fallback blocks, n >= 1)
 * The search's error status is captured into tag <n>; on success the
 * <n>?? branch marks the target and 1q short-circuits out of the
 * block, skipping the remaining attempts and the check. After the
 * last block (no 1q) a single <0;1;..>??! DNF check over all tags
 * reports the failure.
 * A mode-1 pattern (single-line by default) searches the live buffer
 * with ";0\:fr\:.,$f> ^pat$" instead, restoring the register cache
 * with fr 98 on both the success (before 1q) and no-match paths.
 * A mode-2 pattern (the pattern-7 grp window) is a register-cache search
 * bracketed with "grp 1\:...\:grp 0" so the find lands on the captured group,
 * then resets the search group.
 * A mode-3 pattern (the pattern-8 global straddle window) is a grp search that
 * first saves the cursor ("m <WIN_SAVE_MARK>") and resets to the top ("1;0") so
 * the search runs globally, then restores the cursor ("'<WIN_SAVE_MARK>")
 * unconditionally before the success-gated 1q. */
static void emit_fallback_chain(sbuf *out, pat_spec_t *ps, int nps,
				int mark_id, int target_line, int first)
{
	int pids[NSEARCH];
	sb_chr(out, '?');
	for (int n = 0; n < nps; n++) {
		int m1 = ps[n].mode == 1;
		int g3 = ps[n].mode == 3;
		int g2 = ps[n].mode == 2 || g3;   /* grp bracketing covers both */
		/* Readability line break before each attempt's search: a leading
		 * separator (after the '?' for the first attempt, after the
		 * previous block otherwise), a ${LB} no-op clause and a real
		 * newline, then the separator before the search setup. Every
		 * attempt thus starts on its own source line. */
		EMIT_ESCSEP(out);
		sb_str(out, "${LB}\n");
		EMIT_ESCSEP(out);
		if (g3) {
			/* pattern-8 global window: save the cursor and reset to
			 * the top so the register-cache search scans from offset 0 */
			sb_printf(out, "m %d", WIN_SAVE_MARK);
			EMIT_ESCSEP(out);
			sb_str(out, "1;0");
			EMIT_ESCSEP(out);
		}
		if (g2) {
			sb_str(out, "grp 1");
			EMIT_ESCSEP(out);
		}
		if (m1) {
			/* Mode 1: search the live buffer directly. ";0"
			 * resets xoff, "fr" clears the find register so
			 * f> reads the buffer (not the cache); the matching
			 * fr 98 below restores the cache for later blocks. */
			sb_str(out, ";0");
			EMIT_ESCSEP(out);
			sb_str(out, "fr");
			EMIT_ESCSEP(out);
			sb_str(out, first ? ".,\\$f> " : ".,\\$f+ ");
		} else
			/* g3 always searches forward from the reset top */
			sb_str(out, (g3 || first) ? "%f> " : "%f+ ");
		emit_chain_pattern(out, &ps[n]);
		EMIT_ESCSEP(out);
		sb_printf(out, "%d??", ps[n].pid);
		/* Readability line break once the search result is captured
		 * into tag <n>: a ${LB} (no-op) clause and a real newline split
		 * the long single-line chain so each attempt's match and its
		 * mark action sit on separate source lines. Placed after the
		 * tag capture (and before grp 0 / the action re-test) so it
		 * never separates a tag test from its then-arm. */
		EMIT_ESCSEP(out);
		sb_str(out, "${LB}\n");
		if (g2) {
			/* reset the search group on both match and no-match
			 * paths. Must come AFTER the <n>?? tag capture above,
			 * otherwise the tag records grp 0's (always-success)
			 * status instead of the f> search's. */
			EMIT_ESCSEP(out);
			sb_str(out, "grp 0");
		}
		EMIT_ESCSEP(out);
		sb_printf(out, "%d??", ps[n].pid);
		if (ps[n].offset)
			sb_printf(out, "%+d", ps[n].offset);
		sb_printf(out, "m %d", mark_id);
		/* fallback (non-primary) match: with DBG1=1, OK1 expands
		 * empty and reports which anchor resolved the group */
		if (n) {
			EMIT_ESC3SEP(out);
			sb_printf(out, "${OK1}p OK %s:%d:a%d",
				  cur_file_path ? cur_file_path : "?",
				  target_line, ps[n].pid);
		}
		if (m1) {
			/* restore the register cache on the success path,
			 * before 1q quits out of the chain */
			EMIT_ESC3SEP(out);
			sb_str(out, "fr 98");
		}
		if (g3) {
			/* restore the saved cursor unconditionally (both match
			 * and no-match), at chain level so it runs before any
			 * short-circuit; this undoes the "1;0" reset above */
			EMIT_ESCSEP(out);
			sb_printf(out, "'%d", WIN_SAVE_MARK);
		}
		if (n < nps - 1) {
			if (g3) {
				/* the unconditional restore split the then-arg, so
				 * re-test the tag to keep 1q success-gated */
				EMIT_ESCSEP(out);
				sb_printf(out, "%d??", ps[n].pid);
				EMIT_ESC3SEP(out);
				sb_str(out, "1q");
			} else {
				/* 1q sits inside the <n>?? then-arg, one level
				 * deeper, so its separator needs three escapes */
				EMIT_ESC3SEP(out);
				sb_str(out, "1q");
			}
		}
		if (m1) {
			/* restore the cache on the no-match fall-through */
			EMIT_ESCSEP(out);
			sb_str(out, "fr 98");
		}
	}
	EMIT_SEP(out);
	sb_str(out, "${LB}\n");
	EMIT_SEP(out);
	for (int n = 0; n < nps; n++)
		pids[n] = ps[n].pid;
	emit_err_check_pats(out, pids, nps, target_line);
	sb_str(out, "${LB}\n");
	EMIT_SEP(out);
}

/* Phase 2: delete at a mark */
static void emit_mark_delete(sbuf *out, int line, int mark_id, int count)
{
	if (count == 1)
		sb_printf(out, "'%dd", mark_id);
	else
		sb_printf(out, "'%d,#+%dd", mark_id, count - 1);
	EMIT_SEP(out);
	emit_err_check_mark(out, line, mark_id);
}

/* Phase 2: insert at a mark ("'Ni" after the mark, "'N-1i" before it).
 * mark_id < 0 means a new file's empty buffer: no line to mark,
 * so the insert is emitted bare. */
static void emit_mark_insert(sbuf *out, int line, int mark_id, int use_i,
			     char **texts, int ntexts)
{
	if (ntexts == 0)
		return;
	if (mark_id < 0)
		sb_str(out, "i ");
	else if (use_i)
		sb_printf(out, "'%d-1i ", mark_id);
	else
		sb_printf(out, "'%di ", mark_id);
	emit_content(out, texts, ntexts);
	EMIT_SEP(out);
	emit_err_check_mark(out, line, mark_id);
}

/* Phase 2: change lines at a mark */
static void emit_mark_change(sbuf *out, int line, int mark_id,
			     int del_count, char **texts, int ntexts)
{
	if (ntexts == 0) {
		emit_mark_delete(out, line, mark_id, del_count);
		return;
	}
	if (del_count == 1)
		sb_printf(out, "'%dc ", mark_id);
	else
		sb_printf(out, "'%d,#+%dc ", mark_id, del_count - 1);
	emit_content(out, texts, ntexts);
	EMIT_SEP(out);
	emit_err_check_mark(out, line, mark_id);
}

/* A trailing run of backslashes sitting immediately before the closing
 * delimiter is halved by nextvi's ex_re_read parity rule (commit d94cd92):
 * a run of n escapes before the delim emits ceil(n/2). Our escapers
 * already doubled each literal backslash (k literals -> 2k here), but
 * ex_re_read would then halve 2k back to k, leaving a dangling escape. So
 * double the trailing run once more (-> 4k) so ex_re_read restores the
 * intended 2k. Only the run adjacent to the delimiter is affected; an
 * interior escape is followed by an ordinary char and passes through
 * unchanged, so this must not touch non-trailing backslashes. */
static char *double_trailing_esc(char *s)
{
	int len = strlen(s), t = 0;
	while (t < len && s[len - 1 - t] == '\\')
		t++;
	if (!t)
		return s;
	sbuf_smake(sb, len + t + 1)
	sbuf_mem(sb, s, len)
	sbuf_set(sb, '\\', t)
	free(s);
	sbufn_ret(sb, sb->s)
}

/* Escape replacement text for substitute command.
 * In nextvi :s replacement, only \ is special (for backreferences \0-\9).
 * Delimiter must also be escaped. delim is always '/' in current callers. */
/* Raw escapers omit double_trailing_esc so segments can be concatenated with
 * raw regex (groups, backrefs) before the trailing-run fixup is applied once to
 * the assembled string. The _raw form is correct for interior segments; the
 * non-raw wrappers are for a whole standalone field. */
static char *escape_sub_repl_raw(const char *s, char delim)
{
	char set[3] = { '\\', delim, 0 };
	return escape_chars(s, set);
}

static char *escape_sub_repl(const char *s, char delim)
{
	return double_trailing_esc(escape_sub_repl_raw(s, delim));
}

/* Escape regex pattern for substitute command.
 * Like escape_regex() but also escapes the delimiter for ex_re_read. */
static char *escape_sub_pat_raw(const char *s, char delim)
{
	char set[sizeof(REGEX_META) + 1];
	snprintf(set, sizeof(set), "%s%c", REGEX_META, delim);
	return escape_chars(s, set);
}

static char *escape_sub_pat(const char *s, char delim)
{
	return double_trailing_esc(escape_sub_pat_raw(s, delim));
}

/* Allocate a NUL-terminated copy of n bytes from s. */
static char *dup_n(const char *s, int n)
{
	char *r = emalloc(n + 1);
	memcpy(r, s, n);
	r[n] = '\0';
	return r;
}

/* Longest common substring of a[0..alen) and b[0..blen). Returns its byte
 * length and sets ai, bi to the start offsets in a/b. Plain O(alen*blen) DP;
 * diff lines are short. */
static int lcs_substr(const char *a, int alen, const char *b, int blen,
		      int *ai, int *bi)
{
	int best = 0;
	*ai = 0;
	*bi = 0;
	int row = blen + 1;
	int *prev = emalloc(row * sizeof(int));
	int *cur = emalloc(row * sizeof(int));
	memset(prev, 0, row * sizeof(int));
	for (int i = 1; i <= alen; i++) {
		cur[0] = 0;
		for (int j = 1; j <= blen; j++) {
			if (a[i - 1] == b[j - 1]) {
				cur[j] = prev[j - 1] + 1;
				if (cur[j] > best) {
					best = cur[j];
					*ai = i - cur[j];
					*bi = j - cur[j];
				}
			} else {
				cur[j] = 0;
			}
		}
		int *t = prev;
		prev = cur;
		cur = t;
	}
	free(prev);
	free(cur);
	return best;
}

/* A common block: identical run om[oa..oa+len) == nm[na..na+len), trimmed to
 * UTF-8 rune boundaries. `keep` marks it as a captured "(.*)" island. */
typedef struct {
	int oa, na, len;
	int keep;
} block_t;

typedef struct {
	block_t *v;
	int n, cap;
} blockvec_t;

static void bv_add(blockvec_t *bv, int oa, int na, int len)
{
	if (bv->n == bv->cap) {
		bv->cap = bv->cap ? bv->cap * 2 : 8;
		bv->v = erealloc(bv->v, bv->cap * sizeof(block_t));
	}
	bv->v[bv->n].oa = oa;
	bv->v[bv->n].na = na;
	bv->v[bv->n].len = len;
	bv->v[bv->n].keep = 0;
	bv->n++;
}

/* Recursively decompose om[os..oe)/nm[ns..ne) into in-order common blocks
 * (difflib-style: longest common substring, then recurse on the two flanks).
 * Each block is rune-trimmed; the trimmed-off edges and gaps fall through as
 * changed text. */
static void collect_blocks(const char *om, int os, int oe,
			   const char *nm, int ns, int ne, blockvec_t *bv)
{
	int alen = oe - os, blen = ne - ns;
	if (alen <= 0 || blen <= 0)
		return;
	int ai, bi;
	int L = lcs_substr(om + os, alen, nm + ns, blen, &ai, &bi);
	if (L <= 0)
		return;
	int bo = os + ai, bn = ns + bi;
	int s = 0, e = L;
	while (s < e && (om[bo + s] & 0xC0) == 0x80)
		s++;
	while (e > s && (om[bo + e] & 0xC0) == 0x80)
		e--;
	bo += s;
	bn += s;
	L = e - s;
	if (L <= 0)
		return;   /* whole block was a partial rune; treat as change */
	collect_blocks(om, os, bo, nm, ns, bn, bv);
	bv_add(bv, bo, bn, L);
	collect_blocks(om, bo + L, oe, nm, bn + L, ne, bv);
}

/* Build the exact (rung 0) substitute: minimal-span old/new fully escaped
 * (regex+delim, repl+delim, trailing-esc). This is the primary form, unchanged
 * from the original single-shot substitute. */
static void build_exact_sub(const char *old, const char *new,
			    char **pat_out, char **repl_out)
{
	*pat_out = escape_sub_pat(old, '/');
	*repl_out = escape_sub_repl(new, '/');
}

#define GRP_MIN_ISLAND 3   /* stable run must be >= this many runes to anchor a group */

/* Render mode for a stable run (capture group). */
enum { GM_LIT, GM_WILD, GM_FUZZ };   /* "(text)" / "(.*)" / "(head.*tail)" */

/* One token of the grp decomposition: a stable common run (a capture group) or
 * an edit (changed text matched literally on the old side, re-emitted on the
 * new side). Texts are borrowed slices of old/new. */
typedef struct {
	int stable;          /* 1 = stable anchor (group), 0 = edit */
	const char *o;
	int olen;   /* old text (pattern side) */
	const char *n;
	int nlen;   /* new text (replacement side) */
	int mode;            /* (stable) GM_LIT / GM_WILD / GM_FUZZ */
	int hb, tb;          /* (GM_FUZZ) head/tail byte lengths within o */
} gtok_t;

/* Byte length of the first k runes of [s,len] (clamped to len). */
static int rune_take(const char *s, int len, int k)
{
	int i = 0, n = 0;
	while (i < len && n < k) {
		i += uc_len((s + i));
		n++;
	}
	return i < len ? i : len;
}

/* Number of (overlapping) occurrences of needle in haystack. */
static int str_count_occ(const char *hay, int hl, const char *ndl, int nl)
{
	int c = 0;
	if (nl <= 0 || nl > hl)
		return 0;
	for (int i = 0; i + nl <= hl; i++)
		if (memcmp(hay + i, ndl, nl) == 0)
			c++;
	return c;
}

/*
 * For a stable run [o,olen] (a slice of the whole old line [old,oldlen]), pick
 * the MINIMAL head and tail (in runes) that each occur exactly once in old, so
 * the pattern "(head.*tail)" matches the run deterministically: the leading
 * "(.*)" can only end at the sole head position, and the middle ".*" can only
 * reach the sole tail. Minimal anchors maximize the wildcarded interior. Needs
 * non-overlapping head/tail leaving >= 1 rune of middle. Returns 1 with byte
 * lengths written to hb and tb, else 0 (caller falls back to a literal capture).
 */
static int fuzz_anchors(const char *old, int oldlen,
			const char *o, int olen, int *hb, int *tb)
{
	int R = rune_count_n(o, olen);
	int hk = 0, tk = 0, hbytes = 0, tbytes = 0;
	for (int k = 1; k <= R; k++) {
		hbytes = rune_take(o, olen, k);
		if (str_count_occ(old, oldlen, o, hbytes) == 1) {
			hk = k;
			break;
		}
	}
	if (!hk)
		return 0;
	for (int k = 1; k <= R; k++) {
		int off = rune_take(o, olen, R - k);
		tbytes = olen - off;
		if (str_count_occ(old, oldlen, o + off, tbytes) == 1) {
			tk = k;
			break;
		}
	}
	if (!tk || hk + tk >= R)   /* overlap or no interior left to absorb */
		return 0;
	*hb = hbytes;
	*tb = tbytes;
	return 1;
}

/*
 * Grp-capture absorbing substitute (rung 1 of the progression).
 *
 * Decompose the changed line into stable common runs and the edits between
 * them, then build a pattern over the EXACT SPAN ONLY -- from the first edit to
 * the last edit. The stable runs OUTSIDE that span (the unchanged line prefix
 * and suffix) are dropped: the substitute matches as an unanchored substring,
 * so prefix/suffix are already free, and wrapping them in leading/trailing
 * "(.*)" would just duplicate the exact rung (s/old/new/ over the same span).
 *
 * Each in-span stable run becomes a capture group; each edit is matched
 * literally (old text) and re-emitted (new text). A stable run is wildcarded so
 * it absorbs drift *inside* itself:
 *   - full "(.*)" when flanked by non-empty edits whose old-texts are each
 *     UNIQUE in the old line (the literal separators pin the greedy boundaries),
 *     e.g. two-spot "X bbbb Y" -> "P bbbb Q": s/X(.*)Y/P\1Q/.
 *   - else "(head.*tail)" keeping the MINIMAL head/tail runes that are each
 *     unique in the old line (see fuzz_anchors) -- used when a separator is an
 *     insertion (empty old) or repeats, where a bare "(.*)" would be ambiguous.
 *   - else a literal "(text)" capture (no unique anchors / no interior left).
 *
 * The variant is emitted only if at least one in-span run is wildcarded
 * ("(.*)" or "(head.*tail)"); otherwise it reproduces the span verbatim and is
 * a pure dup of the exact rung, so it returns 0.
 *
 * Returns 1 and sets pre-escaped pat/repl (sub layer + trailing fixup), else 0.
 */
static int build_grp_variant(const char *old, const char *new,
			     char **pat_out, char **repl_out)
{
	*pat_out = NULL;
	*repl_out = NULL;
	int olen = strlen(old), nlen = strlen(new);
	blockvec_t bv = {0};
	collect_blocks(old, 0, olen, new, 0, nlen, &bv);

	/* Token stream: edits and substantial stable runs. Small common blocks
	 * are folded into the surrounding edit (pos not advanced past them). */
	gtok_t *tk = emalloc((bv.n * 2 + 2) * sizeof(gtok_t));
	int nt = 0, pos_o = 0, pos_n = 0;
	for (int i = 0; i < bv.n; i++) {
		block_t *b = &bv.v[i];
		/* Fold small INTERIOR common runs into the surrounding edit; keep the
		 * boundary runs (leftmost/rightmost) regardless of size, since they
		 * become "(.*)" absorbers where literal length is irrelevant. Folding
		 * a short trailing/leading anchor would strand an insertion against a
		 * wildcard and force the whole variant to be rejected. */
		int boundary = (i == 0 || i == bv.n - 1);
		if (!boundary && rune_count_n(old + b->oa, b->len) < GRP_MIN_ISLAND)
			continue;
		if (b->oa > pos_o || b->na > pos_n) {   /* edit gap before anchor */
			tk[nt].stable = 0;
			tk[nt].o = old + pos_o;
			tk[nt].olen = b->oa - pos_o;
			tk[nt].n = new + pos_n;
			tk[nt].nlen = b->na - pos_n;
			nt++;
		}
		tk[nt].stable = 1;
		tk[nt].o = old + b->oa;
		tk[nt].olen = b->len;
		tk[nt].n = new + b->na;
		tk[nt].nlen = b->len;
		nt++;
		pos_o = b->oa + b->len;
		pos_n = b->na + b->len;
	}
	if (olen > pos_o || nlen > pos_n) {   /* trailing edit */
		tk[nt].stable = 0;
		tk[nt].o = old + pos_o;
		tk[nt].olen = olen - pos_o;
		tk[nt].n = new + pos_n;
		tk[nt].nlen = nlen - pos_n;
		nt++;
	}
	free(bv.v);

	/* The exact span runs from the first edit to the last edit; only the
	 * stable runs strictly inside it are emitted (the rest is the unchanged
	 * prefix/suffix the substring match already skips). */
	int fe = -1, le = -1;
	for (int i = 0; i < nt; i++)
		if (!tk[i].stable) {
			if (fe < 0)
				fe = i;
			le = i;
		}
	if (fe < 0) {   /* no edit at all (old == new): nothing to do */
		free(tk);
		return 0;
	}
	int ns = 0;
	for (int i = fe + 1; i < le; i++)
		if (tk[i].stable)
			ns++;
	if (ns == 0 || ns > 9) {   /* no in-span run, or > \1..\9 backref limit */
		free(tk);
		return 0;
	}

	/* Per-stable cumulative old-text length of the edit gap immediately to its
	 * left and right; a non-empty gap is a literal separator that disambiguates
	 * an adjacent full "(.*)". */
	int *lgap = emalloc(nt * sizeof(int)), *rgap = emalloc(nt * sizeof(int));
	int run = 0;
	for (int i = 0; i < nt; i++) {
		if (tk[i].stable) {
			lgap[i] = run;
			run = 0;
		} else
			run += tk[i].olen;
	}
	run = 0;
	for (int i = nt - 1; i >= 0; i--) {
		if (tk[i].stable) {
			rgap[i] = run;
			run = 0;
		} else
			run += tk[i].olen;
	}

	int absorb = 0;
	for (int i = fe + 1; i < le; i++) {
		if (!tk[i].stable)
			continue;
		if (lgap[i] > 0 && rgap[i] > 0 &&
		    str_count_occ(old, olen, tk[i-1].o, tk[i-1].olen) == 1 &&
		    str_count_occ(old, olen, tk[i+1].o, tk[i+1].olen) == 1) {
			/* Full "(.*)": its boundaries are pinned by the literal edit
			 * old-text on each side, so both separators must be unique in
			 * the old line or the greedy ".*" split is ambiguous. */
			tk[i].mode = GM_WILD;
			absorb = 1;
		} else if (fuzz_anchors(old, olen, tk[i].o, tk[i].olen,
					&tk[i].hb, &tk[i].tb)) {
			tk[i].mode = GM_FUZZ;       /* head/tail anchored absorber */
			absorb = 1;
		} else {
			tk[i].mode = GM_LIT;        /* no unique minimal anchors */
		}
	}
	free(lgap);
	free(rgap);
	if (!absorb) {   /* reproduces the span verbatim: a dup of the exact rung */
		free(tk);
		return 0;
	}

	sbuf_smake(pat, 128)
	sbuf_smake(repl, 128)
	int g = 0;
	for (int i = fe; i <= le; i++) {
		if (tk[i].stable) {
			char br[16];
			snprintf(br, sizeof(br), "\\%d", ++g);
			if (tk[i].mode == GM_WILD) {
				sbuf_str(pat, "(.*)")
			} else if (tk[i].mode == GM_FUZZ) {
				char *h = dup_n(tk[i].o, tk[i].hb);
				char *t = dup_n(tk[i].o + tk[i].olen - tk[i].tb,
						tk[i].tb);
				char *eh = escape_sub_pat_raw(h, '/');
				char *et = escape_sub_pat_raw(t, '/');
				sbuf_chr(pat, '(')
				sbuf_str(pat, eh)
				sbuf_str(pat, ".*")
				sbuf_str(pat, et)
				sbuf_chr(pat, ')')
				free(eh);
				free(et);
				free(h);
				free(t);
			} else {
				char *tmp = dup_n(tk[i].o, tk[i].olen);
				char *e = escape_sub_pat_raw(tmp, '/');
				sbuf_chr(pat, '(')
				sbuf_str(pat, e)
				sbuf_chr(pat, ')')
				free(e);
				free(tmp);
			}
			sbuf_str(repl, br)
		} else {
			char *to = dup_n(tk[i].o, tk[i].olen);
			char *tn = dup_n(tk[i].n, tk[i].nlen);
			char *pe = escape_sub_pat_raw(to, '/');
			char *re = escape_sub_repl_raw(tn, '/');
			sbuf_str(pat, pe)
			sbuf_str(repl, re)
			free(pe);
			free(re);
			free(to);
			free(tn);
		}
	}
	free(tk);
	sbufn_null(pat)
	sbufn_null(repl)
	*pat_out = double_trailing_esc(pat->s);
	*repl_out = double_trailing_esc(repl->s);
	return 1;
}

/* Emit one s/// field (pattern or replacement): apply the outer ex_arg + shell
 * layers to an already sub-escaped string. */
static void emit_sub_field(sbuf *out, const char *escaped)
{
	char *ea = escape_exarg(escaped);
	emit_escaped_line(out, ea);
	free(ea);
}

/* Emit a substitute from pre-escaped pat/repl strings (one progression rung). */
static void emit_substitute_grp(sbuf *out, const char *pat, const char *repl)
{
	sb_str(out, "s/");
	emit_sub_field(out, pat);
	sb_chr(out, '/');
	emit_sub_field(out, repl);
	sb_chr(out, '/');
}

/* One rung of the phase-2 substitute progression: a fully-escaped s/// pair. */
typedef struct {
	char *pat;
	char *repl;
	int sid;
} subvar_t;

/* Parse "s/<pat>/<repl>/[flags]" into its (still-escaped) pat/repl substrings,
 * respecting "\/" escaped delimiters. Only the '/' delimiter is recognized
 * (the chain emit hardcodes it). Returns 1 and allocates pat/repl on success,
 * leaving any trailing flags out (the chain has no use for them). */
static int parse_sub_line(const char *line, char **pat, char **repl)
{
	if (line[0] != 's' || line[1] != '/')
		return 0;
	const char *p = line + 2;
	const char *ends[2];
	for (int f = 0; f < 2; f++) {
		while (*p && *p != '/') {
			if (*p == '\\' && p[1])
				p++;
			p++;
		}
		if (*p != '/')
			return 0;
		ends[f] = p;
		p++;
	}
	*pat = dup_n(line + 2, ends[0] - (line + 2));
	*repl = dup_n(ends[0] + 1, ends[1] - (ends[0] + 1));
	return 1;
}

/*
 * Phase 2 substitute progression: try each variant (exact -> grp-absorbing)
 * in order at the mark, first success wins. The s/// is both
 * test and action: a failed match leaves the line untouched, so the next, looser
 * variant is safe to try; the first success short-circuits with 1q so no later
 * variant can re-edit the (now changed) line. A non-primary success reports via
 * ${OK2}. If every variant fails the trailing <0;1;..>??! DNF * check reports FAIL.
 * Structure mirrors emit_fallback_chain (phase 1).
 *
 * A single-variant chain degrades to a plain addressed s/// + check.
 */
static void emit_substitute_chain(sbuf *out, int line, int mark_id,
				  subvar_t *v, int nv)
{
	int sids[NSEARCH];
	if (nv <= 1) {
		sb_printf(out, "'%d", mark_id);
		emit_substitute_grp(out, v[0].pat, v[0].repl);
		EMIT_SEP(out);
		emit_err_check_mark(out, line, mark_id);
		return;
	}
	sb_chr(out, '?');
	for (int n = 0; n < nv; n++) {
		if (n)
			EMIT_ESCSEP(out);
		/* action: substitute at the mark (status tested below) */
		sb_printf(out, "'%d", mark_id);
		emit_substitute_grp(out, v[n].pat, v[n].repl);
		EMIT_ESCSEP(out);
		sb_printf(out, "%d??", v[n].sid);   /* capture s/// status into tag */
		EMIT_ESCSEP(out);
		sb_printf(out, "%d??", v[n].sid);   /* on success (fire): */
		if (n) {
			/* harmless mark jump as the immediate then-arg keeps
			 * ${OK2} non-immediate (mirrors OK1 after "m id") */
			sb_printf(out, "'%d", mark_id);
			EMIT_ESC3SEP(out);
			sb_printf(out, "${OK2}p OK %s:%d:s%d",
				  cur_file_path ? cur_file_path : "?", line, v[n].sid);
			if (n < nv - 1) {
				EMIT_ESC3SEP(out);
				sb_str(out, "1q");
			}
		} else {
			sb_str(out, "1q");
		}
	}
	EMIT_SEP(out);
	sb_str(out, "${LB}\n");
	EMIT_SEP(out);
	for (int n = 0; n < nv; n++)
		sids[n] = v[n].sid;
	emit_err_check_subs(out, sids, nv, line, mark_id);
	sb_str(out, "${LB}\n");
	EMIT_SEP(out);
}

/* Build the substitute progression for a single-line change into v[0..1]:
 * rung 0 exact (minimal-span s/old/new/), rung 1 grp-capture absorbing variant
 * over the exact span (built from the full hunk line, no original-file reach).
 * The grp rung is skipped when it would not absorb any interior drift (i.e.
 * would be a pure dup of the exact rung). Returns the count. Fields are fully
 * escaped (as displayed in interactive mode). Caller frees. */
static int build_sub_variants(group_t *g, subvar_t *v)
{
	int nv = 0;
	build_exact_sub(g->ld_old_text, g->ld_new_text, &v[nv].pat, &v[nv].repl);
	v[nv].sid = 1;
	nv++;
	if (build_grp_variant(g->del_texts[0], g->add_texts[0],
			      &v[nv].pat, &v[nv].repl)) {
		v[nv].sid = 2;
		nv++;
	}
	return nv;
}

/* Phase 2: substitute at a mark, building the exact -> grp-absorbing
 * progression. The pattern can fail to match within the (possibly drifted)
 * line, so each rung is error-checked; see emit_substitute_chain. */
static void emit_mark_substitute(sbuf *out, int line, int mark_id,
				 group_t *g)
{
	subvar_t v[2];
	int nv = build_sub_variants(g, v);
	emit_substitute_chain(out, line, mark_id, v, nv);
	for (int i = 0; i < nv; i++) {
		free(v[i].pat);
		free(v[i].repl);
	}
}

/* Phase 2: horizontal ;c / ;d edit tail, emitted after an address
 * prefix ('N for marks). Uses precomputed minimal diff positions. */
static void emit_horiz_tail(sbuf *out, group_t *g)
{
	if (!*g->ldc_new_text && g->ldc_start != g->ldc_end)
		sb_printf(out, ";%d;%dd", g->ldc_start, g->ldc_end);
	else if (g->ldc_start == g->ldc_end) {
		sb_printf(out, ";%dc ", g->ldc_start);
		emit_escaped_text(out, g->ldc_new_text);
	} else {
		sb_printf(out, ";%d;%dc ", g->ldc_start, g->ldc_end);
		emit_escaped_text(out, g->ldc_new_text);
	}
	EMIT_SEP(out);
}

/* Parse and strip relative offset prefix from custom edit lines (rel/relc).
 * "+N" or "-N" alone as lines[0]: offset-only line (substitute with offset).
 *   Extract offset, remove lines[0], shift remaining lines down, decrement *nlines.
 * "+N" or "-N" followed immediately by verb (e.g. "+3a text"): embedded prefix.
 *   Extract offset number, strip prefix from lines[0] in-place.
 * Returns the extracted offset (0 if no prefix found). */
static int parse_ecmd_offset(char **lines, int *nlines)
{
	if (*nlines == 0)
		return 0;
	char *first = lines[0];
	if (first[0] != '+' && first[0] != '-')
		return 0;
	int i = 1;
	while (first[i] >= '0' && first[i] <= '9')
		i++;
	if (i == 1)
		return 0; /* sign but no digits */
	int offset = atoi(first);
	if (first[i] == '\0') {
		/* Offset-only line: remove it, shift remaining */
		free(lines[0]);
		for (int k = 0; k < *nlines - 1; k++)
			lines[k] = lines[k + 1];
		(*nlines)--;
	} else {
		/* Prefix embedded in verb line: strip leading "+N" */
		memmove(first, first + i, strlen(first + i) + 1);
	}
	return offset;
}

/* Match a SEARCH PATTERN section line that is only a +N/-N offset
 * override. Real pattern lines starting with + are regex-escaped
 * (\+), so a bare signed number is unambiguous. */
static int pat_off_line(const char *s, int *off)
{
	if ((s[0] != '+' && s[0] != '-') || !s[1])
		return 0;
	for (const char *p = s + 1; *p; p++)
		if (*p < '0' || *p > '9')
			return 0;
	*off = atoi(s);
	return 1;
}

/* grp_delta_t doubles as the per-group editor-buffer parse result; the parse
 * path just leaves group_idx/pre_ctx/post_ctx unset. */

#define FREE_ARR(p, n) do { for (int _i = 0; _i < (n); _i++) free((p)[_i]); \
			    free(p); } while (0)

/* Free every array a grp_delta_t owns (scalars need no cleanup). */
static void free_grp(grp_delta_t *p)
{
	FREE_ARR(p->del_lines, p->ndel_lines);
	FREE_ARR(p->add_lines, p->nadd_lines);
	FREE_ARR(p->custom_text, p->ncustom_text);
	FREE_ARR(p->pre_ctx, p->npre_ctx);
	FREE_ARR(p->post_ctx, p->npost_ctx);
	for (int k = 0; k < NSEARCH; k++)
		FREE_ARR(p->pattern[k], p->npattern[k]);
	FREE_ARR(p->abs_cmd, p->nabs);
	FREE_ARR(p->rel_cmd, p->nrel);
	FREE_ARR(p->relc_cmd, p->nrelc);
	free(p->ph1);
	free(p->ph2);
}

/* Section codes shared by the editor-buffer (parse_grp_blob) and embedded-delta
 * (main) parsers. The two formats carry the same per-group fields under
 * different header spellings; each section's body appends through gsect_add so
 * the field set lives in one place. CONTENT (-/+ -> del/add) stays per-parser:
 * the two formats split it differently. */
enum {
	GS_NONE = 0, GS_PAT, GS_ABS, GS_REL, GS_RELC,
	GS_CONTENT, GS_PRE, GS_POST, GS_CUSTOM, GS_STRAT,
};

/* Append a body line into the grp_delta_t array selected by sect; pat_idx picks
 * the pattern slot for GS_PAT. */
static void gsect_add(grp_delta_t *gd, int sect, int pat_idx, const char *line)
{
	switch (sect) {
	case GS_PAT:
		arr_append(&gd->pattern[pat_idx], &gd->npattern[pat_idx],
			   &gd->pat_cap[pat_idx], line);
		break;
	case GS_ABS:
		arr_append(&gd->abs_cmd, &gd->nabs, &gd->abs_cap, line);
		break;
	case GS_REL:
		arr_append(&gd->rel_cmd, &gd->nrel, &gd->rel_cap, line);
		break;
	case GS_RELC:
		arr_append(&gd->relc_cmd, &gd->nrelc, &gd->relc_cap, line);
		break;
	case GS_PRE:
		arr_append(&gd->pre_ctx, &gd->npre_ctx, &gd->pre_cap, line);
		break;
	case GS_POST:
		arr_append(&gd->post_ctx, &gd->npost_ctx, &gd->post_cap, line);
		break;
	case GS_CUSTOM:
		arr_append(&gd->custom_text, &gd->ncustom_text,
			   &gd->custom_text_cap, line);
		break;
	case GS_STRAT:
		gd->strategy = strat_from_name(line, strlen(line));
		break;
	}
}

/* Parse "=== LEVEL <n>[*] ===" into gd->level / gd->has_star (default 2). */
static void parse_level(grp_delta_t *gd, char *line)
{
	char *lv = line + 10;
	char *end = strstr(lv, " ===");
	if (end)
		*end = '\0';
	int len = strlen(lv);
	gd->has_star = (len > 0 && lv[len - 1] == '*');
	gd->level = atoi(lv);
	if (gd->level < 1)
		gd->level = 2;
}

/*
 * Parse a multi-file interactive editor blob (mutated in place: each line
 * gets its newline replaced with a terminator, so parse a blob only once).
 * Sections marked by "=== FILE: <path> ===" route subsequent groups to
 * per_file_results[k] (k = matching index in active[]). Stores raw content
 * (no parse_ecmd_offset stripping) for apples-to-apples comparison between
 * the generated baseline and the edited buffer.
 */
static void parse_grp_blob(char *blob, file_patch_t **active, int nactive,
			   grp_delta_t **per_file_results)
{
	char *line, *next;
	int gi = -1;
	int file_idx = -1;
	int in_pat = 0, in_cstrat = 0, in_ecmd = 0;
	int in_content_section =
		0;  /* between GROUP header and first section keyword */
	int ecmd_strat = STRAT_DEFAULT;
	int in_ph = 0;      /* 1/2 = inside a PHASE blob (raw capture) */
	sbuf_smake(ph, MAX_LINE)

	for (line = blob; line; line = next) {
		char *nl = strchr(line, '\n');
		next = nl ? nl + 1 : NULL;
		if (nl)
			*nl = '\0';
		else if (!*line)
			break;  /* blob ends in a newline: no final line */
		chomp(line);

		/* PHASE blobs are byte-verbatim: only the end tag terminates,
		 * every other line (including "=== ..." lines) is content. The
		 * display pass appended one newline after the blob; strip it
		 * back off (exact inverse). */
		if (in_ph) {
			if (strcmp(line, end_tag_rd) == 0) {
				if (ph->s_n > 0 && ph->s[ph->s_n - 1] == '\n')
					ph->s_n--;
				sbuf_null(ph)
				if (file_idx >= 0 && gi >= 0 &&
				    gi < active[file_idx]->ngroups) {
					grp_delta_t *pg = &per_file_results[file_idx][gi];
					char **dst = in_ph == 1 ? &pg->ph1 : &pg->ph2;
					free(*dst);
					*dst = uc_dup(ph->s);
				}
				sbufn_cut(ph, 0)
				in_ph = 0;
			} else {
				sbuf_str(ph, line)
				sbuf_chr(ph, '\n')
			}
			continue;
		}
		if (strcmp(line, "=== PHASE 1 ===") == 0 ||
		    strcmp(line, "=== PHASE 2 ===") == 0) {
			in_ph = line[10] - '0';
			in_ecmd = 0;
			in_content_section = 0;
			in_pat = 0;
			in_cstrat = 0;
			continue;
		}

		/* "=== OFFSET <%+d> MODE <%d> ===" marker right after a
		 * SEARCH PATTERN header: the offset and search mode for that
		 * pattern. Handled before the generic reset so in_pat stays
		 * active. MODE is optional (older files omit it). */
		if (strncmp(line, "=== OFFSET ", 11) == 0) {
			if (in_pat && file_idx >= 0 && gi >= 0 &&
			    gi < active[file_idx]->ngroups) {
				grp_delta_t *pg = &per_file_results[file_idx][gi];
				pg->pat_off[in_pat - 1] = atoi(line + 11);
				pg->pat_has_off[in_pat - 1] = 1;
				char *m = strstr(line + 11, " MODE ");
				if (m) {
					pg->pat_mode[in_pat - 1] = atoi(m + 6);
					pg->pat_has_mode[in_pat - 1] = 1;
				}
			}
			continue;
		}

		if (strncmp(line, "=== ", 4) == 0) {
			in_ecmd = 0;
			in_content_section = 0;
			in_pat = 0;
			in_cstrat = 0;
		}

		if (strncmp(line, "=== FILE: ", 10) == 0) {
			const char *p = line + 10;
			const char *end = strstr(p, " ===");
			int plen = end ? (int)(end - p) : (int)strlen(p);
			file_idx = -1;
			for (int k = 0; k < nactive; k++) {
				if ((int)strlen(active[k]->path) == plen &&
				    strncmp(active[k]->path, p, plen) == 0) {
					file_idx = k;
					break;
				}
			}
			gi = -1;
			continue;
		}
		if (strncmp(line, "=== GROUP ", 10) == 0) {
			gi = atoi(line + 10) - 1;
			in_content_section = 1;
			/* optional "MARK <m>" (verbatim blobs reference it) */
			const char *mk = strstr(line + 10, " MARK ");
			if (mk && file_idx >= 0 && gi >= 0 &&
			    gi < active[file_idx]->ngroups)
				per_file_results[file_idx][gi].ovr_mark =
					atoi(mk + 6);
			continue;
		}
		if (strncmp(line, "=== COMMAND STRATEGY", 20) == 0) {
			in_cstrat = 1;
			continue;
		}
		if (strncmp(line, "=== SEARCH PATTERN", 18) == 0) {
			/* "=== SEARCH PATTERN <1-NSEARCH> ===", bare legacy form
			 * maps to the top-context slot (historical single
			 * pattern), now SEARCH PATTERN 3. */
			const char *p = line + 18;
			while (*p == ' ')
				p++;
			in_pat = (*p >= '1' && *p <= '0' + NSEARCH) ? *p - '0' : 3;
			continue;
		}
		if (strncmp(line, "=== EDIT COMMAND (", 18) == 0) {
			in_ecmd = 1;
			const char *p = line + 18, *e = strchr(p, ')');
			ecmd_strat = e ? strat_from_name(p, e - p) : STRAT_DEFAULT;
			continue;
		}
		if (strcmp(line, end_tag_rd) == 0)
			continue;

		if (file_idx < 0)
			continue;
		int ngroups = active[file_idx]->ngroups;
		grp_delta_t *results = per_file_results[file_idx];

		if (in_ecmd && gi >= 0 && gi < ngroups) {
			int s = ecmd_strat == STRAT_ABS ? GS_ABS
				: ecmd_strat == STRAT_REL ? GS_REL
				: ecmd_strat == STRAT_RELC ? GS_RELC : GS_NONE;
			gsect_add(&results[gi], s, 0, line);
			continue;
		}

		if (in_cstrat) {
			const char *name = (line[0] == '#') ? line + 1 : line;
			int s = strat_from_name(name, strlen(name));
			/* first-wins: don't overwrite if user
			 * uncommented multiple strategies */
			if (line[0] != '#' && s != STRAT_DEFAULT &&
			    gi >= 0 && gi < ngroups &&
			    results[gi].strategy == STRAT_DEFAULT)
				results[gi].strategy = s;
			continue;
		}

		/* Capture lines that appear after GROUP header.
		 * -/+ prefixed lines go into del_lines/add_lines (backward compat).
		 * ALL lines (including non-prefixed) go into custom_text as-is. */
		if (gi >= 0 && gi < ngroups && in_content_section &&
		    line[0] == '-' && line[1] != '-') {
			arr_append(&results[gi].del_lines, &results[gi].ndel_lines,
				   &results[gi].del_cap, line + 1);
		}
		if (gi >= 0 && gi < ngroups && in_content_section &&
		    line[0] == '+') {
			arr_append(&results[gi].add_lines, &results[gi].nadd_lines,
				   &results[gi].add_cap, line + 1);
		}

		/* Parse level: field (appears after END GROUP, before sections) */
		if (gi >= 0 && gi < ngroups &&
		    strncmp(line, "=== LEVEL ", 10) == 0) {
			parse_level(&results[gi], line);
			continue;
		}

		if (in_pat && gi >= 0 && gi < ngroups)
			gsect_add(&results[gi], GS_PAT, in_pat - 1, line);

		/* Catch-all: capture every line in the group section into custom_text as-is */
		if (gi >= 0 && gi < ngroups && in_content_section)
			gsect_add(&results[gi], GS_CUSTOM, 0, line);
	}

	free(ph->s);
}

/* Emit one group's delta in human-readable structured format */
static void emit_grp_delta(sbuf *out, grp_delta_t *gd)
{
	sb_printf(out, "=== GROUP %d ===\n", gd->group_idx);
	for (int i = 0; i < gd->ndel_lines; i++)
		sb_printf(out, "-%s\n", gd->del_lines[i]);
	for (int i = 0; i < gd->nadd_lines; i++)
		sb_printf(out, "+%s\n", gd->add_lines[i]);
	sb_printf(out, "%s\n", end_tag_wr);
	int eglvl = gd->level ? gd->level : 2;
	sb_printf(out, "=== LEVEL %d%s ===\n", eglvl,
		  gd->has_star ? "*" : "");
	if (gd->ncustom_text > 0) {
		sb_printf(out, "=== custom_text ===\n");
		for (int i = 0; i < gd->ncustom_text; i++)
			sb_printf(out, "%s\n", gd->custom_text[i]);
		sb_printf(out, "%s\n", end_tag_wr);
	}
	if (gd->npre_ctx > 0) {
		sb_printf(out, "=== pre_ctx ===\n");
		for (int i = 0; i < gd->npre_ctx; i++)
			sb_printf(out, "%s\n", gd->pre_ctx[i]);
		sb_printf(out, "%s\n", end_tag_wr);
	}
	if (gd->npost_ctx > 0) {
		sb_printf(out, "=== post_ctx ===\n");
		for (int i = 0; i < gd->npost_ctx; i++)
			sb_printf(out, "%s\n", gd->post_ctx[i]);
		sb_printf(out, "%s\n", end_tag_wr);
	}
	if (gd->strategy != STRAT_DEFAULT) {
		const char *s = "abs";
		if (gd->strategy == STRAT_REL)
			s = "rel";
		else if (gd->strategy == STRAT_RELC)
			s = "relc";
		sb_printf(out, "=== strategy ===\n%s\n%s\n", s, end_tag_wr);
	}
	for (int pi = 0; pi < NSEARCH; pi++) {
		if (gd->npattern[pi] > 0) {
			sb_printf(out, "=== pattern%d ===\n", pi + 1);
			for (int i = 0; i < gd->npattern[pi]; i++)
				sb_printf(out, "%s\n", gd->pattern[pi][i]);
			sb_printf(out, "%s\n", end_tag_wr);
		}
		if (gd->pat_has_off[pi])
			sb_printf(out, "=== offset%d %+d ===\n",
				  pi + 1, gd->pat_off[pi]);
		if (gd->pat_has_mode[pi])
			sb_printf(out, "=== mode%d %d ===\n",
				  pi + 1, gd->pat_mode[pi]);
	}
	if (gd->nabs > 0) {
		sb_printf(out, "=== edit_cmd_abs ===\n");
		for (int i = 0; i < gd->nabs; i++)
			sb_printf(out, "%s\n", gd->abs_cmd[i]);
		sb_printf(out, "%s\n", end_tag_wr);
	}
	if (gd->nrelc > 0) {
		sb_printf(out, "=== edit_cmd_relc ===\n");
		for (int i = 0; i < gd->nrelc; i++)
			sb_printf(out, "%s\n", gd->relc_cmd[i]);
		sb_printf(out, "%s\n", end_tag_wr);
	}
	if (gd->nrel > 0) {
		sb_printf(out, "=== edit_cmd_rel ===\n");
		for (int i = 0; i < gd->nrel; i++)
			sb_printf(out, "%s\n", gd->rel_cmd[i]);
		sb_printf(out, "%s\n", end_tag_wr);
	}
	if (gd->ph1 || gd->ph2) {
		sb_printf(out, "=== verbatim mark %d esc %d ===\n",
			  gd->ovr_mark, gd->ovr_esc);
		sb_printf(out, "=== phase1 ===\n%s\n%s\n",
			  gd->ph1 ? gd->ph1 : "", end_tag_wr);
		sb_printf(out, "=== phase2 ===\n%s\n%s\n",
			  gd->ph2 ? gd->ph2 : "", end_tag_wr);
	}
}

/* Emit one file-validated window SEARCH PATTERN (fuzz/grp/straddle slots). A
 * recorded delta wins; else the freshly generated window w (when has). def_mode
 * is the recorded-delta mode default (consulted only when recorded). */
static void emit_win_section(sbuf *fp, grp_delta_t *gd, int slot,
			     fuzzwin_t *w, int has, int def_mode)
{
	int recorded = gd && gd->npattern[slot] > 0;
	if (!recorded && !has)
		return;
	sb_printf(fp, "=== SEARCH PATTERN %d ===\n", slot + 1);
	int poff = (gd && gd->pat_has_off[slot]) ? gd->pat_off[slot]
		   : recorded ? 0 : w->offset;
	int pmode = (gd && gd->pat_has_mode[slot]) ? gd->pat_mode[slot]
		    : recorded ? def_mode : w->mode;
	sb_printf(fp, "=== OFFSET %+d MODE %d ===\n", poff, pmode);
	if (recorded)
		for (int i = 0; i < gd->npattern[slot]; i++)
			sb_printf(fp, "%s\n", gd->pattern[slot][i]);
	else
		for (int i = 0; i < w->nlines; i++)
			sb_printf(fp, "%s\n", w->lines[i]);
	sb_printf(fp, "%s\n", end_tag_wr);
}

/*
 * Write all groups to fp, optionally injecting stored delta from in_fd.
 * with_phase adds the MARK header field and the per-group PHASE 1/PHASE 2
 * sections holding the group's verbatim segment bytes (override, else
 * generated); requires gen_group_segments to have run.
 */
static void write_groups_to_file(sbuf *fp, group_t *groups, int ngroups,
				 file_delta_t *in_fd, int is_new,
				 const char *orig_path, int with_phase)
{
	/* Load the pre-patch original so the fuzzed SEARCH PATTERN sections
	 * can be validated against it; freed before returning. */
	if (orig_path && !is_new)
		load_orig_file(orig_path);
	for (int gi = 0; gi < ngroups; gi++) {
		group_t *g = &groups[gi];
		if (!g->del_start && !g->nadd)
			continue;
		int target = g->del_start ? g->del_start : g->add_after;

		grp_delta_t *gd = find_grp_delta(in_fd, gi + 1,
						 g->del_texts, g->ndel,
						 g->add_texts, g->nadd,
						 g->all_pre_ctx, g->nall_pre_ctx,
						 g->post_ctx, g->npost_ctx,
						 delta_mode > 0 ? delta_mode : 0);

		int has_anchors = g->nanchors >= 2
				  || (g->nanchors == 1 && g->anchors[0] && g->anchors[0][0])
				  || (g->follow_ctx && g->follow_ctx[0])
				  || (g->ndel > 0 && g->del_texts[0] && g->del_texts[0][0]);

		int default_offset = 0;
		if (g->nanchors >= 2)
			default_offset = g->nanchors + g->anchor_offset - 1;
		else if (g->nanchors == 1)
			default_offset = g->anchor_offset;
		else if (g->follow_ctx && g->follow_ctx[0])
			default_offset = -(g->follow_offset);
		else if (!(g->ndel > 0 && g->del_texts[0] && g->del_texts[0][0]))
			default_offset = g->block_change_idx;

		/* A +N/-N prefix on a stored rel/relc EDIT COMMAND stays on
		 * the verb: it rides the mark address at apply time (see
		 * EMIT_MARK_EDIT, which folds custom_offset into "'N+off"),
		 * so an insert-above-line-1 ("-1i") survives replay instead
		 * of underflowing a pattern search offset to line 0. */

		/* Group header; MARK names the phase-1 mark id the PHASE
		 * blobs reference (edit it when renumbering marks in them) */
		if (with_phase && g->mark_id >= 0)
			sb_printf(fp, "=== GROUP %d/%d (line %d) MARK %d ===\n",
				  gi + 1, ngroups, target, g->mark_id);
		else
			sb_printf(fp, "=== GROUP %d/%d (line %d) ===\n",
				  gi + 1, ngroups, target);
		if (gd && gd->ncustom_text > 0 && gd->has_star && in_fd) {
			for (int i = 0; i < gd->ncustom_text; i++)
				sb_printf(fp, "%s\n", gd->custom_text[i]);
		} else {
			for (int i = 0; i < g->ndel; i++)
				sb_printf(fp, "-%s\n", g->del_texts[i]);
			for (int i = 0; i < g->nadd; i++)
				sb_printf(fp, "+%s\n", g->add_texts[i]);
		}
		sb_printf(fp, "%s\n", end_tag_wr);
		int lvl = (gd && gd->level) ? gd->level : 2;
		sb_printf(fp, "=== LEVEL %d%s ===\n", lvl, gd && gd->has_star ? "*" : "");

		/* COMMAND STRATEGY: inject stored strategy or keep all commented */
		int sel_strat = (gd && gd->strategy != STRAT_DEFAULT)
				? gd->strategy : STRAT_DEFAULT;
		sb_printf(fp, "=== COMMAND STRATEGY ===\n");
		sb_printf(fp, "%sabs\n", sel_strat == STRAT_ABS ? "" : "#");
		if (has_anchors && g->ndel == 1 && g->nadd == 1 && g->has_line_diff)
			sb_printf(fp, "%srelc\n", sel_strat == STRAT_RELC ? "" : "#");
		if (has_anchors)
			sb_printf(fp, "%srel\n", sel_strat == STRAT_REL ? "" : "#");

		/* SEARCH PATTERN 1-NPAT (fallbacks, first match wins):
		 * 1 = whole hunk, 2 = deleted lines + post ctx, 3 = top
		 * context only, 4 = deleted lines, 5 = post ctx only.
		 * Single-line patterns show the ^...$ disamb anchors so the
		 * user can remove them; emit respects the edit. */
		sb_printf(fp, "%s\n", end_tag_wr);
		/* Pure adds position on the line to append after, so the
		 * displayed offsets include the -1 step the append-after "i"
		 * implies (matching the "i"/"-1i" choice in the rel EDIT
		 * COMMAND). */
		int pure_add = !g->del_start && g->nadd;
		int add_a = pure_add && default_offset - 1 >= 0;
		char **praw = emalloc((g->ndel + 7) * sizeof(char *));
		for (int pi = 0; pi < NPAT; pi++) {
			sb_printf(fp, "=== SEARCH PATTERN %d ===\n", pi + 1);
			int doff;
			int n = default_pat_lines(g, pi, praw, &doff);
			/* OFFSET marker: lines from match start to the edit
			 * target when this pattern matches. MODE selects the
			 * search form: 1 = .,$f> (live buffer, default for
			 * single-line patterns), 0 = %f> (register cache). */
			int poff = (gd && gd->pat_has_off[pi])
				   ? gd->pat_off[pi]
				   : doff - (add_a ? 1 : 0);
			int pat_nlines = (gd && gd->npattern[pi] > 0)
					 ? gd->npattern[pi] : n;
			int pmode = (gd && gd->pat_has_mode[pi])
				    ? gd->pat_mode[pi]
				    : pat_nlines == 1 ? 1 : 0;
			sb_printf(fp, "=== OFFSET %+d MODE %d ===\n", poff, pmode);
			if (gd && gd->npattern[pi] > 0) {
				for (int i = 0; i < gd->npattern[pi]; i++)
					sb_printf(fp, "%s\n", gd->pattern[pi][i]);
			} else {
				int wrap = n == 1;
				for (int i = 0; i < n; i++) {
					char *esc = escape_regex(praw[i]);
					sb_printf(fp, wrap ? "^%s$\n" : "%s\n", esc);
					free(esc);
				}
			}
			sb_printf(fp, "%s\n", end_tag_wr);
		}
		free(praw);

		/* File-validated relaxed SEARCH PATTERN slots (fuzz NPAT..,
		 * grp 7, straddle 8/9). Generated fresh from the original; a
		 * recorded delta wins so user tweaks round-trip. Pre-escaped
		 * regex, written verbatim. */
		fuzzwin_t fz[NFUZZ];
		int nfz = gen_fuzz_windows(g, fz, NFUZZ);
		for (int pi = NPAT; pi < GRP_SLOT; pi++) {
			int fi = pi - NPAT;
			emit_win_section(fp, gd, pi, fi < nfz ? &fz[fi] : NULL,
					 fi < nfz, gd && gd->npattern[pi] == 1);
		}
		free_fuzz_windows(fz, nfz);

		fuzzwin_t gw;
		int has_gw = gen_grp_window(g, &gw);
		emit_win_section(fp, gd, GRP_SLOT, &gw, has_gw, 2);
		if (has_gw)
			free_fuzz_windows(&gw, 1);

		fuzzwin_t ww;
		int has_ww = gen_win_window(g, &ww, 0);
		emit_win_section(fp, gd, WIN_SLOT, &ww, has_ww, 3);
		if (has_ww)
			free_fuzz_windows(&ww, 1);

		fuzzwin_t ww2;
		int has_ww2 = gen_win_window(g, &ww2, 1);
		emit_win_section(fp, gd, WIN2_SLOT, &ww2, has_ww2, 3);
		if (has_ww2)
			free_fuzz_windows(&ww2, 1);

		/* EDIT COMMAND sections */
#define WG_CONTENT(fp) do { \
	if (g->nadd > 0) { \
		sb_chr((fp), ' '); \
		sb_str((fp), g->add_texts[0]); \
		sb_chr((fp), '\n'); \
		for (int _k = 1; _k < g->nadd; _k++) { \
			sb_str((fp), g->add_texts[_k]); \
			sb_chr((fp), '\n'); \
		} \
	} else { sb_chr((fp), '\n'); } \
} while (0)

		/* abs */
		sb_str(fp, "=== EDIT COMMAND (abs) ===\n");
		if (gd && gd->nabs > 0) {
			for (int k = 0; k < gd->nabs; k++)
				sb_printf(fp, "%s\n", gd->abs_cmd[k]);
		} else {
			if (g->del_start && g->nadd) {
				if (g->ndel == 1)
					sb_printf(fp, "%dc", g->del_start);
				else
					sb_printf(fp, "%d,%dc", g->del_start, g->del_end);
				WG_CONTENT(fp);
			} else if (g->del_start) {
				if (g->ndel == 1)
					sb_printf(fp, "%dd\n", g->del_start);
				else
					sb_printf(fp, "%d,%dd\n", g->del_start, g->del_end);
			} else if (g->nadd) {
				if (is_new)
					sb_str(fp, "i");
				else if (g->add_after <= 0)
					sb_str(fp, "0i");
				else
					sb_printf(fp, "%di", g->add_after);
				WG_CONTENT(fp);
			}
		}
		sb_printf(fp, "%s\n", end_tag_wr);

		/* relc */
		int show_relc = has_anchors && g->ndel == 1 && g->nadd == 1 && g->has_line_diff;
		if (show_relc || (gd && gd->nrelc > 0)) {
			sb_str(fp, "=== EDIT COMMAND (relc) ===\n");
			if (gd && gd->nrelc > 0) {
				for (int k = 0; k < gd->nrelc; k++)
					sb_printf(fp, "%s\n", gd->relc_cmd[k]);
			} else if (show_relc) {
				if (g->ldc_start == g->ldc_end)
					sb_printf(fp, ".;%dc %s\n",
						  g->ldc_start, g->ldc_new_text);
				else
					sb_printf(fp, ".;%d;%dc %s\n",
						  g->ldc_start, g->ldc_end,
						  g->ldc_new_text);
			}
			sb_printf(fp, "%s\n", end_tag_wr);
		}

		/* rel */
		if (has_anchors || (gd && gd->nrel > 0)) {
			sb_str(fp, "=== EDIT COMMAND (rel) ===\n");
			if (gd && gd->nrel > 0) {
				for (int k = 0; k < gd->nrel; k++)
					sb_printf(fp, "%s\n", gd->rel_cmd[k]);
			} else if (has_anchors) {
				if (g->ndel == 1 && g->nadd == 1 &&
				    g->has_line_diff) {
					/* substitute progression: one s/// per rung
					 * (exact -> grp-absorbing), newline separated.
					 * All target the same marked line; the emit
					 * side turns >1 rung into a first-wins chain. */
					subvar_t v[2];
					int nv = build_sub_variants(g, v);
					for (int k = 0; k < nv; k++) {
						sb_printf(fp, "s/%s/%s/\n",
							  v[k].pat, v[k].repl);
						free(v[k].pat);
						free(v[k].repl);
					}
				} else if (g->del_start && g->nadd) {
					if (g->ndel == 1)
						sb_str(fp, "c");
					else
						sb_printf(fp, ",#+%dc", g->ndel - 1);
					WG_CONTENT(fp);
				} else if (g->del_start) {
					if (g->ndel == 1)
						sb_str(fp, "d\n");
					else
						sb_printf(fp, ",#+%dd\n", g->ndel - 1);
				} else if (g->nadd) {
					sb_str(fp, add_a ? "i" : "-1i");
					WG_CONTENT(fp);
				}
			}
			sb_printf(fp, "%s\n", end_tag_wr);
		}

		/* PHASE 1/2: the verbatim ex-body bytes this group contributes
		 * (override wins over generated). Inside these sections only
		 * the end tag terminates; editing them supersedes every
		 * structured section above for this group. */
		if (with_phase) {
			const char *b1 = g->ph1_ovr ? g->ph1_ovr : g->ph1_gen;
			const char *b2 = g->ph2_ovr ? g->ph2_ovr : g->ph2_gen;
			sb_printf(fp, "=== PHASE 1 ===\n%s\n%s\n",
				  b1 ? b1 : "", end_tag_wr);
			sb_printf(fp, "=== PHASE 2 ===\n%s\n%s\n",
				  b2 ? b2 : "", end_tag_wr);
		}
		if (gi + 1 < ngroups)
			sb_chr(fp, '\n');
	}
	free_orig_file();
}

static void gen_group_segments(file_patch_t *fp);

/* Drop every custom_* override on g so apply_grp_edits starts from a clean
 * slate; an emptied editor section then reverts to defaults instead of
 * leaving the previous pass's values behind. */
static void clear_group_customs(group_t *g)
{
	for (int pi = 0; pi < NSEARCH; pi++) {
		FREE_ARR(g->custom_pat[pi], g->ncustom_pat[pi]);
		g->custom_pat[pi] = NULL;
		g->ncustom_pat[pi] = 0;
		g->custom_pat_has_off[pi] = 0;
		g->custom_pat_off[pi] = 0;
		g->custom_pat_has_mode[pi] = 0;
		g->custom_pat_mode[pi] = 0;
	}
	FREE_ARR(g->custom_abs_lines, g->custom_abs_nlines);
	g->custom_abs_lines = NULL;
	g->custom_abs_nlines = 0;
	FREE_ARR(g->custom_relc_lines, g->custom_relc_nlines);
	g->custom_relc_lines = NULL;
	g->custom_relc_nlines = 0;
	FREE_ARR(g->custom_rel_lines, g->custom_rel_nlines);
	g->custom_rel_lines = NULL;
	g->custom_rel_nlines = 0;
	g->custom_offset = 0;
}

/* Transfer parsed per-group sections eg into g's custom_* overrides (steals
 * eg's arrays). Runs twice per session - pre-editor to bake stored structured
 * deltas into the PHASE baselines, post-editor with the user's edits - so the
 * previous pass's values are cleared first. */
static void apply_grp_edits(group_t *g, grp_delta_t *eg)
{
	clear_group_customs(g);
	g->strategy = eg->strategy;
	for (int pi = 0; pi < NSEARCH; pi++) {
		/* OFFSET marker: per-pattern offset, wins over
		 * the legacy +N first-line override */
		if (eg->pat_has_off[pi]) {
			g->custom_pat_has_off[pi] = 1;
			g->custom_pat_off[pi] = eg->pat_off[pi];
		}
		if (eg->pat_has_mode[pi]) {
			g->custom_pat_has_mode[pi] = 1;
			g->custom_pat_mode[pi] = eg->pat_mode[pi];
		}
		if (eg->npattern[pi] == 0)
			continue;
		/* a first line of just +N/-N overrides this
		 * pattern's search offset */
		int poff;
		if (pat_off_line(eg->pattern[pi][0], &poff)) {
			if (!eg->pat_has_off[pi]) {
				g->custom_pat_has_off[pi] = 1;
				g->custom_pat_off[pi] = poff;
			}
			free(eg->pattern[pi][0]);
			memmove(eg->pattern[pi], eg->pattern[pi] + 1,
				(eg->npattern[pi] - 1) * sizeof(char *));
			eg->npattern[pi]--;
		}
		if (eg->npattern[pi] > 0) {
			g->custom_pat[pi] = eg->pattern[pi];
			g->ncustom_pat[pi] = eg->npattern[pi];
			eg->pattern[pi] = NULL;
			eg->npattern[pi] = 0;
			eg->pat_cap[pi] = 0;
		}
	}
	if (eg->nabs > 0) {
		g->custom_abs_lines = eg->abs_cmd;
		g->custom_abs_nlines = eg->nabs;
		eg->abs_cmd = NULL;
		eg->nabs = 0;
		eg->abs_cap = 0;
	}
	/* Process in file order (relc before rel) so the last-written
	 * custom_offset matches the old read_back semantics: whichever
	 * section appears last in the file wins. */
	if (eg->nrelc > 0) {
		g->custom_offset = parse_ecmd_offset(eg->relc_cmd, &eg->nrelc);
		g->custom_relc_lines = eg->relc_cmd;
		g->custom_relc_nlines = eg->nrelc;
		eg->relc_cmd = NULL;
		eg->nrelc = 0;
		eg->relc_cap = 0;
	}
	if (eg->nrel > 0) {
		g->custom_offset = parse_ecmd_offset(eg->rel_cmd, &eg->nrel);
		g->custom_rel_lines = eg->rel_cmd;
		g->custom_rel_nlines = eg->nrel;
		eg->rel_cmd = NULL;
		eg->nrel = 0;
		eg->rel_cap = 0;
	}
}

/* A structured edit supersedes a stale verbatim override: drop it, but
 * preserve the blobs (with the group's identity) in the .rej file so the
 * user can re-derive a fix from them. */
static void discard_verbatim_ovr(const char *path, int idx, group_t *g,
				 const char *rejpath)
{
	fprintf(stderr, "%s group %d: structured edit discards verbatim "
			"override (saved to %s)\n", path, idx, rejpath);
	FILE *rej = fopen(rejpath, "a");
	if (rej) {
		grp_delta_t tmp = {0};
		tmp.group_idx = idx;
		tmp.del_lines = g->del_texts;
		tmp.ndel_lines = g->ndel;
		tmp.add_lines = g->add_texts;
		tmp.nadd_lines = g->nadd;
		tmp.ph1 = g->ph1_ovr;
		tmp.ph2 = g->ph2_ovr;
		tmp.ovr_mark = g->ovr_mark;
		tmp.ovr_esc = g->ovr_esc;
		sbuf_smake(sb, MAX_LINE)
		sb_printf(sb, "=== FILE: %s ===\n", path);
		emit_grp_delta(sb, &tmp);
		sb_printf(sb, "%s\n", end_tag_wr);
		fwrite(sb->s, 1, sb->s_n, rej);
		free(sb->s);
		fclose(rej);
	}
	free(g->ph1_ovr);
	free(g->ph2_ovr);
	g->ph1_ovr = NULL;
	g->ph2_ovr = NULL;
	g->ovr_mark = 0;
	g->ovr_esc = 0;
}

/* Editor bring-up, hoisted from nextvi's main()/ex_init() — no argv
 * processing, no EXINIT, for the sessions that edit buffers patch2vi
 * built rather than files (-E goes through nextvi_main() instead, argv
 * and all). Split into init/teardown so one process can run several
 * independent editor lifetimes: the interactive buffer session
 * (edit_buffers) and, one per script block, the -e runner (run_body).
 * The global config tables and the input buffer are process-wide and
 * built once; everything else is per session and released by ed_free().
 * With use_tty, the editor gets the controlling terminal on fds 0/1 for
 * the session (patch2vi's stdin/stdout may be the patch pipe and the
 * generated script); ed_done() restores the original fds. */
static int ed_in = -1, ed_out = -1;
static int ed_once;	/* the process-wide half of the bring-up is done */

/* Put the controlling terminal on fds 0/1 for the session; ed_ungrabtty()
 * puts the caller's own stdin/stdout back. */
static int ed_grabtty(void)
{
	int tty = open("/dev/tty", O_RDWR);
	if (tty < 0) {
		perror("/dev/tty");
		return -1;
	}
	fflush(stdout);
	ed_in = dup(0);
	ed_out = dup(1);
	dup2(tty, 0);
	dup2(tty, 1);
	close(tty);
	return 0;
}

static void ed_ungrabtty(void)
{
	fflush(stdout);
	if (ed_in >= 0) {
		dup2(ed_in, 0);
		dup2(ed_out, 1);
		close(ed_in);
		close(ed_out);
		ed_in = ed_out = -1;
	}
}

static int ed_init(int use_tty)
{
	if (use_tty && ed_grabtty() < 0)
		return -1;
	if (!ed_once) {
		setup_signals();
		dir_init();
		syn_init();
		ibuf = emalloc(ibuf_sz);
		ed_once = 1;
	}
	temp_open(0, "/hist/", _ft);
	temp_open(1, "/fm/", fm_ft);
	temp_open(2, "/sc/", _ft);
	term_init();
	ec_setbufsmax(NULL, NULL, "");
	xmpt = 0;
	return 0;
}

/* End the session and report it as nextvi's main() would */
static int ed_done(void)
{
	int st;
	term_done();
	st = xquit < -256 ? (abs(xquit) - 257) & 255 : abs(xquit) - 1;
	xquit = 0;
	ed_ungrabtty();
	return st;
}

/* The session state that is not a buffer: temporary buffers, registers,
 * the anchored-status tags of "??", the last search and the globals a
 * body may have changed. Dropped between blocks even when the buffers
 * themselves persist (replay mode), so no block inherits another's
 * register cache, tags or separators. */
static void ed_free_session(void)
{
	int i;
	for (i = 0; i < (int)LEN(tempbufs); i++) {
		free(tempbufs[i].path);
		lbuf_free(tempbufs[i].lb);
		memset(&tempbufs[i], 0, sizeof(tempbufs[i]));
	}
	for (i = 0; i < xregs_n; i++)
		if (xregs[i])
			sbuf_free(xregs[i])
	free(xregs);
	xregs = NULL;
	xregs_n = 0;
	if (xanchor) {
		sbuf_free(xanchor)
		xanchor = NULL;
	}
	if (xacreg) {
		sbuf_free(xacreg)
		xacreg = NULL;
	}
	rset_free(xkwdrs);
	xkwdrs = NULL;
	xrow = xoff = xtop = 0;
	xleft = 0;
	xquit = xgrec = xgdep = xexec_dep = 0;
	xkwddir = xkwdcnt = 0;
	xfr = xrr = xpr = xgrp = xdefreg = 0;
	xpret = NULL;
	xsep = ':';
	xesc = '\\';
	xerr = 1;
	xseq = 1;
	xvis = 0;
}

/* Drop every trace of the session: the state above plus the buffers (and
 * with them their marks and undo history). A block started after this
 * sees exactly what a freshly spawned editor sees. */
static void ed_free(void)
{
	for (int i = 0; i < xbufcur; i++)
		bufs_free(i);
	xbufcur = 0;
	free(bufs);
	bufs = NULL;
	ex_buf = ex_pbuf = ex_tpbuf = NULL;
	xbufsmax = 0;
	ed_free_session();
}

/* Run the embedded nextvi on in-memory buffers: text under name and, when
 * given, rejtext under rejname. The buffers never touch the filesystem:
 * the names are labels (referencing the original input where possible)
 * and the main buffer's final content is returned (heap-allocated)
 * however the session ended, saved or not. NULL on error. */
static void ed_loadbuf(const char *name, char *text)
{
	char msg[512];
	bufs_switch(bufs_open(name, strlen(name)));
	lbuf_edit(xb, text, 0, 0, 0, 0);
	ex_bufpostfix(ex_buf, 1);
	snprintf(msg, sizeof(msg), "\"%s\" %dL [f]", xb_path, lbuf_len(xb));
	ex_print(msg, bar_ft)
}

/* Hand the loaded buffers to the user and end the session. The buffers
 * outlive it, so the caller can read them back; ed_free() drops them. */
static int ed_run(void)
{
	char *ln;
	int st;
	syn_setft(xb_ft);
	if ((ln = getenv("P2VI_EX")))	/* test harness hook */
		ex_command(ln)
	vi(1);
	st = ed_done();
	if (st != 0)
		fprintf(stderr, "editor exited with error %d\n", st);
	return st;
}

/* A buffer's content as one heap-allocated string */
static char *lbuf_text(struct lbuf *lb)
{
	char *ln;
	sbuf_smake(sb, MAX_LINE)
	for (int i = 0; i < lbuf_len(lb); i++) {
		ln = lbuf_get(lb, i);
		sbuf_mem(sb, ln, lbuf_s(ln)->len + 1)
	}
	sbufn_ret(sb, sb->s)
}

static char *edit_buffers(const char *name, char *text,
			  const char *rejname, char *rejtext)
{
	if (ed_init(1) < 0)
		return NULL;
	ed_loadbuf(name, text);
	if (rejtext) {
		xmpt = 0;
		ed_loadbuf(rejname, rejtext);
	}
	if (ed_run() != 0)
		return NULL;
	return lbuf_text(bufs[0].lb);
}

/*
 * Interactive editing of all groups' search patterns in one buffer.
 * Opens the built-in nextvi once with all groups, everything held in RAM.
 * Pattern lines are shown regex-escaped (as they'll appear to the regex
 * engine). The buffer content is read back as-is when the editor exits —
 * no save needed, no modification tracking: an untouched buffer parses
 * back to the same customizations that were written into it.
 */
static void interactive_edit_all_files(file_patch_t **active, int nactive)
{
	if (nactive == 0)
		return;

	/* Buffer labels, not files: reference the original input (the patch,
	 * or the previously generated script under -d) when it has a name;
	 * .diff/.rej pick up nextvi's diff highlighting. */
	char bufname[288];
	char rejname[288];
	const char *base = input_file ? input_file : "patch2vi";
	snprintf(bufname, sizeof(bufname), "%s.p2v.diff", base);
	snprintf(rejname, sizeof(rejname), "%s.p2v.rej", base);
	/* Per-file in_fd lookup */
	file_delta_t **in_fd_per = ecalloc(nactive, sizeof(file_delta_t *));
	if (delta_mode) {
		for (int k = 0; k < nactive; k++)
			for (int di = 0; di < nin_deltas; di++)
				if (strcmp(in_deltas[di].filepath, active[k]->path) == 0) {
					in_fd_per[k] = &in_deltas[di];
					break;
				}
	}

	/* Auto-generated baseline (no injection) for later comparison */
	sbuf_smake(orig_sb, MAX_LINE)
	for (int k = 0; k < nactive; k++) {
		sb_printf(orig_sb, "=== FILE: %s ===\n", active[k]->path);
		write_groups_to_file(orig_sb,
				     active[k]->groups, active[k]->ngroups, NULL,
				     active[k]->is_new,
				     active[k]->orig_path ? active[k]->orig_path
				     : active[k]->path, 0);
		sb_printf(orig_sb, "%s\n\n", end_tag_wr);
	}
	sbuf_null(orig_sb)

	/* Rejection check: before building the editor buffer so we can clear
	 * has_star on rejected deltas (prevents custom_text injection). */
	sbuf *rej = NULL;
	for (int k = 0; k < nactive; k++) {
		file_delta_t *in_fd = in_fd_per[k];
		if (!in_fd)
			continue;
		int file_header_written = 0;
		for (int gi = 0; gi < in_fd->ngrps; gi++) {
			grp_delta_t *stored = &in_fd->grps[gi];
			int lvl = delta_mode > 0 ? delta_mode
				  : (stored->level ? stored->level : 2);
			int rejected = stored->group_idx > active[k]->ngroups;
			group_t *g = !rejected ? &active[k]->groups[stored->group_idx - 1] : NULL;
			switch (lvl) {
			case 1:
				break;
			case 2:
				if (rejected)
					break;
				if (stored->has_star && stored->level == 2)
					rejected = !grp_content_regex_matches(stored,
									      g->del_texts, g->ndel,
									      g->add_texts, g->nadd);
				else
					rejected = !grp_content_matches(stored,
									g->del_texts, g->ndel,
									g->add_texts, g->nadd);
				break;
			case 3:
				if (rejected)
					break;
				rejected = !grp_full_hunk_matches(stored,
								  g->all_pre_ctx, g->nall_pre_ctx,
								  g->del_texts, g->ndel,
								  g->add_texts, g->nadd,
								  g->post_ctx, g->npost_ctx);
				break;
			case 4:
				rejected = 1;
				for (int gi2 = 0; gi2 < active[k]->ngroups; gi2++) {
					group_t *g2 = &active[k]->groups[gi2];
					if ((stored->has_star && stored->level == 4
					     && grp_content_regex_matches(stored,
									  g2->del_texts, g2->ndel,
									  g2->add_texts, g2->nadd))
					    || grp_content_matches(stored,
								   g2->del_texts, g2->ndel,
								   g2->add_texts, g2->nadd)) {
						rejected = 0;
						break;
					}
				}
				break;
			case 5:
				rejected = 1;
				for (int gi2 = 0; gi2 < active[k]->ngroups; gi2++) {
					group_t *g2 = &active[k]->groups[gi2];
					if (grp_full_hunk_matches(stored,
								  g2->all_pre_ctx, g2->nall_pre_ctx,
								  g2->del_texts, g2->ndel,
								  g2->add_texts, g2->nadd,
								  g2->post_ctx, g2->npost_ctx)) {
						rejected = 0;
						break;
					}
				}
				break;
			}
			if (rejected) {
				stored->has_star = 0;
				if (!rej) {
					sbuf_make(rej, MAX_LINE)
					sb_printf(rej,
						  "# Rejected: index out of range"
						  " or content mismatch\n\n");
				}
				if (!file_header_written) {
					sb_printf(rej, "=== FILE: %s ===\n",
						  active[k]->path);
					file_header_written = 1;
				}
				emit_grp_delta(rej, stored);
			}
		}
		if (file_header_written && rej)
			sb_printf(rej, "%s\n\n", end_tag_wr);
	}

	/* -i (interactive, non-delta): every stored delta is rejected
	 * wholesale. in_fd_per stays NULL so nothing is injected or
	 * preserved; the deltas are dumped to the .rej buffer so the user
	 * can re-apply them by hand, mirroring -d's reject flow. */
	if (!delta_mode && nin_deltas) {
		sbuf_make(rej, MAX_LINE)
		sb_printf(rej, "# Rejected: interactive (-i)"
			  " discards all stored deltas\n\n");
		for (int di = 0; di < nin_deltas; di++) {
			file_delta_t *in_fd = &in_deltas[di];
			sb_printf(rej, "=== FILE: %s ===\n", in_fd->filepath);
			for (int gi = 0; gi < in_fd->ngrps; gi++)
				emit_grp_delta(rej, &in_fd->grps[gi]);
			sb_printf(rej, "%s\n\n", end_tag_wr);
		}
	}
	if (rej)
		sbuf_null(rej)

	/* First pass: structured sections only, stored delta injected
	 * (has_star was cleared for rejected groups above, so custom_text
	 * won't be written). Parsed right back and applied to the groups so
	 * the PHASE baselines carry the stored structured customizations
	 * exactly as an unedited session would emit them. */
	sbuf_smake(tmp_sb, MAX_LINE)
	for (int k = 0; k < nactive; k++) {
		sb_printf(tmp_sb, "=== FILE: %s ===\n", active[k]->path);
		write_groups_to_file(tmp_sb,
				     active[k]->groups, active[k]->ngroups,
				     in_fd_per[k], active[k]->is_new,
				     active[k]->orig_path ? active[k]->orig_path
				     : active[k]->path, 0);
		sb_printf(tmp_sb, "%s\n\n", end_tag_wr);
	}
	sbuf_null(tmp_sb)

	grp_delta_t **pre_per = emalloc(nactive * sizeof(grp_delta_t *));
	for (int k = 0; k < nactive; k++)
		pre_per[k] = ecalloc(active[k]->ngroups, sizeof(grp_delta_t));
	parse_grp_blob(tmp_sb->s, active, nactive, pre_per);
	for (int k = 0; k < nactive; k++) {
		for (int gi = 0; gi < active[k]->ngroups; gi++) {
			apply_grp_edits(&active[k]->groups[gi], &pre_per[k][gi]);
			free_grp(&pre_per[k][gi]);
		}
		free(pre_per[k]);
	}
	free(pre_per);

	/* Attach stored verbatim overrides before segment generation so their
	 * marks are reserved; matching mirrors the injection above. */
	for (int k = 0; k < nactive; k++) {
		if (!in_fd_per[k])
			continue;
		for (int gi = 0; gi < active[k]->ngroups; gi++) {
			group_t *g = &active[k]->groups[gi];
			if (!g->del_start && !g->nadd)
				continue;
			grp_delta_t *gd = find_grp_delta(in_fd_per[k], gi + 1,
							 g->del_texts, g->ndel,
							 g->add_texts, g->nadd,
							 g->all_pre_ctx, g->nall_pre_ctx,
							 g->post_ctx, g->npost_ctx,
							 delta_mode > 0 ? delta_mode : 0);
			if (!gd || (!gd->ph1 && !gd->ph2))
				continue;
			g->ph1_ovr = gd->ph1 ? uc_dup(gd->ph1) : NULL;
			g->ph2_ovr = gd->ph2 ? uc_dup(gd->ph2) : NULL;
			g->ovr_mark = gd->ovr_mark;
			g->ovr_esc = gd->ovr_esc;
			if (gd->ovr_esc != dyn_esc)
				fprintf(stderr, "%s group %d: verbatim override "
						"captured under escape byte %d, current is %d\n",
					active[k]->path, gi + 1, gd->ovr_esc, dyn_esc);
		}
	}

	for (int k = 0; k < nactive; k++)
		gen_group_segments(active[k]);

	/* Second pass: same content plus MARK and PHASE 1/2 sections. */
	sbufn_cut(tmp_sb, 0)
	for (int k = 0; k < nactive; k++) {
		sb_printf(tmp_sb, "=== FILE: %s ===\n", active[k]->path);
		write_groups_to_file(tmp_sb,
				     active[k]->groups, active[k]->ngroups,
				     in_fd_per[k], active[k]->is_new,
				     active[k]->orig_path ? active[k]->orig_path
				     : active[k]->path, 1);
		sb_printf(tmp_sb, "%s\n\n", end_tag_wr);
	}
	sbuf_null(tmp_sb)

	/* Open the built-in editor; take the buffer content back as-is */
	char *edited = edit_buffers(bufname, tmp_sb->s, rejname,
				    rej ? rej->s : NULL);
	free(tmp_sb->s);
	if (rej)
		sbuf_free(rej)
	if (!edited)
		goto cleanup_orig;

	/* Parse the edited buffer once per file slot */
	grp_delta_t **edit_per = emalloc(nactive * sizeof(grp_delta_t *));
	for (int k = 0; k < nactive; k++)
		edit_per[k] = ecalloc(active[k]->ngroups, sizeof(grp_delta_t));
	parse_grp_blob(edited, active, nactive, edit_per);
	free(edited);

	/* Compute structured delta: compare the auto-generated baseline
	 * against the edited buffer. Only store changed groups. An untouched
	 * session parses back to the injected customizations, so the stored
	 * delta is re-derived rather than specially preserved. */
	grp_delta_t **orig_per = emalloc(nactive * sizeof(grp_delta_t *));
	for (int k = 0; k < nactive; k++)
		orig_per[k] = ecalloc(active[k]->ngroups, sizeof(grp_delta_t));
	parse_grp_blob(orig_sb->s, active, nactive, orig_per);

	for (int k = 0; k < nactive; k++) {
		file_delta_t *od = NULL;
		for (int gi = 0; gi < active[k]->ngroups; gi++) {
			grp_delta_t *og = &orig_per[k][gi];
			grp_delta_t *eg = &edit_per[k][gi];

			int strat_ch = (eg->strategy != og->strategy);
			int pat_ch = 0;
			for (int pi = 0; pi < NSEARCH; pi++)
				if (!lines_equal(eg->pattern[pi], eg->npattern[pi],
						 og->pattern[pi], og->npattern[pi]) ||
				    eg->pat_has_off[pi] != og->pat_has_off[pi] ||
				    eg->pat_off[pi] != og->pat_off[pi] ||
				    eg->pat_has_mode[pi] != og->pat_has_mode[pi] ||
				    eg->pat_mode[pi] != og->pat_mode[pi])
					pat_ch = 1;
			int abs_ch = !lines_equal(eg->abs_cmd, eg->nabs,
						  og->abs_cmd, og->nabs);
			int rel_ch = !lines_equal(eg->rel_cmd, eg->nrel,
						  og->rel_cmd, og->nrel);
			int relc_ch = !lines_equal(eg->relc_cmd, eg->nrelc,
						   og->relc_cmd, og->nrelc);
			int custom_ch = !lines_equal(eg->custom_text, eg->ncustom_text,
						     og->custom_text, og->ncustom_text);
			int level_ch = eg->level != og->level;
			int struct_ch = strat_ch || pat_ch || abs_ch ||
					rel_ch || relc_ch || level_ch ||
					custom_ch;

			/* Verbatim PHASE edits are detected against the
			 * displayed bytes (override, else generated), so a
			 * structured-only edit leaves them untouched and the
			 * blobs regenerate from it next session. Latest-edited
			 * representation wins per group; tie goes to verbatim. */
			group_t *g = &active[k]->groups[gi];
			const char *d1 = g->ph1_ovr ? g->ph1_ovr : g->ph1_gen;
			const char *d2 = g->ph2_ovr ? g->ph2_ovr : g->ph2_gen;
			int verb_ch = (eg->ph1 && d1 && strcmp(eg->ph1, d1) != 0) ||
				      (eg->ph2 && d2 && strcmp(eg->ph2, d2) != 0);
			if (verb_ch) {
				if (struct_ch)
					fprintf(stderr, "%s group %d: structured "
							"edit shadowed by verbatim PHASE "
							"edit\n", active[k]->path, gi + 1);
				char *n1 = uc_dup(eg->ph1 ? eg->ph1 : (d1 ? d1 : ""));
				char *n2 = uc_dup(eg->ph2 ? eg->ph2 : (d2 ? d2 : ""));
				free(g->ph1_ovr);
				free(g->ph2_ovr);
				g->ph1_ovr = n1;
				g->ph2_ovr = n2;
				g->ovr_mark = eg->ovr_mark > 0 ? eg->ovr_mark
					      : g->mark_id;
				g->ovr_esc = dyn_esc;
			} else if (struct_ch && (g->ph1_ovr || g->ph2_ovr)) {
				discard_verbatim_ovr(active[k]->path, gi + 1,
						     g, rejname);
			}
			int has_ovr = g->ph1_ovr || g->ph2_ovr;

			if (!struct_ch && !has_ovr)
				continue;

			if (!od) {
				od = &out_deltas[nout_deltas++];
				od->filepath = uc_dup(active[k]->path);
				od->grps = NULL;
				od->ngrps = 0;
				od->gcap = 0;
			}
			if (od->ngrps >= od->gcap) {
				od->gcap = od->gcap ? od->gcap * 2 : 4;
				od->grps = erealloc(od->grps,
						    od->gcap * sizeof(grp_delta_t));
			}
			grp_delta_t *gout = &od->grps[od->ngrps++];
			memset(gout, 0, sizeof(*gout));
			gout->group_idx = gi + 1;
			gout->level = eg->level ? eg->level : 2;
			gout->has_star = eg->has_star;
			/* original del/add always from patch */
			arr_clone(&gout->del_lines, &gout->ndel_lines, &gout->del_cap,
				  active[k]->groups[gi].del_texts, active[k]->groups[gi].ndel);
			arr_clone(&gout->add_lines, &gout->nadd_lines, &gout->add_cap,
				  active[k]->groups[gi].add_texts, active[k]->groups[gi].nadd);
			arr_clone(&gout->pre_ctx, &gout->npre_ctx, &gout->pre_cap,
				  active[k]->groups[gi].all_pre_ctx, active[k]->groups[gi].nall_pre_ctx);
			arr_clone(&gout->post_ctx, &gout->npost_ctx, &gout->post_cap,
				  active[k]->groups[gi].post_ctx, active[k]->groups[gi].npost_ctx);
			/* customization from user's edits; kept even under a
			 * verbatim override — custom_text doubles as the
			 * group-locator regex for starred LEVEL 2/4 matching,
			 * so dropping it would degrade re-entry matching to
			 * index-only (or first-group at level 4). */
			if (custom_ch) {
				arr_clone(&gout->custom_text, &gout->ncustom_text, &gout->custom_text_cap,
					  eg->custom_text, eg->ncustom_text);
			} else if (in_fd_per[k]) {
				/* preserve existing customization from stored delta */
				grp_delta_t *stored = find_grp_delta(in_fd_per[k], gi + 1,
								     active[k]->groups[gi].del_texts, active[k]->groups[gi].ndel,
								     active[k]->groups[gi].add_texts, active[k]->groups[gi].nadd,
								     active[k]->groups[gi].all_pre_ctx, active[k]->groups[gi].nall_pre_ctx,
								     active[k]->groups[gi].post_ctx, active[k]->groups[gi].npost_ctx,
								     delta_mode > 0 ? delta_mode : 0);
				if (stored && stored->ncustom_text > 0) {
					arr_clone(&gout->custom_text, &gout->ncustom_text, &gout->custom_text_cap,
						  stored->custom_text, stored->ncustom_text);
				}
			}
			/* A verbatim override supersedes the structured edit
			 * customizations (strategy/patterns/commands): store
			 * the pair of blobs (both phases, so the group is
			 * frozen as one consistent unit) plus its mark and
			 * escape regime. */
			if (has_ovr) {
				gout->ph1 = uc_dup(g->ph1_ovr ? g->ph1_ovr
						   : (g->ph1_gen ? g->ph1_gen : ""));
				gout->ph2 = uc_dup(g->ph2_ovr ? g->ph2_ovr
						   : (g->ph2_gen ? g->ph2_gen : ""));
				gout->ovr_mark = g->ovr_mark > 0 ? g->ovr_mark
						 : g->mark_id;
				gout->ovr_esc = g->ovr_esc;
				continue;
			}
			if (strat_ch)
				gout->strategy = eg->strategy;
			if (pat_ch)
				for (int pi = 0; pi < NSEARCH; pi++) {
					arr_clone(&gout->pattern[pi], &gout->npattern[pi],
						  &gout->pat_cap[pi],
						  eg->pattern[pi], eg->npattern[pi]);
					gout->pat_off[pi] = eg->pat_off[pi];
					gout->pat_has_off[pi] = eg->pat_has_off[pi];
					gout->pat_mode[pi] = eg->pat_mode[pi];
					gout->pat_has_mode[pi] = eg->pat_has_mode[pi];
				}
			if (abs_ch)
				arr_clone(&gout->abs_cmd, &gout->nabs, &gout->abs_cap,
					  eg->abs_cmd, eg->nabs);
			if (rel_ch)
				arr_clone(&gout->rel_cmd, &gout->nrel, &gout->rel_cap,
					  eg->rel_cmd, eg->nrel);
			if (relc_ch)
				arr_clone(&gout->relc_cmd, &gout->nrelc, &gout->relc_cap,
					  eg->relc_cmd, eg->nrelc);
		}
	}

	for (int k = 0; k < nactive; k++) {
		for (int gi = 0; gi < active[k]->ngroups; gi++)
			free_grp(&orig_per[k][gi]);
		free(orig_per[k]);
	}
	free(orig_per);

	/* Apply edit_per to groups[], transferring ownership of arrays. */
	for (int k = 0; k < nactive; k++) {
		for (int gi = 0; gi < active[k]->ngroups; gi++) {
			apply_grp_edits(&active[k]->groups[gi], &edit_per[k][gi]);
			free_grp(&edit_per[k][gi]);
		}
		free(edit_per[k]);
	}
	free(edit_per);

cleanup_orig:
	free(orig_sb->s);
	free(in_fd_per);
}

/* Emit a custom EDIT COMMAND lines array + trailing SEP.
 * lines[0] = "cmd [first-content]", lines[1..n] = extra content lines.
 * s/pat/repl/: emit_escaped_exarg_only.
 * bare cmd (d, etc.): output verbatim. */
static void emit_custom_edit_lines(sbuf *out, char **lines, int nlines)
{
	if (nlines == 0)
		return;
	const char *first = lines[0];
	if (is_substitute(first)) {
		emit_escaped_exarg_only(out, first);
		return;
	}
	/* Find first space: split command prefix from inline content */
	const char *sp = strchr(first, ' ');
	if (sp) {
		int horiz = memchr(first, ';', sp - first) != NULL;
		sb_mem(out, first, sp - first);  /* command prefix verbatim */
		sb_chr(out, ' ');
		emit_escaped_text(out, sp + 1);     /* first content line escaped */
		if (!horiz)
			sb_chr(out, '\n');
		for (int k = 1; k < nlines; k++) {
			emit_escaped_text(out, lines[k]);
			sb_chr(out, '\n');
		}
	} else {
		/* No content (d, ,#+Nd, etc.) */
		sb_str(out, first);
	}
}

/* Process operations for one file and emit script */
/*
 * Build groups[] for a file from its ops[]. A group is a contiguous
 * sequence of deletes/adds with optional context anchors. Stored in
 * fp->groups (heap-allocated) for later interactive editing and emission.
 */
static void build_file_groups(file_patch_t *fp)
{
	if (fp->nops == 0)
		return;

	fp->groups = ecalloc(fp->nops + 1, sizeof(group_t));
	group_t *groups = fp->groups;
	int ngroups = 0;
	int i = 0;

	while (i < fp->nops) {
		group_t *g = &groups[ngroups];
		memset(g, 0, sizeof(group_t));

		/* Skip context lines, collecting up to 3 consecutive for relative mode */
		int last_ctx_line = 0;
		char *ctx_ring[3] = {NULL, NULL, NULL};
		int ctx_line_ring[3] = {0, 0, 0};
		int ctx_count = 0;
		/* For interactive mode: collect ALL context lines */
		char **all_ctx = NULL;
		int nall_ctx = 0;
		int all_ctx_cap = 0;
		while (i < fp->nops && fp->ops[i].type == 'c') {
			last_ctx_line = fp->ops[i].oline;
			/* Shift ring buffer */
			ctx_ring[0] = ctx_ring[1];
			ctx_line_ring[0] = ctx_line_ring[1];
			ctx_ring[1] = ctx_ring[2];
			ctx_line_ring[1] = ctx_line_ring[2];
			ctx_ring[2] = fp->ops[i].text;
			ctx_line_ring[2] = fp->ops[i].oline;
			ctx_count++;
			if (interactive_mode) {
				/* Reset on hunk boundary (gap in line numbers) */
				if (nall_ctx > 0 && fp->ops[i].oline !=
				    fp->ops[i-1].oline + 1)
					nall_ctx = 0;
				if (nall_ctx >= all_ctx_cap) {
					all_ctx_cap = all_ctx_cap ? all_ctx_cap * 2 : 16;
					all_ctx = erealloc(all_ctx, all_ctx_cap * sizeof(char*));
				}
				all_ctx[nall_ctx++] = fp->ops[i].text;
			}
			i++;
		}
		if (i >= fp->nops) {
			free(all_ctx);
			break;
		}

		/* Store anchor info for relative mode */
		if (last_ctx_line) {
			int first_change_line = fp->ops[i].oline;
			g->anchor_offset = first_change_line - last_ctx_line;
		}
		/* Store multi-line anchors (up to 3 consecutive context lines before change) */
		if (ctx_count >= 3) {
			g->anchors[0] = ctx_ring[0];
			g->anchors[1] = ctx_ring[1];
			g->anchors[2] = ctx_ring[2];
			g->nanchors = 3;
		} else if (ctx_count == 2) {
			g->anchors[0] = ctx_ring[1];
			g->anchors[1] = ctx_ring[2];
			g->nanchors = 2;
		} else if (ctx_count == 1) {
			g->anchors[0] = ctx_ring[2];
			g->nanchors = 1;
		}

		/* Record the enclosing @@ hunk span (for gen_win_window) */
		g->hunk_lo = fp->ops[i].hunk_lo;
		g->hunk_hi = fp->ops[i].hunk_hi;

		/* Collect consecutive deletes */
		int del_start_idx = i;
		if (fp->ops[i].type == 'd') {
			g->del_start = fp->ops[i].oline;
			g->del_end = fp->ops[i].oline;
			i++;
			while (i < fp->nops && fp->ops[i].type == 'd' &&
			       fp->ops[i].oline == g->del_end + 1) {
				g->del_end = fp->ops[i].oline;
				i++;
			}
		}
		g->ndel = i - del_start_idx;
		g->del_texts = emalloc(g->ndel * sizeof(char*));
		for (int j = 0; j < g->ndel; j++)
			g->del_texts[j] = fp->ops[del_start_idx + j].text;

		/* Collect consecutive adds */
		int add_start = i;
		while (i < fp->nops && fp->ops[i].type == 'a')
			i++;
		g->nadd = i - add_start;
		if (g->nadd > 0) {
			g->add_texts = emalloc(g->nadd * sizeof(char*));
			for (int j = 0; j < g->nadd; j++)
				g->add_texts[j] = fp->ops[add_start + j].text;
			if (g->del_start == 0) {
				/* Pure add - need to know where */
				g->add_after = fp->ops[add_start].oline - 1;
			}
		}

		/* Peek at following context for fallback */
		if (i < fp->nops && fp->ops[i].type == 'c') {
			g->follow_ctx = fp->ops[i].text;
			/* Distance from first change to following context */
			int first_change_line = g->del_start ? g->del_start : g->add_after + 1;
			g->follow_offset = fp->ops[i].oline - first_change_line;
		}

		/* Collect post-change context for fallback patterns. Both
		 * relative and interactive mode use up to 3 following context
		 * lines (default_pat_lines pi 0/1/4); without this, relative
		 * mode falls back to the single g->follow_ctx line and the
		 * lower post-context lines of pattern 1 are lost, drifting from
		 * the interactive output. */
		if ((relative_mode || interactive_mode) && (g->del_start || g->nadd)) {
			/* Peek at up to 3 following context lines */
			int post_cap = 3;
			int post_avail = 0;
			int pi = i;
			while (pi < fp->nops && fp->ops[pi].type == 'c' && post_avail < post_cap) {
				post_avail++;
				pi++;
			}
			if (post_avail > 0) {
				g->post_ctx = emalloc(post_avail * sizeof(char*));
				g->npost_ctx = post_avail;
				for (int j = 0; j < post_avail; j++)
					g->post_ctx[j] = fp->ops[i + j].text;
			}
		}
		/* Interactive mode: also retain all leading context and the
		 * block split point for editable SEARCH PATTERN sections. */
		if (interactive_mode && (g->del_start || g->nadd)) {
			g->all_pre_ctx = all_ctx;
			g->nall_pre_ctx = nall_ctx;
			g->block_change_idx = g->nall_pre_ctx;
		} else {
			free(all_ctx);
		}

		/* Precompute find_line_diff() for interactive mode */
		if (g->ndel == 1 && g->nadd == 1 &&
		    g->del_texts[0] && g->add_texts[0]) {
			g->has_line_diff = find_line_diff(
						   g->del_texts[0], g->add_texts[0],
						   &g->ld_old_text, &g->ld_new_text);
			if (g->has_line_diff) {
				/* Minimal diff positions for ;c (no uniqueness expansion) */
				const char *old = g->del_texts[0];
				const char *new = g->add_texts[0];
				int olen = strlen(old), nlen = strlen(new);
				/* rune-snapped like find_line_diff: ;c positions
				 * are rune indexes, so a split rune would shift
				 * ldc_start/ldc_end and splice invalid UTF-8 */
				int prefix = 0;
				while (old[prefix] && new[prefix] && old[prefix] == new[prefix])
					prefix++;
				while (prefix > 0 && (old[prefix] & 0xC0) == 0x80)
					prefix--;
				int suffix = 0;
				while (suffix < olen - prefix && suffix < nlen - prefix &&
				       old[olen-1-suffix] == new[nlen-1-suffix])
					suffix++;
				while (suffix > 0 && (old[olen - suffix] & 0xC0) == 0x80)
					suffix--;
				g->ldc_start = rune_count_n(old, prefix);
				g->ldc_end = rune_count_n(old, olen - suffix);
				int ns = prefix, ne = nlen - suffix;
				g->ldc_new_text = emalloc(ne - ns + 1);
				memcpy(g->ldc_new_text, new + ns, ne - ns);
				g->ldc_new_text[ne - ns] = '\0';
			}
		}

		if (g->del_start || g->nadd)
			ngroups++;
	}
	fp->ngroups = ngroups;
}

/*
 * Emit ex commands for one file's groups.
 * Caller must have built fp->groups via build_file_groups() and run
 * interactive editing if applicable.
 */
/* Free a group's heap data after emission */
static void free_group(group_t *g)
{
	free(g->del_texts);
	free(g->add_texts);
	free(g->all_pre_ctx);
	free(g->post_ctx);
	for (int pi = 0; pi < NSEARCH; pi++) {
		for (int k = 0; k < g->ncustom_pat[pi]; k++)
			free(g->custom_pat[pi][k]);
		free(g->custom_pat[pi]);
	}
	free(g->ld_old_text);
	free(g->ld_new_text);
	free(g->ldc_new_text);
	free(g->ph1_gen);
	free(g->ph2_gen);
	free(g->ph1_ovr);
	free(g->ph2_ovr);
}

/* Allocate the group's phase-1 mark: a verbatim override forces its reserved
 * mark id (the blobs reference it); everything else takes the next free id. */
static int group_mark_id(group_t *g, int *next_id)
{
	if ((g->ph1_ovr || g->ph2_ovr) && g->ovr_mark > 0)
		return g->ovr_mark;
	return next_mark_id(next_id);
}

/* Generate every group's verbatim phase-1/phase-2 segment bytes into
 * g->ph1_gen/g->ph2_gen (forward/relative layout only), regenerating from the
 * current structured state on every call. emit_file_script substitutes
 * g->ph1_ovr/g->ph2_ovr for the generated bytes at write time; groups carrying
 * an override still generate normally (their bytes are discarded) so
 * cross-group state - mark allocation, first-search flag - evolves exactly as
 * when the override was captured, with the mark id forced to the reserved
 * override mark. */
static void gen_group_segments(file_patch_t *fp)
{
	group_t *groups = fp->groups;
	int ngroups = fp->ngroups;

	cur_file_path = fp->path;

	/* Read the pre-patch original (if present) to validate anchor
	 * uniqueness; new files have no original to read. Prefer the "---"
	 * path (it names the pre-patch content); fall back to the edit
	 * target, which holds that same content before the script runs. */
	if (!fp->is_new)
		load_orig_file(fp->orig_path ? fp->orig_path : fp->path);

	/* Drop stale segments from a previous (pre-editor display) run and
	 * reserve every override's mark before any allocation. */
	nreserved_marks = 0;
	for (int gi = 0; gi < ngroups; gi++) {
		group_t *g = &groups[gi];
		free(g->ph1_gen);
		free(g->ph2_gen);
		g->ph1_gen = NULL;
		g->ph2_gen = NULL;
		if ((g->ph1_ovr || g->ph2_ovr) && g->ovr_mark > 0)
			reserve_mark(g->ovr_mark);
	}

	/*
	 * Phase 1 (resolve): run every group's search against the register
	 * cache yanked once after file open and record the target line in
	 * a mark. No edits happen here, so the cache never goes stale and
	 * a failed anchor aborts with the file untouched.
	 */
	/* edit marks start at 1; mark WIN_SAVE_MARK (0) is reserved as pattern
	 * 8's save/restore scratch around its global search */
	int next_id = WIN_SAVE_MARK + 1;
	int first_search = 1;
	for (int gi = 0; gi < ngroups; gi++) {
		group_t *g = &groups[gi];
		g->mark_id = -1;
		g->insert_i = 0;
		if (!g->del_start && !g->nadd)
			continue;
		sbuf_smake(out, MAX_LINE)
		int target_line = g->del_start ? g->del_start : g->add_after;

		/*
		 * Resolve strategy for this group.
		 * In non-interactive mode, strategy is determined by flags:
		 *   relative_mode -> REL, else ABS.
		 * In interactive mode, strategy comes from user selection
		 * (g->strategy), with STRAT_DEFAULT resolved here.
		 */
		int strat = g->strategy;

		int has_anchors = g->nanchors >= 2
				  || (g->nanchors == 1 && g->anchors[0] && g->anchors[0][0])
				  || (g->follow_ctx && g->follow_ctx[0])
				  || (g->ndel > 0 && g->del_texts[0] && g->del_texts[0][0]);

		if (!interactive_mode)
			strat = (relative_mode && has_anchors) ? STRAT_REL : STRAT_ABS;
		else if (strat == STRAT_DEFAULT)
			strat = has_anchors ? STRAT_REL : STRAT_ABS;

		/* Validate strategy, apply fallback chain */
		if (strat == STRAT_REL && !has_anchors)
			strat = STRAT_ABS;
		if (strat == STRAT_RELC) {
			if (!has_anchors)
				strat = STRAT_ABS;
			else if (!(g->ndel == 1 && g->nadd == 1 && g->has_line_diff))
				strat = STRAT_REL;  /* fall back to s// if no ;c data */
		}
		g->res_strat = strat;

		if (strat == STRAT_ABS) {
			/* Custom abs commands carry their own addresses */
			if (g->custom_abs_lines && g->custom_abs_nlines > 0)
				goto ph1_done;
			/* New file: empty buffer, nothing to mark; phase 2
			 * emits a bare i (mark_id stays -1) */
			if (fp->is_new && !g->del_start) {
				g->insert_i = 1;
				goto ph1_done;
			}
			int t = target_line;
			if (!g->del_start && t <= 0) {
				t = 1;
				g->insert_i = 1;
			}
			g->mark_id = group_mark_id(g, &next_id);
			sb_printf(out, "%dm %d", t, g->mark_id);
			EMIT_SEP(out);
			goto ph1_done;
		}

		/* Build the fallback pattern list: edited SEARCH PATTERN
		 * sections if any, else auto defaults. Duplicates dropped,
		 * first match wins at apply time. */
		pat_spec_t ps[NSEARCH];
		int nps = 0;
		char **raw = NULL;
		fuzzwin_t fz[NFUZZ];   /* owned fuzzed windows (plain -r path) */
		int nfz = 0;
		fuzzwin_t gw;          /* owned grp window (plain -r path) */
		int has_gw = 0;
		fuzzwin_t ww;          /* owned global straddle window (plain -r path) */
		int has_ww = 0;
		fuzzwin_t ww2;         /* owned farther straddle window (pattern 9) */
		int has_ww2 = 0;
		for (int pi = 0; pi < NSEARCH; pi++) {
			if (g->ncustom_pat[pi] == 0)
				continue;
			ps[nps].lines = g->custom_pat[pi];
			ps[nps].nlines = g->ncustom_pat[pi];
			ps[nps].pre_escaped = 1;
			/* Default offset (no explicit OFFSET marker) mirrors
			 * default_pat_lines: deletion-rooted slots (del+post,
			 * deleted only) start on the target line, post-only
			 * starts g->ndel lines below it, the rest anchor on
			 * leading context. */
			ps[nps].offset = g->custom_pat_has_off[pi]
					 ? g->custom_pat_off[pi]
					 : (pi == 1 || pi == 3) ? 0
					 : pi == 4 ? -(g->ndel)
					 : g->custom_offset;
			ps[nps].off_final = g->custom_pat_has_off[pi];
			ps[nps].mode = g->custom_pat_has_mode[pi]
				       ? g->custom_pat_mode[pi]
				       : g->ncustom_pat[pi] == 1 ? 1 : 0;
			ps[nps].pid = pi + 1;
			nps++;
		}
		if (nps == 0) {
			int slot_sz = g->ndel + 7;
			raw = emalloc(NPAT * slot_sz * sizeof(char *));
			for (int pi = 0; pi < NPAT; pi++) {
				char **slot = raw + pi * slot_sz;
				int doff;
				int n = default_pat_lines(g, pi, slot, &doff);
				if (!n)
					continue;
				ps[nps].lines = slot;
				ps[nps].nlines = n;
				ps[nps].pre_escaped = 0;
				ps[nps].offset = doff;
				ps[nps].off_final = 0;
				ps[nps].mode = n == 1 ? 1 : 0;
				ps[nps].pid = pi + 1;
				nps++;
			}
			/* File-validated relaxed windows appended loosest-last
			 * (fuzz, grp 7 mode 2, straddle 8/9 mode 3). off_final on the
			 * latter three preserves their offsets through the pure-add
			 * shift. Interactive mode surfaces these as custom_pat instead;
			 * this is the plain -r path. */
			nfz = gen_fuzz_windows(g, fz, NFUZZ);
			for (int i = 0; i < nfz && nps < NSEARCH; i++)
				nps = push_win_pat(ps, nps, &fz[i], NPAT + i + 1, 0);
			has_gw = nps < NSEARCH && gen_grp_window(g, &gw);
			if (has_gw)
				nps = push_win_pat(ps, nps, &gw, GRP_SLOT + 1, 1);
			has_ww = nps < NSEARCH && gen_win_window(g, &ww, 0);
			if (has_ww)
				nps = push_win_pat(ps, nps, &ww, WIN_SLOT + 1, 1);
			has_ww2 = nps < NSEARCH && gen_win_window(g, &ww2, 1);
			if (has_ww2)
				nps = push_win_pat(ps, nps, &ww2, WIN2_SLOT + 1, 1);
			/* No re-sort: default_pat_lines already orders strict to loose
			 * and every pattern is file-proven, so order only picks the
			 * winner on a drifted apply. The -i chain emits in this same
			 * slot order, so sorting only -r would diverge the modes. */
		}
		int w = 0;
		for (int pi = 0; pi < nps; pi++) {
			int dup = 0;
			for (int pj = 0; pj < w; pj++)
				if (ps[pi].pre_escaped == ps[pj].pre_escaped &&
				    lines_equal(ps[pi].lines, ps[pi].nlines,
						ps[pj].lines, ps[pj].nlines))
					dup = 1;
			if (!dup)
				ps[w++] = ps[pi];
		}
		nps = w;

		/* Pure insert: position lands on the line to append after.
		 * Custom edit lines carry their own verb and a verb-relative
		 * offset (the displayed "+Ni" already includes the insert step),
		 * so no adjustment is applied for them. */
		if (!g->del_start && g->nadd
		    && !(g->custom_rel_lines && g->custom_rel_nlines > 0)) {
			if (g->add_after <= 0)
				g->insert_i = 1;
			else
				for (int pi = 0; pi < nps; pi++)
					if (!ps[pi].off_final)
						ps[pi].offset -= 1;
		}

		g->mark_id = group_mark_id(g, &next_id);
		if (nps == 0) {
			/* No usable anchor: mark the absolute line */
			sb_printf(out, "%dm %d",
				  target_line > 0 ? target_line : 1, g->mark_id);
			EMIT_SEP(out);
			free_fuzz_windows(fz, nfz);
			if (has_gw)
				free_fuzz_windows(&gw, 1);
			if (has_ww)
				free_fuzz_windows(&ww, 1);
			if (has_ww2)
				free_fuzz_windows(&ww2, 1);
			free(raw);
			goto ph1_done;
		}
		if (nps == 1)
			emit_search(out, ps[0].lines, ps[0].nlines,
				    ps[0].offset, g->mark_id, target_line,
				    ps[0].pre_escaped, first_search, ps[0].mode);
		else
			emit_fallback_chain(out, ps, nps, g->mark_id,
					    target_line, first_search);
		first_search = 0;
		free_fuzz_windows(fz, nfz);
		if (has_gw)
			free_fuzz_windows(&gw, 1);
		if (has_ww)
			free_fuzz_windows(&ww, 1);
		if (has_ww2)
			free_fuzz_windows(&ww2, 1);
		free(raw);
ph1_done:
		sbuf_null(out)
		g->ph1_gen = out->s;
	}

	/*
	 * Phase 2 (commit): apply edits at the marks, forward order.
	 * Marks auto-adjust as edits shift lines above them.
	 */

	/* Helper: emit a custom edit command (lines array) at the mark.
	 * Substitute (lines[0] starts s+non-alnum): exarg escaping.
	 * Otherwise: verbs attach directly to the mark address. A nonzero
	 * custom_offset (a +N/-N pulled off the EDIT COMMAND verb) rides on
	 * the mark address as "'N+off", so an insert-above-line-1 ("'N-1i",
	 * resolving to the line-0 insert) survives the template round-trip
	 * instead of being lost against the patterns' explicit OFFSETs. */
#define EMIT_MARK_EDIT(rlines, rnlines) do { \
		if (g->custom_offset) \
			sb_printf(out, "'%d%+d", g->mark_id, g->custom_offset); \
		else \
			sb_printf(out, "'%d", g->mark_id); \
		if (is_substitute((rlines)[0])) { \
			emit_escaped_exarg_only(out, (rlines)[0]); \
			EMIT_SEP(out); \
		} else { \
			emit_custom_edit_lines(out, (rlines), (rnlines)); \
			EMIT_SEP(out); \
		} \
		emit_err_check_mark(out, tline, g->mark_id); \
} while (0)

	for (int gi = 0; gi < ngroups; gi++) {
		group_t *g = &groups[gi];
		if (!g->del_start && !g->nadd)
			continue;
		sbuf_smake(out, MAX_LINE)
		int strat = g->res_strat;
		int tline = g->del_start ? g->del_start : g->add_after;
		sb_str(out, "${LB}\n");
		EMIT_SEP(out);

		/* Custom abs/rel edit commands apply regardless of del/add shape */
		if (strat == STRAT_ABS && g->custom_abs_lines) {
			emit_custom_edit_lines(out, g->custom_abs_lines,
					       g->custom_abs_nlines);
			EMIT_SEP(out);
			emit_err_check_mark(out, tline, g->mark_id);
		} else if (strat == STRAT_REL && g->custom_rel_lines
			   && g->custom_rel_nlines > 0) {
			/* A multi-line rel block of pure substitutes is the
			 * editable substitute progression: rebuild it as a
			 * first-wins chain. Anything else (single command, or a
			 * multi-line c with content) emits verbatim at the mark. */
			subvar_t cv[NSEARCH];
			int cn = 0, all_sub = g->custom_rel_nlines > 1;
			for (int k = 0; all_sub && k < g->custom_rel_nlines
			     && cn < NSEARCH; k++) {
				if (parse_sub_line(g->custom_rel_lines[k],
						   &cv[cn].pat, &cv[cn].repl)) {
					cv[cn].sid = cn + 1;
					cn++;
				} else
					all_sub = 0;
			}
			if (all_sub && cn == g->custom_rel_nlines) {
				emit_substitute_chain(out, tline, g->mark_id,
						      cv, cn);
			} else {
				EMIT_MARK_EDIT(g->custom_rel_lines,
					       g->custom_rel_nlines);
			}
			for (int k = 0; k < cn; k++) {
				free(cv[k].pat);
				free(cv[k].repl);
			}
		} else if (g->del_start && g->nadd) {
			if (strat == STRAT_RELC) {
				if (g->custom_relc_lines && g->custom_relc_nlines > 0) {
					/* custom relc lines address the current
					 * line (".;A;Bc"): jump to the mark first */
					sb_printf(out, "'%d", g->mark_id);
					EMIT_SEP(out);
					emit_custom_edit_lines(out, g->custom_relc_lines,
							       g->custom_relc_nlines);
					EMIT_SEP(out);
				} else {
					sb_printf(out, "'%d", g->mark_id);
					emit_horiz_tail(out, g);
				}
				emit_err_check_mark(out, tline, g->mark_id);
			} else if (strat == STRAT_REL && g->ndel == 1 && g->nadd == 1
				   && g->has_line_diff) {
				emit_mark_substitute(out, tline, g->mark_id, g);
			} else if (strat == STRAT_ABS && g->ndel == 1 && g->nadd == 1
				   && g->has_line_diff) {
				sb_printf(out, "'%d", g->mark_id);
				emit_horiz_tail(out, g);
				emit_err_check_mark(out, tline, g->mark_id);
			} else {
				emit_mark_change(out, tline, g->mark_id,
						 g->ndel, g->add_texts, g->nadd);
			}
		} else if (g->del_start) {
			emit_mark_delete(out, tline, g->mark_id, g->ndel);
		} else if (g->nadd) {
			emit_mark_insert(out, tline, g->mark_id, g->insert_i,
					 g->add_texts, g->nadd);
		}
		sbuf_null(out)
		g->ph2_gen = out->s;
	}
	free_orig_file();
}

static void emit_file_script(sbuf *out, file_patch_t *fp)
{
	if (fp->ngroups == 0)
		return;

	group_t *groups = fp->groups;
	int ngroups = fp->ngroups;
	int forward = relative_mode || interactive_mode;

	if (!forward) {
		/* Absolute mode: reverse order (bottom-to-top) preserves
		 * line numbers; no searches, no marks. */
		for (int gi = ngroups - 1; gi >= 0; gi--) {
			group_t *g = &groups[gi];
			if (g->del_start && g->nadd) {
				if (g->ndel == 1 && g->nadd == 1 && g->has_line_diff)
					emit_horizontal_change(out, g->del_start,
							       g->ldc_start, g->ldc_end,
							       g->ldc_new_text);
				else
					emit_change(out, g->del_start, g->del_end,
						    g->add_texts, g->nadd);
			} else if (g->del_start) {
				emit_delete(out, g->del_start, g->del_end);
			} else if (g->nadd) {
				emit_insert_after(out, g->add_after,
						  g->add_texts, g->nadd,
						  fp->is_new);
			}
			free_group(g);
		}
		return;
	}

	/* Segments regenerate from the current structured state; a verbatim
	 * override replaces its group's generated bytes at write time. */
	gen_group_segments(fp);
	for (int gi = 0; gi < ngroups; gi++) {
		group_t *g = &groups[gi];
		const char *seg = g->ph1_ovr ? g->ph1_ovr : g->ph1_gen;
		if (seg)
			sb_str(out, seg);
	}
	for (int gi = 0; gi < ngroups; gi++) {
		group_t *g = &groups[gi];
		const char *seg = g->ph2_ovr ? g->ph2_ovr : g->ph2_gen;
		if (seg)
			sb_str(out, seg);
	}
	for (int gi = 0; gi < ngroups; gi++)
		free_group(&groups[gi]);
}

/* set by "--- /dev/null", consumed by the next "+++" */
static int pending_is_new;
/* "---" path (pre-patch original), consumed by the next "+++" */
static char *pending_orig_path;

static void new_file(const char *path)
{
	if (nfiles >= (int)(sizeof(files) / sizeof(files[0]))) {
		fprintf(stderr, "too many files\n");
		exit(1);
	}
	files[nfiles].path = uc_dup(path);
	files[nfiles].nops = 0;
	files[nfiles].is_new = pending_is_new;
	files[nfiles].orig_path = pending_orig_path;
	pending_is_new = 0;
	pending_orig_path = NULL;
	nfiles++;
	/* path appears in the FAIL <path>:<line> error message inside EXINIT */
	mark_bytes_used(path);
}

/* Original-line span of the @@ hunk currently being parsed (0 = none yet). */
static int cur_hunk_lo, cur_hunk_hi;

static void add_op(int type, int oline, const char *text)
{
	if (nfiles == 0)
		return;
	file_patch_t *fp = &files[nfiles - 1];
	if (fp->nops >= MAX_OPS) {
		fprintf(stderr, "too many operations\n");
		exit(1);
	}
	fp->ops[fp->nops].type = type;
	fp->ops[fp->nops].oline = oline;
	fp->ops[fp->nops].text = text ? uc_dup(text) : NULL;
	fp->ops[fp->nops].hunk_lo = cur_hunk_lo;
	fp->ops[fp->nops].hunk_hi = cur_hunk_hi;
	fp->nops++;

	/* Track bytes used in patch content */
	if (text)
		mark_bytes_used(text);
}

/*
 * -e: run a generated patch2vi script through the embedded editor, no
 * shell involved. The script's grammar is closed and self-generated —
 * header assignments, one printf'd ex body per editor invocation and the
 * "$VI -e <files> $P2VIF" line that runs it — so it is parsed exactly,
 * and anything outside that grammar is an error rather than a
 * best-effort guess. Each block gets its own editor lifetime, mirroring
 * the separate $VI process the shell would spawn.
 */

static int exec_mode;		/* -e: execute the input script */
static const char *exec_script;	/* its path, i.e. the script's $0 */

/* Shell variables assigned by the script header. Looked up before the
 * environment, so a header assignment shadows an inherited value while
 * the header's own conditionals still test the inherited one. */
typedef struct {
	char *name;
	char *val;
} shvar_t;

static shvar_t shvars[64];
static int nshvars;

static const char *sh_get(const char *name)
{
	for (int i = 0; i < nshvars; i++)
		if (!strcmp(shvars[i].name, name))
			return shvars[i].val;
	return getenv(name);
}

/* Header assignments belong to one script; a session replaying two of
 * them (-po) must not let the first script's assignments shadow the
 * environment while the second's conditionals are read. */
static void sh_reset(void)
{
	for (int i = 0; i < nshvars; i++) {
		free(shvars[i].name);
		free(shvars[i].val);
	}
	nshvars = 0;
}

static void sh_set(const char *name, const char *val)
{
	for (int i = 0; i < nshvars; i++)
		if (!strcmp(shvars[i].name, name)) {
			free(shvars[i].val);
			shvars[i].val = uc_dup(val);
			return;
		}
	if (nshvars == LEN(shvars)) {
		fprintf(stderr, "-e: too many shell variables\n");
		exit(1);
	}
	shvars[nshvars].name = uc_dup(name);
	shvars[nshvars].val = uc_dup(val);
	nshvars++;
}

/* byte-exact substring; uc_sub() counts characters, not bytes */
static char *str_dupn(const char *s, int n)
{
	char *r = emalloc(n + 1);
	memcpy(r, s, n);
	r[n] = '\0';
	return r;
}

static int sh_err(const char *what, const char *s)
{
	fprintf(stderr, "-e: unsupported %s: %s\n", what, s);
	return -1;
}

/* Expand one double-quoted shell word: ${VAR}, ${VAR:-default} (nestable,
 * as ${DBG1:-...${QF1}} is), $VAR, $0, $(printf '\NNN') and the escapes
 * that survive double quotes. Everything else is literal. */
static int sh_expand(const char *s, sbuf *out)
{
	char name[128];
	int j;
	while (*s) {
		if (*s == '\\' && (s[1] == '$' || s[1] == '"' || s[1] == '\\'
				   || s[1] == '`')) {
			sbuf_chr(out, s[1])
			s += 2;
			continue;
		}
		if (*s != '$') {
			sbuf_chr(out, *s++)
			continue;
		}
		s++;
		if (*s == '(') {	/* $(printf 'escapes') */
			if (strncmp(s, "(printf '", 9))
				return sh_err("substitution", s);
			for (s += 9; *s && *s != '\''; ) {
				if (*s != '\\') {
					sbuf_chr(out, *s++)
					continue;
				}
				s++;
				if (*s >= '0' && *s <= '7') {
					int v = 0;
					for (j = 0; j < 3 && *s >= '0'
					     && *s <= '7'; j++, s++)
						v = v * 8 + (*s - '0');
					sbuf_chr(out, v)
					continue;
				}
				if (*s == 'n')
					sbuf_chr(out, '\n')
				else if (*s == 't')
					sbuf_chr(out, '\t')
				else if (*s == '\\' || *s == '\'')
					sbuf_chr(out, *s)
				else
					return sh_err("printf escape", s - 1);
				s++;
			}
			if (strncmp(s, "')", 2))
				return sh_err("substitution", s);
			s += 2;
			continue;
		}
		if (*s == '0') {	/* $0: the script itself */
			s++;
			sbuf_str(out, exec_script ? exec_script : "patch2vi")
			continue;
		}
		if (*s == '{') {
			const char *val;
			s++;
			for (j = 0; *s && (isalnum((unsigned char)*s)
					   || *s == '_'); s++)
				if (j < (int)sizeof(name) - 1)
					name[j++] = *s;
			name[j] = '\0';
			if (!j)
				return sh_err("expansion", s);
			val = sh_get(name);
			if (*s == '}') {
				s++;
				if (val)
					sbuf_str(out, val)
				continue;
			}
			if (s[0] != ':' || s[1] != '-')
				return sh_err("expansion", s);
			s += 2;
			/* the default runs to the matching brace and is
			 * skipped whether or not it is the one used */
			const char *b = s;
			for (int depth = 1; depth; ) {
				if (!*s)
					return sh_err("expansion", b);
				else if (s[0] == '$' && s[1] == '{') {
					depth++;
					s += 2;
				} else if (*s == '}') {
					if (!--depth)
						break;
					s++;
				} else
					s++;
			}
			if (val && *val) {
				sbuf_str(out, val)
			} else {
				sbuf_smake(def, MAX_LINE)
				sbuf_mem(def, b, s - b)
				sbuf_null(def)
				j = sh_expand(def->s, out);
				free(def->s);
				if (j < 0)
					return -1;
			}
			s++;	/* the closing brace */
			continue;
		}
		if (isalpha((unsigned char)*s) || *s == '_') {
			const char *val;
			for (j = 0; *s && (isalnum((unsigned char)*s)
					   || *s == '_'); s++)
				if (j < (int)sizeof(name) - 1)
					name[j++] = *s;
			name[j] = '\0';
			if ((val = sh_get(name)))
				sbuf_str(out, val)
			continue;
		}
		return sh_err("expansion", s - 1);
	}
	return 0;
}

/* NAME=value, with value optionally double quoted */
static int sh_assign(const char *s)
{
	char name[128];
	int j, ret;
	for (j = 0; *s && (isalnum((unsigned char)*s) || *s == '_'); s++)
		if (j < (int)sizeof(name) - 1)
			name[j++] = *s;
	name[j] = '\0';
	if (!j || *s != '=')
		return sh_err("assignment", s);
	s++;
	sbuf_smake(val, MAX_LINE)
	if (*s == '"') {
		int n = strlen(s);
		if (n < 2 || s[n - 1] != '"') {
			free(val->s);
			return sh_err("assignment", s);
		}
		sbuf_mem(val, s + 1, n - 2)
		sbuf_null(val)
		s = val->s;
	}
	sbuf_smake(out, MAX_LINE)
	if (!(ret = sh_expand(s, out))) {
		sbuf_null(out)
		sh_set(name, out->s);
	}
	free(out->s);
	free(val->s);
	return ret;
}

/* [ "$X" = "1" ] && A=v || B=w */
static int sh_cond(const char *s)
{
	char name[128];
	const char *p, *alt;
	char *cmp, *pick;
	int j, ret;
	if (strncmp(s, "[ \"$", 4))
		return sh_err("conditional", s);
	for (s += 4, j = 0; *s && (isalnum((unsigned char)*s) || *s == '_'); s++)
		if (j < (int)sizeof(name) - 1)
			name[j++] = *s;
	name[j] = '\0';
	if (strncmp(s, "\" = \"", 5))
		return sh_err("conditional", s);
	s += 5;
	if (!(p = strstr(s, "\" ] && ")))
		return sh_err("conditional", s);
	cmp = str_dupn(s, p - s);
	p += 7;
	/* the last " || " separates the arms; a quoted value may contain
	 * anything else but never that */
	for (alt = NULL, s = p; (s = strstr(s, " || ")); s += 4)
		alt = s;
	if (!alt) {
		free(cmp);
		return sh_err("conditional", p);
	}
	s = sh_get(name);
	pick = !strcmp(s ? s : "", cmp) ? str_dupn(p, alt - p)
		: uc_dup((char *)alt + 4);
	ret = sh_assign(pick);
	free(pick);
	free(cmp);
	return ret;
}

typedef struct {
	char **paths;	/* real files, in $VI argument order */
	int npaths;
	char *body;	/* the printf'd ex command body */
	int sep;	/* the script's separator byte, its commands' delimiter */
} p2vi_block_t;

/* One editor lifetime: the files as b0..bN-1, then the body. EXINIT only
 * exists to lift the body out of the buffer the shell had to pass it in;
 * -e holds the body already, so it fills the register the body may recurse
 * through and runs the chain itself. */
static int run_body(p2vi_block_t *blk)
{
	int st;
	if (ed_init(0) < 0)
		return -1;
	xvis |= 2;
	xbufsalloc = MAX(blk->npaths, xbufsalloc);
	ec_setbufsmax(NULL, NULL, "");
	for (int i = 0; i < blk->npaths; i++) {
		xmpt = 0;
		ec_edit("", "e", blk->paths[i]);
	}
	xmpt = 0;
	xvis &= ~4;
	ex_regput(P2VI_REG, blk->body, 0);
	ex_exec(blk->body);
	if (!xquit)
		ex();
	st = ed_done();
	ed_free();
	return st;
}

static void free_block(p2vi_block_t *blk)
{
	for (int i = 0; i < blk->npaths; i++)
		free(blk->paths[i]);
	free(blk->paths);
	free(blk->body);
	memset(blk, 0, sizeof(*blk));
}

/* The script's separator byte, the delimiter of the body's commands. Read
 * per block at parse time: a replay may span two scripts, each with its own
 * header, and the bodies keep their own separator once expanded. */
static int sh_sepbyte(void)
{
	const char *s = sh_get("SEP");
	if (!s || !s[0] || s[1]) {
		fprintf(stderr, "replay: script has no single-byte SEP\n");
		return -1;
	}
	return (unsigned char)s[0];
}

/* EXINIT='<init>' $VI -e 'file' ... "$P2VIF"; the init is the fixed one
 * emit_script() writes and -e supplies its effect itself, so it is only
 * checked, never interpreted */
static int parse_vi_call(const char *s, p2vi_block_t *blk)
{
	const char *p;
	if (strncmp(s, P2VI_VICALL, strlen(P2VI_VICALL)))
		return sh_err("vi call", s);
	s += strlen(P2VI_VICALL);
	if (strncmp(s, " $VI -e", 7))
		return sh_err("vi call", s);
	for (s += 7; *s; ) {
		if (*s == ' ') {
			s++;
			continue;
		}
		if (!strncmp(s, "\"$P2VIF\"", 8)) {
			s += 8;
			continue;
		}
		if (*s != '\'' || !(p = strchr(s + 1, '\'')))
			return sh_err("vi call", s);
		blk->paths = erealloc(blk->paths,
				      (blk->npaths + 1) * sizeof(char *));
		blk->paths[blk->npaths++] = str_dupn(s + 1, p - s - 1);
		s = p + 1;
	}
	return 0;
}

/* Read the script's executable region (everything before "exit 0") into
 * one block per editor invocation. Parsing is separate from running: -e
 * runs each block in its own editor lifetime, while the compat session
 * replays them all in one, and that needs the whole list up front. */
static int parse_p2vi_script(FILE *in, p2vi_block_t **blks, int *nblks)
{
	char line[MAX_LINE];
	const char *body_end = " > \"$P2VIF\"";
	p2vi_block_t blk = {0};
	int skip = 0, in_body = 0, ret = 0, j;
	sbuf_smake(body, MAX_LINE)
	while (ret >= 0 && fgets(line, sizeof(line), in)) {
		char *seg = line;
		chomp(line);
		if (!in_body && !strncmp(line, "printf '%s\\n' \"", 15)) {
			sbuf_cut(body, 0)
			in_body = 1;
			seg = line + 15;
		}
		if (in_body) {
			/* the body ends at the closing quote of the printf
			 * argument; inside it a quote is always escaped */
			int n = strlen(seg);
			int el = strlen(body_end);
			if (n >= el + 1 && seg[n - el - 1] == '"'
			    && !strcmp(seg + n - el, body_end)) {
				seg[n - el - 1] = '\0';
				in_body = 0;
			}
			sbuf_str(body, seg)
			sbuf_chr(body, '\n')
			continue;
		}
		if (!strcmp(line, "exit 0"))
			break;
		if (skip) {
			if (!strcmp(line, "fi"))
				skip--;
			else if (!strncmp(line, "if ", 3))
				skip++;
			continue;
		}
		if (!line[0] || line[0] == '#')
			continue;
		if (!strncmp(line, "if ", 3)) {
			skip++;
			continue;
		}
		/* the temp file the body would be staged in, and its trap:
		 * -e keeps the body in RAM, so both are moot */
		if (!strncmp(line, "( : > ", 6) || !strncmp(line, "trap ", 5))
			continue;
		if (!strncmp(line, "EXINIT=", 7)) {
			sbuf_null(body)
			if ((ret = parse_vi_call(line, &blk)) < 0)
				break;
			sbuf_smake(exp, MAX_LINE)
			if (!(ret = sh_expand(body->s, exp))) {
				sbuf_null(exp)
				blk.body = uc_dup(exp->s);
				/* -e never needs it, but reading it here keeps
				 * every block self-contained for the replay */
				blk.sep = sh_sepbyte();
				*blks = erealloc(*blks, (*nblks + 1)
						 * sizeof(**blks));
				(*blks)[(*nblks)++] = blk;
				memset(&blk, 0, sizeof(blk));
			} else
				free_block(&blk);
			free(exp->s);
			sbuf_cut(body, 0)
			continue;
		}
		/* the name of a leading NAME=value assignment */
		j = 0;
		while (isalnum((unsigned char)line[j]) || line[j] == '_')
			j++;
		if (line[0] == '[')
			ret = sh_cond(line);
		else if (j && line[j] == '=')
			ret = sh_assign(line);
		else
			ret = sh_err("command", line);
	}
	free(body->s);
	if (in_body && ret >= 0)
		ret = sh_err("body", "unterminated printf");
	return ret;
}

static void free_blocks(p2vi_block_t *blks, int nblks)
{
	for (int i = 0; i < nblks; i++)
		free_block(&blks[i]);
	free(blks);
}

/* -e: every block in order, each in its own editor lifetime, stopping at
 * the first failure the way "sh -e" does. */
static int exec_p2vi_script(FILE *in)
{
	p2vi_block_t *blks = NULL;
	int nblks = 0, st = 0, ret = parse_p2vi_script(in, &blks, &nblks);
	for (int i = 0; ret >= 0 && i < nblks; i++) {
		if ((st = run_body(&blks[i])) < 0)
			ret = -1;
		if (st)
			break;
	}
	free_blocks(blks, nblks);
	return ret < 0 ? 1 : st;
}

/*
 * Replay: the same blocks, but as one editor session. Deriving a
 * compatibility patch means seeing the tree an origin script leaves
 * behind, so the buffers persist across blocks (a later block naming a
 * file an earlier one edited switches to the edited buffer, ec.c's
 * bufs_find path, with no disk round-trip), nothing is ever written, and
 * the last block hands the session to the user. Everything that is not a
 * buffer is still reset per block, as it is under -e: an editor the shell
 * spawned per block shares no register cache, "??" tag or separator with
 * the one before it.
 */

#define BODY_DELIM(c) ((c) == sep || (c) == '\n')

/* Drop the body's trailing writes: "b<N> SEP w" per file and the final
 * "2q". Parsed from the end, since "vis 2" (which stays) also occurs
 * inside the QF1/QF2/INTR fragments and so anchors nothing. */
static int strip_body_tail(char *body, int sep)
{
	int n = strlen(body), s;
	while (n && (body[n - 1] == '\n' || body[n - 1] == sep))
		n--;
	for (s = n; s && !BODY_DELIM(body[s - 1]); s--);
	if (n - s != 2 || strncmp(body + s, "2q", 2))
		return sh_err("body", "no trailing quit");
	for (n = s ? s - 1 : 0; n > 0; ) {
		for (s = n; s && !BODY_DELIM(body[s - 1]); s--);
		if (n - s != 1 || body[s] != 'w')
			break;
		n = s ? s - 1 : 0;
		for (s = n; s && !BODY_DELIM(body[s - 1]); s--);
		if (n - s < 2 || body[s] != 'b')
			return sh_err("body", "write without a buffer");
		n = s ? s - 1 : 0;
	}
	body[n] = '\0';
	return 0;
}

/* b<N> indexes the block's own file list, but a replay session's buffer
 * indices are session-global: a file another block opened first keeps its
 * index here. Rewrite the tokens with the session's own numbers. Only a
 * whole command counts as one, so the literal ":b0:" inside INTR (whose
 * commands are colon-separated) is left alone. */
static char *remap_bufnums(const char *body, int sep, int *bmap, int nmap)
{
	const char *s = body, *e, *d;
	sbuf_smake(out, MAX_LINE)
	while (*s) {
		for (e = s; *e && !BODY_DELIM(*e); e++);
		for (d = s + 1; d < e && isdigit((unsigned char)*d); d++);
		if (*s == 'b' && d > s + 1 && d == e) {
			int n = atoi(s + 1);
			if (n >= nmap) {
				free(out->s);
				sh_err("body", "buffer out of range");
				return NULL;
			}
			sb_printf(out, "b%d", bmap[n]);
		} else
			sbuf_mem(out, s, e - s)
		if (*e)
			sbuf_chr(out, *e++)
		s = e;
	}
	sbufn_ret(out, out->s)
}

/* The session's buffer index for a path, opening it if this is the first
 * block to name it. Mirrors bufs_open()'s append order, so the index the
 * body sees is the index the editor uses. */
static int sess_buf(char ***paths, int *npaths, const char *path)
{
	for (int i = 0; i < *npaths; i++)
		if (!strcmp((*paths)[i], path))
			return i;
	*paths = erealloc(*paths, (*npaths + 1) * sizeof(char *));
	(*paths)[*npaths] = uc_dup(path);
	return (*npaths)++;
}

/* Run every block in one session. With handover, the last block leaves
 * the editor to the user (a full vi(1), on the terminal) instead of
 * returning at the end of its body. The session's buffers are left alive
 * for the caller to read back; ed_free() drops them. */
static int replay_blocks(p2vi_block_t *blks, int nblks, int handover)
{
	char **paths = NULL, *body, *ln;
	int npaths = 0, *bmap = NULL, nmap = 0, i, k, st = 0, sep, bad = 0;
	/* sized for the union of every block's files: an eviction would
	 * silently drop an edited buffer from the derived patch */
	xbufsalloc = MAX(64, xbufsalloc);
	for (i = 0; i < nblks && st == 0; i++) {
		int last = handover && i == nblks - 1;
		if ((sep = blks[i].sep) < 0) {
			st = -1;
			break;
		}
		if (ed_init(last) < 0) {
			st = -1;
			break;
		}
		xvis |= 2;
		if (blks[i].npaths > nmap) {
			nmap = blks[i].npaths;
			bmap = erealloc(bmap, nmap * sizeof(int));
		}
		for (k = 0; k < blks[i].npaths; k++) {
			bmap[k] = sess_buf(&paths, &npaths, blks[i].paths[k]);
			xmpt = 0;
			ec_edit("", "e", blks[i].paths[k]);
		}
		xmpt = 0;
		xvis &= ~4;
		body = uc_dup(blks[i].body);
		if (strip_body_tail(body, sep) < 0
		    || !(ln = remap_bufnums(body, sep, bmap, blks[i].npaths))) {
			free(body);
			ed_done();
			st = -1;
			break;
		}
		free(body);
		body = ln;
		ex_regput(P2VI_REG, body, 0);
		ex_exec(body);
		free(body);
		/* a body that quit did so on failure (its own quit tail is
		 * gone), so the user is handed nothing and the status
		 * stands */
		if (last && !xquit) {
			/* hand over a plain editor: the body's own separator,
			 * escape and mode came from its "|sc!" prologue and
			 * the "vis 2" the stripped tail left behind */
			xvis = 0;
			xsep = ':';
			xesc = '\\';
			xerr = 1;
			if ((ln = getenv("P2VI_EX")))	/* test harness hook */
				ex_command(ln)
			if (!xquit)
				vi(1);
		}
		if (!xquit)	/* no counted quit: the block simply ended */
			xquit = -1;
		st = ed_done();
		bad = i + 1;
		if (i + 1 < nblks || !handover) {
			/* the next block starts as a fresh editor over the
			 * same buffers: saved (so :e and :q see no
			 * modification and undo cannot cross the boundary),
			 * and with no session state carried over */
			if (xbufcur)
				bufs_switch(0);
			for (k = 0; k < xbufcur; k++)
				lbuf_saved(bufs[k].lb, 1);
			ed_free_session();
		}
	}
	for (i = 0; i < npaths; i++)
		free(paths[i]);
	free(paths);
	free(bmap);
	if (st > 0)
		fprintf(stderr, "replay: block %d failed with status %d\n",
			bad, st);
	return st;
}

/* Replay generated scripts over the tree as it is on disk, all in one
 * session whose buffers the caller reads back: -po replays the origin and
 * then the target, so the user is handed the state both have been applied
 * to. Header assignments are per script (sh_reset), while each block carries
 * its own separator, so the two headers never mix. Phase 1 is made fatal
 * (QF1) and loud (DBG1) through the scripts' own conditionals, which read
 * the environment: a compat patch derived from a half-applied origin would
 * silently lack the changes that did not land. */
static int replay_scripts(const char **paths, int nscripts, int handover)
{
	p2vi_block_t *blks = NULL;
	int nblks = 0, st = 0, i;
	setenv("DBG1", "1", 1);
	setenv("QF1", "1", 1);
	for (i = 0; i < nscripts && st >= 0; i++) {
		FILE *f = fopen(paths[i], "r");
		if (!f) {
			perror(paths[i]);
			st = -1;
			break;
		}
		sh_reset();
		exec_script = paths[i];
		st = parse_p2vi_script(f, &blks, &nblks);
		fclose(f);
	}
	if (st >= 0)
		st = replay_blocks(blks, nblks, handover);
	free_blocks(blks, nblks);
	return st;
}

static int replay_script(const char *path, int handover)
{
	return replay_scripts(&path, 1, handover);
}

/*
 * -E: edit a file in the built-in nextvi and convert what changed into a
 * script. Nothing is written back: every buffer the editor leaves behind
 * is diffed against the file as it was on disk, and that diff feeds the
 * same pipeline a diff read from stdin would - so a session that visits
 * several files with :e yields one script covering all of them. Hence the
 * built-in differ below.
 */
static int edit_mode;		/* -E: edit, then emit the diff as a script */
static int compat_mode;		/* -pr: 1 (pre), -po: 2 (post) */
static const char *compat_origin;	/* the script it is derived against */

#define DIFF_CTX 3		/* context lines around a hunk */
#define DIFF_MAX_CELLS 4000000	/* largest LCS table worth building */

typedef struct {
	char t;		/* ' ' keep, '-' delete, '+' insert */
	char *s;	/* the line, owned by the old/new arrays */
} dop_t;

typedef struct {
	dop_t *v;
	int n, cap;
} dops_t;

static void dop_add(dops_t *d, char t, char *s)
{
	if (d->n >= d->cap) {
		d->cap = d->cap ? d->cap * 2 : 64;
		d->v = erealloc(d->v, d->cap * sizeof(dop_t));
	}
	d->v[d->n].t = t;
	d->v[d->n++].s = s;
}

/* Ops turning old[os..oe) into new[ns..ne), by way of the classic longest
 * common subsequence table. A table too large to be worth building degrades
 * to deleting the whole range and inserting the whole replacement. */
static void diff_region(dops_t *d, char **old, int os, int oe,
			char **new, int ns, int ne)
{
	int n = oe - os, m = ne - ns, i, j;
	int *c;
	if ((double)(n + 1) * (m + 1) > DIFF_MAX_CELLS) {
		for (i = os; i < oe; i++)
			dop_add(d, '-', old[i]);
		for (j = ns; j < ne; j++)
			dop_add(d, '+', new[j]);
		return;
	}
	c = emalloc((size_t)(n + 1) * (m + 1) * sizeof(int));
#define LCS(i, j) c[(i) * (m + 1) + (j)]
	for (i = n; i >= 0; i--) {
		for (j = m; j >= 0; j--) {
			if (i == n || j == m)
				LCS(i, j) = 0;
			else if (!strcmp(old[os + i], new[ns + j]))
				LCS(i, j) = LCS(i + 1, j + 1) + 1;
			else
				LCS(i, j) = MAX(LCS(i + 1, j), LCS(i, j + 1));
		}
	}
	i = j = 0;
	while (i < n && j < m) {
		if (!strcmp(old[os + i], new[ns + j])) {
			dop_add(d, ' ', old[os + i]);
			i++;
			j++;
		} else if (LCS(i + 1, j) >= LCS(i, j + 1)) {
			/* deletions first, so a change reads -... then +... */
			dop_add(d, '-', old[os + i]);
			i++;
		} else {
			dop_add(d, '+', new[ns + j]);
			j++;
		}
	}
	for (; i < n; i++)
		dop_add(d, '-', old[os + i]);
	for (; j < m; j++)
		dop_add(d, '+', new[ns + j]);
#undef LCS
	free(c);
}

/* One changed region of the origin script's landing: the span the origin
 * inserted, in post-origin line numbers, plus the lines it removed there. A
 * region with lo == hi removed only. */
static void free_lines(char **v, int n);

typedef struct {
	int lo, hi;	/* inserted span [lo,hi) in post coordinates */
	char **del;	/* removed lines */
	int ndel, dcap;
} chg_t;

static void free_regions(chg_t *r, int n)
{
	for (int i = 0; i < n; i++)
		free_lines(r[i].del, r[i].ndel);
	free(r);
}

/* Where the origin actually landed, after its own fuzz: the changed regions
 * of pre[] -> post[]. Strictly better than reading the origin's stored diff,
 * whose line numbers are the pre-fuzz ones. */
static int gate_regions(chg_t **out, char **pre, int npre,
			char **post, int npost)
{
	dops_t d;
	chg_t c;
	int i, ni = 0, n = 0, cap = 0;
	memset(&d, 0, sizeof(d));
	*out = NULL;
	diff_region(&d, pre, 0, npre, post, 0, npost);
	for (i = 0; i < d.n; ) {
		if (d.v[i].t == ' ') {
			ni++;
			i++;
			continue;
		}
		memset(&c, 0, sizeof(c));
		c.lo = c.hi = ni;
		for (; i < d.n && d.v[i].t != ' '; i++) {
			if (d.v[i].t == '+') {
				if (c.lo == c.hi)
					c.lo = ni;
				c.hi = ++ni;
			} else
				arr_append(&c.del, &c.ndel, &c.dcap, d.v[i].s);
		}
		if (n >= cap) {
			cap = cap ? cap * 2 : 8;
			*out = erealloc(*out, cap * sizeof(chg_t));
		}
		(*out)[n++] = c;
	}
	free(d.v);
	return n;
}

/* count_window() reads the global original; a probe is counted against three
 * texts in turn, so the global is swapped rather than the counter duplicated. */
static int count_in(char **src, int nsrc, char **win, int n)
{
	char **sv = orig_lines;
	int svn = n_orig_lines, first, cnt;
	orig_lines = src;
	n_orig_lines = nsrc;
	cnt = count_window(win, n, &first);
	orig_lines = sv;
	n_orig_lines = svn;
	return cnt;
}

/* Distance from a region to the compat hunk's anchor span, both in post
 * coordinates; 0 when they touch. Regions are tried nearest first, so the
 * gate probes the change that causes this collision and not some unrelated
 * hunk of the origin. */
static int span_dist(int lo, int hi, int alo, int ahi)
{
	if (hi < alo)
		return alo - hi;
	if (lo > ahi)
		return lo - ahi;
	return 0;
}

/* A probe window is usable when it names the post-origin text and nothing
 * else: unique there (or at least present, for an ANDed pair), absent from
 * the pre-origin text, and - for -po, where the gate must also separate
 * "origin + target" from "target alone" - absent from the target-only text. */
static int probe_ok(char **win, int n, char **pre, int npre,
		    char **post, int npost, char **x2o, int nx2o, int uniq)
{
	int cnt = count_in(post, npost, win, n);
	if (uniq ? cnt != 1 : cnt < 1)
		return 0;
	if (count_in(pre, npre, win, n))
		return 0;
	return !(x2o && count_in(x2o, nx2o, win, n));
}

/* The shortest window of post[r->lo .. r->hi) that qualifies, growing one
 * line at a time up to GATE_MAXLINES and never overlapping [xlo,xhi) - the
 * compat hunk's own edit span and anchors, which the block would otherwise
 * be free to destroy or duplicate along with its gate condition.
 * Returns the window length, 0 when the region yields none. */
static int probe_from_region(gate_t *g, chg_t *r, char **pre, int npre,
			     char **post, int npost, char **x2o, int nx2o,
			     int xlo, int xhi, int uniq)
{
	int len, s, i;
	for (len = 1; len <= GATE_MAXLINES && len <= r->hi - r->lo; len++) {
		for (s = r->lo; s + len <= r->hi; s++) {
			if (s < xhi && xlo < s + len)
				continue;
			if (!probe_ok(post + s, len, pre, npre, post, npost,
				      x2o, nx2o, uniq))
				continue;
			g->lines = emalloc(len * sizeof(char *));
			for (i = 0; i < len; i++)
				g->lines[i] = uc_dup(post[s + i]);
			g->nlines = len;
			g->polarity = GATE_PRESENT;
			g->mode = 0;
			return len;
		}
	}
	return 0;
}

/* Delete-only region: nothing the origin inserted is available to probe, so
 * probe a line it removed and invert the polarity - quit when the probe IS
 * found. The window must be gone from the post-origin text and present in
 * both the pre-origin text and (for -po) the target-only one, which is the
 * mirror of probe_ok(). */
static int probe_removed(gate_t *g, chg_t *r, char **pre, int npre,
			 char **post, int npost, char **x2o, int nx2o)
{
	int len, s, i;
	for (len = 1; len <= GATE_MAXLINES && len <= r->ndel; len++) {
		for (s = 0; s + len <= r->ndel; s++) {
			if (count_in(post, npost, r->del + s, len))
				continue;
			if (count_in(pre, npre, r->del + s, len) != 1)
				continue;
			if (x2o && count_in(x2o, nx2o, r->del + s, len) < 1)
				continue;
			g->lines = emalloc(len * sizeof(char *));
			for (i = 0; i < len; i++)
				g->lines[i] = uc_dup(r->del[s + i]);
			g->nlines = len;
			g->polarity = GATE_ABSENT;
			g->mode = 0;
			return len;
		}
	}
	return 0;
}

static void free_gates(gate_t *g, int n)
{
	for (int i = 0; i < n; i++) {
		free_lines(g[i].lines, g[i].nlines);
		g[i].lines = NULL;
		g[i].nlines = 0;
	}
}

/* Derive the gate for one compat block over one file. pre[]/post[] are the
 * file before and after the origin's blocks ran, x2o[] (optional, -po) the
 * same file with the target applied but not the origin. [alo,ahi) is the
 * compat hunk's anchor span and [xlo,xhi) the span the compat edit rewrites,
 * both in post coordinates.
 *
 * Regions are tried nearest the anchor first, and a single unique probe wins;
 * failing that, two individually ambiguous probes from distinct regions are
 * ANDed (sequential early exits, no nesting). Returns the number of gate
 * sections, 0 when no probe validates - a hard error for the caller, never a
 * reason to ship a weak gate. */
static int derive_gates(gate_t *g, int maxg, char **pre, int npre,
			char **post, int npost, char **x2o, int nx2o,
			int alo, int ahi, int xlo, int xhi)
{
	chg_t *r;
	int nr = gate_regions(&r, pre, npre, post, npost);
	int *ord = nr ? emalloc(nr * sizeof(int)) : NULL;
	int i, j, t, n = 0;
	if (maxg > GATE_MAXPROBES)
		maxg = GATE_MAXPROBES;
	for (i = 0; i < nr; i++)
		ord[i] = i;
	for (i = 1; i < nr; i++)		/* insertion sort, nearest first */
		for (j = i; j > 0 &&
		     span_dist(r[ord[j]].lo, r[ord[j]].hi, alo, ahi) <
		     span_dist(r[ord[j - 1]].lo, r[ord[j - 1]].hi, alo, ahi);
		     j--) {
			t = ord[j];
			ord[j] = ord[j - 1];
			ord[j - 1] = t;
		}
	for (i = 0; i < nr && !n; i++) {
		chg_t *c = &r[ord[i]];
		if (probe_from_region(g, c, pre, npre, post, npost,
				      x2o, nx2o, xlo, xhi, 1))
			n = 1;
		else if (c->lo == c->hi && probe_removed(g, c, pre, npre,
							 post, npost, x2o, nx2o))
			n = 1;
	}
	/* no region names the origin's landing on its own: AND two that each
	 * rule out the pre-origin text */
	if (!n && maxg > 1) {
		int got = 0;
		for (j = 0; j < nr && got < maxg; j++)
			if (probe_from_region(&g[got], &r[ord[j]], pre, npre,
					      post, npost, x2o, nx2o,
					      xlo, xhi, 0))
				got++;
		if (got == maxg)
			n = got;
		else
			free_gates(g, got);
	}
	free(ord);
	free_regions(r, nr);
	return n;
}

/* An op list as unified diff text for path: header, then one hunk per run
 * of changes, DIFF_CTX context lines around it. Nothing is written when
 * the list holds no change at all. The op list is the only input, so any
 * other way of deriving one (line identity, say) serializes through here. */
static void emit_dops(sbuf *out, const char *path, int is_new, dops_t *dp)
{
	dops_t d = *dp;
	int i, j, k, start, end, last, oc, nc, changed = 0;
	int *ono, *nno;
	for (i = 0; i < d.n; i++)
		if (d.v[i].t != ' ')
			changed = 1;
	if (!changed)
		return;
	/* the original and the new line number each op sits at */
	ono = emalloc((d.n + 1) * sizeof(int));
	nno = emalloc((d.n + 1) * sizeof(int));
	for (i = 0, j = 1, k = 1; i < d.n; i++) {
		ono[i] = j;
		nno[i] = k;
		j += d.v[i].t != '+';
		k += d.v[i].t != '-';
	}
	ono[d.n] = j;
	nno[d.n] = k;
	if (is_new)
		sbuf_str(out, "--- /dev/null\n")
	else {
		sbuf_str(out, "--- a/")
		sbuf_str(out, path)
		sbuf_chr(out, '\n')
	}
	sbuf_str(out, "+++ b/")
	sbuf_str(out, path)
	sbuf_chr(out, '\n')
	for (i = 0; i < d.n; ) {
		if (d.v[i].t == ' ') {
			i++;
			continue;
		}
		/* one hunk: from here to the last change still close enough
		 * that its context would touch this one's */
		start = MAX(i - DIFF_CTX, 0);
		last = i;
		for (j = i; j < d.n; ) {
			if (d.v[j].t != ' ') {
				last = j++;
				continue;
			}
			for (k = j; k < d.n && d.v[k].t == ' '; k++)
				;
			if (k >= d.n || k - j > 2 * DIFF_CTX)
				break;
			j = k;
		}
		end = last + 1 + DIFF_CTX;
		if (end > d.n)
			end = d.n;
		oc = nc = 0;
		for (k = start; k < end; k++) {
			oc += d.v[k].t != '+';
			nc += d.v[k].t != '-';
		}
		sb_printf(out, "@@ -%d,%d +%d,%d @@\n",
			  oc ? ono[start] : ono[start] - 1, oc,
			  nc ? nno[start] : nno[start] - 1, nc);
		for (k = start; k < end; k++) {
			sbuf_chr(out, d.v[k].t)
			sbuf_str(out, d.v[k].s)
			sbuf_chr(out, '\n')
		}
		i = end;
	}
	free(ono);
	free(nno);
}

/* The difference between old[] and new[] as a unified diff for path. */
static void emit_unified_diff(sbuf *out, const char *path, int is_new,
			      char **old, int nold, char **new, int nnew)
{
	dops_t d;
	int pre = 0, suf = 0, i;
	memset(&d, 0, sizeof(d));
	/* the LCS table only ever sees what head and tail trimming leaves */
	while (pre < nold && pre < nnew && !strcmp(old[pre], new[pre]))
		pre++;
	while (suf < nold - pre && suf < nnew - pre &&
	       !strcmp(old[nold - 1 - suf], new[nnew - 1 - suf]))
		suf++;
	for (i = 0; i < pre; i++)
		dop_add(&d, ' ', old[i]);
	diff_region(&d, old, pre, nold - suf, new, pre, nnew - suf);
	for (i = nold - suf; i < nold; i++)
		dop_add(&d, ' ', old[i]);
	emit_dops(out, path, is_new, &d);
	free(d.v);
}

static void free_lines(char **v, int n)
{
	for (int i = 0; i < n; i++)
		free(v[i]);
	free(v);
}

/* A file as a line array, newlines stripped. Missing file: no lines, and
 * *is_new is set, so it is diffed as a creation. */
static char **read_lines(const char *path, int *n, int *is_new)
{
	char **v = NULL, buf[MAX_LINE];
	int cap = 0, len;
	FILE *f = fopen(path, "r");
	*n = 0;
	*is_new = 0;
	if (!f) {
		*is_new = 1;
		return NULL;
	}
	while (fgets(buf, sizeof(buf), f)) {
		len = strlen(buf);
		if (len && buf[len - 1] == '\n')
			buf[len - 1] = '\0';
		arr_append(&v, n, &cap, buf);
	}
	fclose(f);
	return v;
}

/* Text as a line array, newlines stripped. The text is consumed in place. */
static char **split_lines(char *text, int *n)
{
	char **v = NULL, *p, *nl;
	int cap = 0;
	*n = 0;
	for (p = text; *p; p = nl + 1) {
		if ((nl = strchr(p, '\n')))
			*nl = '\0';
		arr_append(&v, n, &cap, p);
		if (!nl)
			break;
	}
	return v;
}

/* Stage B1 diagnostic, until the extraction and emission of stage B2 give it
 * a real consumer: for every buffer the origin's replay changed, derive and
 * report the gate that would guard a compat block over that file. The anchor
 * span is the whole file and nothing is excluded, so this exercises probe
 * selection and validation but not yet locality. */
static int compat_report_gates(char **x2o, int nx2o)
{
	gate_t g[GATE_MAXPROBES];
	char **pre, **post, *text;
	int npre, npost, is_new, n, i, j, k, bad = 0, next_id = 1;
	for (i = 0; i < xbufcur; i++) {
		if (!bufs[i].lb->modified)
			continue;
		pre = read_lines(bufs[i].path, &npre, &is_new);
		text = lbuf_text(bufs[i].lb);
		post = split_lines(text, &npost);
		n = derive_gates(g, GATE_MAXPROBES, pre, npre, post, npost,
				 x2o, nx2o, 0, npost, -1, -1);
		/* a fresh id per probe: the gate's ?? tag must not fuse with
		 * any group's, so it is allocated before the groups' (B2) */
		for (j = 0; j < n; j++)
			g[j].tag = next_mark_id(&next_id);
		if (!n) {
			fprintf(stderr, "gate: %s: no probe validates, "
				"supply a gate by hand\n", bufs[i].path);
			bad = 1;
		}
		for (j = 0; j < n; j++) {
			sbuf_smake(body, MAX_LINE)
			fprintf(stderr, "=== GATE %d %s mode %d tag %d ===\n",
				j + 1, g[j].polarity == GATE_ABSENT ? "absent"
				: "present", g[j].mode, g[j].tag);
			for (k = 0; k < g[j].nlines; k++)
				fprintf(stderr, "%s\n", g[j].lines[k]);
			fprintf(stderr, "%s\n", end_tag_wr);
			/* the ex-body fragment the gate would emit, so the
			 * q!0 early exit is exercised before B2 consumes it */
			emit_gate(body, &g[j], 1);
			sbuf_null(body)
			fprintf(stderr, "gate-body: %s\n", body->s);
			free(body->s);
		}
		free_gates(g, n);
		free_lines(pre, npre);
		free(post);
		free(text);
	}
	return bad ? -1 : 0;
}

/* One buffer left behind by the session, against the file it names. */
static void buf_to_diff(sbuf *out, const char *path, struct lbuf *lb)
{
	char **old, **new, *text;
	int nold, nnew, is_new;
	old = read_lines(path, &nold, &is_new);
	text = lbuf_text(lb);
	new = split_lines(text, &nnew);
	emit_unified_diff(out, path, is_new, old, nold, new, nnew);
	free(text);
	free_lines(old, nold);
	free_lines(new, nnew);
}

/* Run an editing session over args and write the resulting unified diff to
 * out. Every buffer the session leaves behind is diffed, not just the ones
 * named here: files reached with :e during the session join the same diff,
 * in the order they were opened, and the script covers all of them. A path
 * that does not exist yet is diffed as a file creation.
 *
 * The session is nextvi's own main(), renamed nextvi_main() by
 * build_patch2vi.sh, so args is a nextvi command line: its flags (-aemsv,
 * -- ) and its file list behave exactly as they do in vi(1), EXINIT is
 * honoured, and repeated paths share one buffer. Only the framing is
 * patch2vi's - the terminal is claimed first because stdout is the script,
 * and the buffers are read back and freed after, which is the whole point:
 * nextvi_main() returns without touching them and nothing is ever written
 * to disk. Its process-wide bring-up doubles as ed_init()'s, so a later
 * session (-i) must not repeat it. */
static int edit_to_diff(char **args, int nargs, sbuf *out)
{
	char **argv;
	int i, st;
	/* every buffer of the session ends up in the diff, so the session
	 * gets room for more of them than a plain editor would keep */
	xbufsalloc = MAX(64, xbufsalloc);
	if (ed_grabtty() < 0)
		return -1;
	argv = emalloc((nargs + 1) * sizeof(argv[0]));
	argv[0] = "vi";
	for (i = 0; i < nargs; i++)
		argv[i + 1] = args[i];
	st = nextvi_main(nargs + 1, argv);
	free(argv);
	ed_once = 1;
	ed_ungrabtty();
	if (st != 0) {
		fprintf(stderr, "editor exited with error %d\n", st);
		ed_free();
		return -1;
	}
	for (i = 0; i < xbufcur; i++)
		if (bufs[i].path && bufs[i].path[0])
			buf_to_diff(out, bufs[i].path, bufs[i].lb);
	ed_free();
	return 0;
}

/* One line of unified diff, wherever it came from: a file, stdin, or the
 * built-in differ under -E. The line is consumed in place (chomped, and
 * paths are cut out of it). */
static int diff_in_hunk;	/* inside an @@ hunk */
static int diff_old_line;	/* the original line the next op sits at */

static void parse_diff_line(char *line)
{
	/* New file: +++ b/path[\ttimestamp] */
	if (strncmp(line, "+++ ", 4) == 0) {
		char *path = line + 4;
		/* Skip common prefixes like b/ */
		if (path[0] && path[1] == '/')
			path += 2;
		/* Strip trailing tab/space + timestamp (unified diff suffix) */
		char *t = strpbrk(path, "\t ");
		if (t)
			*t = '\0';
		new_file(path);
		diff_in_hunk = 0;
		return;
	}

	/* --- line: /dev/null means the next +++ creates a new file.
	 * Otherwise stash the original path: on disk it holds the
	 * pre-patch content the script will run against, used for
	 * file-aware anchor validation. */
	if (strncmp(line, "--- ", 4) == 0) {
		char *p = line + 4;
		pending_is_new = strncmp(p, "/dev/null", 9) == 0
				 && (!p[9] || p[9] == '\t' || p[9] == ' ');
		free(pending_orig_path);
		pending_orig_path = NULL;
		if (!pending_is_new) {
			if (p[0] && p[1] == '/')
				p += 2;  /* strip a/ prefix */
			char *t = strpbrk(p, "\t ");
			if (t)
				*t = '\0';
			pending_orig_path = uc_dup(p);
		}
		return;
	}

	/* Skip diff line */
	if (strncmp(line, "diff ", 5) == 0)
		return;

	/* Skip index line */
	if (strncmp(line, "index ", 6) == 0)
		return;

	/* Hunk header */
	int os, oc;
	if (parse_hunk_header(line, &os, &oc)) {
		diff_in_hunk = 1;
		diff_old_line = os;
		cur_hunk_lo = os;
		cur_hunk_hi = oc > 0 ? os + oc - 1 : os;
		/* GNU diff -N marks created files with the nonexistent
		 * path and an epoch timestamp instead of /dev/null, so
		 * also detect them by their sole "@@ -0,0" hunk: the
		 * original had no lines. A later hunk addressing real
		 * lines means the file existed after all. */
		if (nfiles) {
			if (os == 0 && oc == 0 && files[nfiles - 1].nops == 0)
				files[nfiles - 1].is_new = 1;
			else if (os > 0)
				files[nfiles - 1].is_new = 0;
		}
		return;
	}

	if (!diff_in_hunk || nfiles == 0)
		return;

	/* Process hunk content */
	if (line[0] == ' ') {
		/* Context line - store content for relative mode */
		add_op('c', diff_old_line, line + 1);
		diff_old_line++;
	} else if (line[0] == '-') {
		/* Delete line - store content for horizontal edit detection */
		add_op('d', diff_old_line, line + 1);
		diff_old_line++;
	} else if (line[0] == '+') {
		/* Add line */
		add_op('a', diff_old_line, line + 1);
	} else if (line[0] == '\\') {
		/* "\ No newline at end of file" - skip */
		return;
	} else {
		/* Unknown line in hunk */
		diff_in_hunk = 0;
	}
}

/* Feed a whole in-memory unified diff through the line parser. */
static void parse_diff_text(const char *text)
{
	char line[MAX_LINE];
	const char *p, *nl;
	for (p = text; *p; p = nl + 1) {
		nl = strchr(p, '\n');
		int len = nl ? (int)(nl - p) : (int)strlen(p);
		if (len > (int)sizeof(line) - 2)
			len = sizeof(line) - 2;
		memcpy(line, p, len);
		line[len] = '\n';
		line[len + 1] = '\0';
		add_raw(line);
		line[len] = '\0';
		parse_diff_line(line);
		if (!nl)
			break;
	}
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-arih] [-d[N]] [-er TAG] [-ew TAG] [input.patch]\n"
		"       %s -e script.sh\n"
		"       %s [-ari]E [nextvi-opts...]\n", prog, prog, prog);
	fprintf(stderr,
		"Converts unified diff to shell script using nextvi ex commands\n");
	fprintf(stderr, "  -a    Use absolute line numbers\n");
	fprintf(stderr,
		"  -r    Use relative regex patterns instead of line numbers\n");
	fprintf(stderr,
		"  -i    Interactive mode: edit search patterns in the built-in nextvi\n");
	fprintf(stderr,
		"        Each group's PHASE 1/2 sections hold its verbatim ex-body\n"
		"        bytes; editing them supersedes the structured sections for\n"
		"        that group (latest edit wins, tie goes to verbatim)\n");
	fprintf(stderr,
		"  -d    Delta mode: re-apply previous customizations (-d implies -i)\n");
	fprintf(stderr,
		"  -d1   Delta mode: match by group index only\n");
	fprintf(stderr,
		"  -d2   Delta mode: match by group index + deleted/inserted text or regex if custom\n");
	fprintf(stderr,
		"  -d3   Delta mode: match by group index + entire hunk\n");
	fprintf(stderr,
		"  -d4   Delta mode: match by deleted/inserted text or regex if custom\n");
	fprintf(stderr,
		"  -d5   Delta mode: match by entire hunk\n");
	fprintf(stderr,
		"  -e    Execute a generated script with the built-in nextvi,\n"
		"        no shell involved (one editor per script block)\n");
	fprintf(stderr,
		"  -E    Edit the named files in the built-in nextvi and convert\n"
		"        the edits into a script on stdout; no file is ever\n"
		"        written, and files opened with :e during the session\n"
		"        join the same script. Everything after -E is a plain\n"
		"        nextvi command line, EXINIT included\n");
	fprintf(stderr,
		"  -er   Read section end tag (default: \"%s\")\n", end_tag_rd);
	fprintf(stderr,
		"  -ew   Write section end tag (default: \"%s\")\n", end_tag_wr);
	fprintf(stderr, "  -h    Show this help\n");
	fprintf(stderr,
		"Input can be a unified diff or a previously generated patch2vi script\n");
	exit(1);
}

int main(int argc, char **argv)
{
	char line[MAX_LINE];
	int i, j;

	/* Parse arguments */
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (argv[i][1] == '-' && !argv[i][2]) {
			i++;
			break;
		}
		if (argv[i][1] == 'e' && argv[i][2] == 'r') {
			if (argv[i][3])
				end_tag_rd = argv[i] + 3;
			else if (i + 1 < argc)
				end_tag_rd = argv[++i];
			else {
				fprintf(stderr, "Option -er requires an argument\n");
				usage(argv[0]);
			}
			continue;
		}
		if (argv[i][1] == 'e' && argv[i][2] == 'w') {
			if (argv[i][3])
				end_tag_wr = argv[i] + 3;
			else if (i + 1 < argc)
				end_tag_wr = argv[++i];
			else {
				fprintf(stderr, "Option -ew requires an argument\n");
				usage(argv[0]);
			}
			continue;
		}
		/* -pr/-po origin.sh: derive a compatibility patch against
		 * that script, applied before (pre) or after (post) the
		 * target, which is the ordinary positional input */
		if (argv[i][1] == 'p' && (argv[i][2] == 'r' || argv[i][2] == 'o')) {
			compat_mode = argv[i][2] == 'r' ? 1 : 2;
			if (argv[i][3])
				compat_origin = argv[i] + 3;
			else if (i + 1 < argc)
				compat_origin = argv[++i];
			else {
				fprintf(stderr, "Option -p%c requires an argument\n",
					argv[i][2]);
				usage(argv[0]);
			}
			continue;
		}
		/* bare -e: execute the script; tested after -er/-ew so it
		 * cannot shadow them, and kept out of the cluster loop
		 * whose letters are a r i h d E */
		if (argv[i][1] == 'e' && !argv[i][2]) {
			exec_mode = 1;
			continue;
		}
		for (j = 1; argv[i][j]; j++) {
			if (argv[i][j] == 'a')
				relative_mode = 0;
			else if (argv[i][j] == 'r')
				relative_mode = 1;
			else if (argv[i][j] == 'i')
				interactive_mode = 1;
			/* -E takes no argument of its own and ends patch2vi's
			 * own option parsing: whatever follows its cluster is
			 * a nextvi command line, options and files alike, and
			 * the script goes to stdout as in every other mode */
			else if (argv[i][j] == 'E')
				edit_mode = 1;
			else if (argv[i][j] == 'd') {
				if (argv[i][j+1] >= '1' && argv[i][j+1] <= '5') {
					j++;
					delta_mode = argv[i][j] - '0';
				} else {
					delta_mode = -1;
				}
				interactive_mode = 1;
			} else if (argv[i][j] == 'h')
				usage(argv[0]);
			else {
				fprintf(stderr, "Unknown option: -%c\n", argv[i][j]);
				usage(argv[0]);
			}
		}
		if (edit_mode) {	/* the rest belongs to nextvi */
			i++;
			break;
		}
	}
	if (i < argc && !edit_mode)
		input_file = argv[i];

	/* Mark chars that cannot be ex separators. */
	static const char *forbidden =
		" \t0123456789+-.,<>/$';%*#|" /* ex range syntax */
		"@&!?bpaefidgmqrwusxycjtohlv=" /* ex commands */
		":\"\\`\n\r";                  /* default sep, shell quote/escape/backtick, newline */
	for (const char *p = forbidden; *p; p++)
		byte_used[(unsigned char)*p] = 1;

	if (relative_mode || interactive_mode)
		mark_bytes_used("FAIL OK");

	/* -E: the diff is not read, it is made. Everything patch2vi's own
	 * option loop did not consume is a nextvi command line - flags after
	 * "--", then files (a missing one counts as a creation) - and the
	 * buffers that session leaves behind are diffed against their disk
	 * copies, that diff going through the parser in place of an input
	 * stream. The script itself goes to stdout, like every other mode. */
	sbuf_smake(dsb, MAX_LINE)
	if (edit_mode) {
		if (i >= argc) {
			fprintf(stderr, "-E requires a file argument\n");
			return 1;
		}
		if (edit_to_diff(argv + i, argc - i, dsb) < 0)
			return 1;
		sbuf_null(dsb)
		parse_diff_text(dsb->s);
	}

	FILE *in = edit_mode ? NULL : stdin;
	if (input_file && !edit_mode) {
		in = fopen(input_file, "r");
		if (!in) {
			perror(input_file);
			return 1;
		}
	}

	/* -e: no conversion, just run the script through the embedded
	 * editor and report the status the shell would have reported */
	if (exec_mode) {
		if (!input_file) {
			fprintf(stderr, "-e requires a script argument\n");
			return 1;
		}
		exec_script = input_file;
		i = exec_p2vi_script(in);
		fclose(in);
		return i;
	}

	/* Detect if input is a previously generated patch2vi script */
	if (in && fgets(line, sizeof(line), in)) {
		if (strncmp(line, "#!/bin/sh", 9) == 0) {
			/* Skip until "exit 0" line */
			while (fgets(line, sizeof(line), in)) {
				chomp(line);
				if (strcmp(line, "exit 0") == 0)
					break;
			}
			/* Read structured delta section */
			file_delta_t *cur_fd = NULL;
			grp_delta_t *cur_gd = NULL;
			int in_sect = GS_NONE;
			int pat_idx = 1; /* pattern[] slot for GS_PAT */
			int in_ph = 0;   /* 1/2 = inside a verbatim phase blob */
			sbuf_smake(ph, MAX_LINE)
			while (fgets(line, sizeof(line), in)) {
				chomp(line);
				/* Verbatim blobs are raw: only the end tag
				 * terminates. Their bytes are marked used so a
				 * changed patch can't pick a SEP/ESC that
				 * collides with the stored segments. */
				if (in_ph) {
					if (strcmp(line, end_tag_rd) == 0) {
						if (ph->s_n > 0 && ph->s[ph->s_n - 1] == '\n')
							ph->s_n--;
						sbuf_null(ph)
						if (cur_gd) {
							char **dst = in_ph == 1
								     ? &cur_gd->ph1 : &cur_gd->ph2;
							free(*dst);
							*dst = uc_dup(ph->s);
							mark_bytes_used(*dst);
						}
						sbufn_cut(ph, 0)
						in_ph = 0;
					} else {
						sbuf_str(ph, line)
						sbuf_chr(ph, '\n')
					}
					continue;
				}
				if (strncmp(line, "=== PATCH2VI PATCH ===", 22) == 0)
					break;
				if (strncmp(line, "=== PATCH2VI DELTA ===", 22) == 0)
					continue;
				if (strcmp(line, end_tag_rd) == 0) {
					if (!in_sect) {
						cur_fd = NULL;
						cur_gd = NULL;
					}
					in_sect = 0;
					continue;
				}
				if (strncmp(line, "=== DELTA ", 10) == 0) {
					in_sect = 0;
					cur_gd = NULL;
					if (interactive_mode && nin_deltas <
					    (int)(sizeof(in_deltas) / sizeof(in_deltas[0]))) {
						char *p = line + 10;
						char *end = strstr(p, " ===");
						if (end)
							*end = '\0';
						cur_fd = &in_deltas[nin_deltas++];
						cur_fd->filepath = uc_dup(p);
						cur_fd->grps = NULL;
						cur_fd->ngrps = 0;
						cur_fd->gcap = 0;
					} else {
						cur_fd = NULL;
					}
					continue;
				}
				if (!interactive_mode || !cur_fd)
					continue;
				if (line[0] == '=' && strncmp(line, "=== ", 4) == 0) {
					if (strncmp(line, "=== GROUP ", 10) == 0) {
						const char *p = line + 10;
						int idx = atoi(p);
						if (cur_fd->ngrps >= cur_fd->gcap) {
							cur_fd->gcap = cur_fd->gcap
								       ? cur_fd->gcap * 2 : 4;
							cur_fd->grps = erealloc(
									       cur_fd->grps,
									       cur_fd->gcap *
									       sizeof(grp_delta_t));
						}
						cur_gd = &cur_fd->grps[cur_fd->ngrps++];
						memset(cur_gd, 0, sizeof(*cur_gd));
						cur_gd->group_idx = idx;
						in_sect = GS_CONTENT;
						continue;
					}
					if (strncmp(line, "=== LEVEL ", 10) == 0) {
						if (cur_gd)
							parse_level(cur_gd, line);
						continue;
					}
					if (strcmp(line, "=== custom_text ===") == 0) {
						in_sect = GS_CUSTOM;
						continue;
					}
					if (strcmp(line, "=== pre_ctx ===") == 0) {
						in_sect = GS_PRE;
						continue;
					}
					if (strcmp(line, "=== post_ctx ===") == 0) {
						in_sect = GS_POST;
						continue;
					}
					if (strcmp(line, "=== strategy ===") == 0) {
						in_sect = GS_STRAT;
						continue;
					}
					if (strncmp(line, "=== pattern", 11) == 0) {
						/* "pattern<1-NSEARCH>"; legacy bare
						 * "pattern" maps to the top-context
						 * slot (pattern 3) */
						char c = line[11];
						pat_idx = (c >= '1' && c <= '0' + NSEARCH)
							  ? c - '1' : 2;
						in_sect = GS_PAT;
						continue;
					}
					if (strncmp(line, "=== offset", 10) == 0) {
						/* "=== offset<1-NSEARCH> <%+d> ===" */
						char c = line[10];
						int oi = (c >= '1' && c <= '0' + NSEARCH)
							 ? c - '1' : 2;
						if (cur_gd) {
							cur_gd->pat_off[oi] = atoi(line + 11);
							cur_gd->pat_has_off[oi] = 1;
						}
						continue;
					}
					if (strncmp(line, "=== mode", 8) == 0) {
						/* "=== mode<1-NSEARCH> <%d> ===" */
						char c = line[8];
						int mi = (c >= '1' && c <= '0' + NSEARCH)
							 ? c - '1' : 2;
						if (cur_gd) {
							cur_gd->pat_mode[mi] = atoi(line + 9);
							cur_gd->pat_has_mode[mi] = 1;
						}
						continue;
					}
					if (strcmp(line, "=== edit_cmd_abs ===") == 0) {
						in_sect = GS_ABS;
						continue;
					}
					if (strcmp(line, "=== edit_cmd_relc ===") == 0) {
						in_sect = GS_RELC;
						continue;
					}
					if (strcmp(line, "=== edit_cmd_rel ===") == 0) {
						in_sect = GS_REL;
						continue;
					}
					if (strncmp(line, "=== verbatim mark ", 18) == 0) {
						if (cur_gd) {
							cur_gd->ovr_mark = atoi(line + 18);
							char *e = strstr(line + 18, " esc ");
							if (e)
								cur_gd->ovr_esc = atoi(e + 5);
						}
						continue;
					}
					if (strcmp(line, "=== phase1 ===") == 0) {
						in_ph = 1;
						continue;
					}
					if (strcmp(line, "=== phase2 ===") == 0) {
						in_ph = 2;
						continue;
					}
					continue;
				}
				if (!cur_gd)
					continue;
				if (in_sect == GS_CONTENT) {
					if (line[0] == '-')
						arr_append(&cur_gd->del_lines,
							   &cur_gd->ndel_lines,
							   &cur_gd->del_cap, line + 1);
					else if (line[0] == '+')
						arr_append(&cur_gd->add_lines,
							   &cur_gd->nadd_lines,
							   &cur_gd->add_cap, line + 1);
					continue;
				}
				gsect_add(cur_gd, in_sect, pat_idx, line);
			}
			free(ph->s);
		} else {
			/* Not a script; store and process this first line */
			add_raw(line);
			chomp(line);
			parse_diff_line(line);
		}
	}

	while (in && fgets(line, sizeof(line), in)) {
		add_raw(line);
		chomp(line);
		parse_diff_line(line);
	}

	if (in && in != stdin)
		fclose(in);

	/* -pr/-po: replay the origin script in one session and hand the
	 * tree it leaves behind to the user, who reshapes it so the target
	 * applies. Runs before the separator is picked, so the bytes of
	 * whatever the session produces are seen by find_unused_byte(). */
	if (compat_mode) {
		if (replay_script(compat_origin, 1) != 0) {
			ed_free();
			return 1;
		}
		i = compat_report_gates(NULL, 0);
		ed_free();
		if (i < 0)
			return 1;
		fprintf(stderr, "-p%c: compat derivation is not implemented yet\n",
			compat_mode == 1 ? 'r' : 'o');
		return 1;
	}

	/* Find separator character */
	int sep = find_unused_byte();
	if (sep < 0) {
		fprintf(stderr,
			"error: patch uses all possible byte values, cannot find separator\n");
		return 1;
	}
	/* Next unused byte becomes the ex escape character; if none is
	 * left, fall back to the default backslash escape paths. */
	byte_used[sep] = 1;
	dyn_esc = find_unused_byte();
	if (dyn_esc < 0)
		dyn_esc = 0;
	else
		byte_used[dyn_esc] = 1;

	/* Emit shell script header; the emit layer targets sbufs, so build
	 * stdout pieces in one scratch sbuf and flush it after each use */
	sbuf_smake(osb, MAX_LINE)
	fputs("#!/bin/sh -e\n# Generated by patch2vi from unified diff\n", stdout);
	list_unused_bytes(osb);
	sbuf_null(osb)
	fputs(osb->s, stdout);
	fputs("\n# Pass any argument to use patch(1) instead of nextvi ex commands\n"
	      "if [ -n \"$1\" ]; then\n"
	      "    sed '1,/^=== PATCH2VI PATCH ===$/d' \"$0\" | patch -p1 --merge=diff3\n"
	      "    exit $?\n"
	      "fi\n\n"
	      "VI=${VI:-vi}\n"
	      "if ! $VI -? 2>&1 | grep -q 'Nextvi'; then\n"
	      "    echo \"Error: $VI is not nextvi\" >&2\n"
	      "    echo \"Set VI environment variable to point to nextvi binary\" >&2\n"
	      "    exit 1\n"
	      "fi\n\n", stdout);
	printf("SEP=\"$(printf '\\%03o')\"\n", sep);
	if (dyn_esc)
		printf("ESC=\"$(printf '\\%03o')\"\n", dyn_esc);
	if (relative_mode || interactive_mode) {
		/* <esc><sep> and <esc><esc><esc><sep> as they appear inside
		 * the script's double-quoted strings, matching what
		 * EMIT_ESCSEP / EMIT_ESC3SEP emit into the command body */
		const char *e1 = dyn_esc ? "${ESC}${SEP}" : "\\\\${SEP}";
		const char *e3 = dyn_esc ? "${ESC}${ESC}${ESC}${SEP}"
				 : "\\\\\\\\\\\\${SEP}";
		fputs("# Command that handles readability line breaks\n"
		      "LB=\"0?\"\n"
		      "# Phase 1 (search/mark): errors disabled by default,\n"
		      "# DBG1=1 enables error reporting, QF1=1 quits on failure\n"
		      "# OK1: with DBG1=1 also report fallback anchor successes\n"
		      "[ \"$DBG1\" = \"1\" ] && OK1= || OK1=\"0?\"\n"
		      "[ \"$DBG1\" = \"1\" ] && DBG1= || DBG1=\"0?\"\n", stdout);
		printf("[ \"$QF1\" = \"1\" ] && QF1=\"%svis 2%sq!1\" || QF1=\n",
		       e1, e1);
		fputs("# Phase 2 (edits): DBG2=1 disables errors, QF2=1 ignores them\n"
		      "# OK2: with DBG2= also report fallback substitute successes\n"
		      "[ \"$DBG2\" = \"1\" ] && OK2=\"0?\" || OK2=\n"
		      "[ \"$DBG2\" = \"1\" ] && DBG2=\"0?\" || DBG2=\n", stdout);
		printf("[ \"$QF2\" = \"1\" ] && QF2= || QF2=\"%svis 2%sq!1\"\n",
		       e1, e1);
		fputs("# Enters vi at failing code line in this script\n"
		      "# Designed for state inspection mid execution\n", stdout);
		printf("[ \"$INTR\" = \"1\" ] && INTR=\"%s|sc|%svis 2:fr 0:e $0:83reg "
		       "%%@47:%%f> %%@112:&Q:b0:|sc! %s|:vis 3%sq1\" || INTR=\n",
		       e1, e1, e3, e1);
	}

	/* Build groups for every file */
	for (int i = 0; i < nfiles; i++)
		build_file_groups(&files[i]);

	/* Collect files with groups */
	file_patch_t *active[256];
	int nactive = 0;
	for (int i = 0; i < nfiles; i++)
		if (files[i].ngroups > 0)
			active[nactive++] = &files[i];

	/* Interactive editing: one built-in editor session for all files */
	if (interactive_mode)
		interactive_edit_all_files(active, nactive);

	if (nactive > 0) {
		fputs("\n# Patch:", stdout);
		for (int k = 0; k < nactive; k++)
			fprintf(stdout, " %s", active[k]->path);
		fputc('\n', stdout);
		/* A large patch overflows the EXINIT env var / argv limits, so the
		 * ex command body is written to a temp file (letting the shell do its
		 * expansion pass) and passed to vi as the last filename. The fixed
		 * EXINIT yanks that buffer into register 97 and executes it. */
		fputs("# Body too large for EXINIT/argv: stage it in a file\n"
		      "( : > /tmp/p2vi.$$ ) 2>/dev/null && P2VIF=/tmp/p2vi.$$ || P2VIF=./p2vi.$$\n"
		      "trap 'rm -f \"$P2VIF\"' EXIT\n", stdout);
		printf("printf '%%s\\n' \"|sc! %s|:vis 3${SEP}",
		       dyn_esc ? "${ESC}${SEP}" : "\\\\\\\\${SEP}");
		/* register <b> caches the buffer; fr gates f> searches to it */
		if (relative_mode || interactive_mode)
			fputs("fr 98${SEP}", stdout);
		for (int k = 0; k < nactive; k++) {
			fprintf(stdout, "b%d${SEP}", k);
			/* nothing to cache or search in a brand new file */
			if ((relative_mode || interactive_mode) && !active[k]->is_new)
				fputs("%ya 98${SEP}", stdout);
			cur_file_path = active[k]->path;
			sbuf_cut(osb, 0)
			emit_file_script(osb, active[k]);
			sbuf_null(osb)
			fputs(osb->s, stdout);
		}
		fputs("vis 2${SEP}", stdout);
		/* Write all buffers at the end */
		for (int k = 0; k < nactive; k++)
			fprintf(stdout, "b%d${SEP}w${SEP}", k);
		fputs("2q\" > \"$P2VIF\"\n"
		      P2VI_VICALL " $VI -e", stdout);
		for (int k = 0; k < nactive; k++)
			fprintf(stdout, " '%s'", active[k]->path);
		/* body file is always the last buffer; vi makes it current at startup */
		fputs(" \"$P2VIF\"\n", stdout);
	}

	/* Embed delta and original patch after exit 0 */
	printf("\nexit 0\n");
	printf("=== PATCH2VI DELTA ===\n");
	for (int i = 0; i < nout_deltas; i++) {
		file_delta_t *od = &out_deltas[i];
		if (od->ngrps == 0)
			continue;
		printf("=== DELTA %s ===\n", od->filepath);
		sbuf_cut(osb, 0)
		for (int j = 0; j < od->ngrps; j++)
			emit_grp_delta(osb, &od->grps[j]);
		sbuf_null(osb)
		fputs(osb->s, stdout);
		printf("%s\n", end_tag_wr);
	}
	printf("=== PATCH2VI PATCH ===\n");
	for (int i = 0; i < nraw; i++)
		fputs(raw_lines[i], stdout);

	free(osb->s);
	free(dsb->s);
	return 0;
}
