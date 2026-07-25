/*
 * patch2vi - turn a unified diff into a /bin/sh script driving nextvi's ex
 * engine, and back.
 *
 * Usage: patch2vi [-arih] [-d[N]] [-er TAG] [-ew TAG] [patch|script]
 *        patch2vi -e script.sh
 *        patch2vi [-ari]E [nextvi-opts...]
 *        patch2vi -co origin.sh target.sh [compat.diff|compat.sh]
 *
 * The script applies the patch in raw ex mode (:vis 3). The command
 * separator and the escape byte are picked per patch via :sc! so that
 * : % ! \ in the content need no escaping, and are exported as $SEP/$ESC.
 * Edits are anchored by line number (-a) or by search pattern (-r); the
 * per-group delta and the original diff are stored after the script's
 * "exit 0", so a generated script regenerates (-d) and is edited (-i)
 * without the diff at hand.
 *
 * Nextvi is embedded whole: vi.c (and through it every editor module) is
 * compiled into this translation unit, build_patch2vi.sh renaming nextvi's
 * main() to nextvi_main() for the build. That editor also
 *   - runs the interactive modes on in-RAM buffers (edit_units): no temp
 *     files, no argv, no EXINIT;
 *   - executes a generated script with no shell (-e), one editor lifetime
 *     per script block;
 *   - turns a plain editing session into a script (-E): everything past -E
 *     is a nextvi command line - its flags, its files, EXINIT - and every
 *     buffer it leaves behind is diffed against its disk copy to produce
 *     the input the converter normally reads;
 *   - replays two scripts (-co) to derive a compatibility patch that
 *     applies before/after the target, gated on the origin's own change; an
 *     optional third argument (diff or script) pre-applies a known compat
 *     patch that the session then continues from.
 * No mode writes to disk; quitting is what emits.
 */
#include "vi.c"

/* nextvi's own main(), renamed for this build by build_patch2vi.sh; -E
 * runs a whole editing session through it, flags, EXINIT and all */
int nextvi_main(int argc, char *argv[]);

/* Initial capacity for sbufs holding one line-ish string; they grow. */
#define SB_INIT 512

/* Grow (arr, cap) so slot n exists and is zeroed; the caller fills it and
 * bumps n. Every table here grows this way - no fixed limits. */
#define ARR_PUSH(arr, n, cap) \
{ \
	if ((n) >= (cap)) { \
		(cap) = (cap) ? (cap) * 2 : 8; \
		(arr) = erealloc((arr), (cap) * sizeof(*(arr))); \
	} \
	memset(&(arr)[n], 0, sizeof(*(arr))); \
} \

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
	op_t *ops;
	int nops, ops_cap;
	struct group_s *groups;  /* heap-allocated, set by build_file_groups */
	int ngroups;
	int is_new;              /* patch creates this file (--- /dev/null) */
	char *orig_path;         /* "---" path, holds pre-patch content (file-aware) */
} file_patch_t;

static file_patch_t *files;
static int nfiles, files_cap;
static const char *cur_file_path;  /* set per-file for error messages */
static int relative_mode;  /* 0=absolute, 1=relative search (-r) */
static int interactive_mode; /* 1=interactive editing of search patterns (-i) */
/* 1 = re-read and re-apply stored deltas/compat regions from a generated
 * script, distinct from opening the group-editing session. -i/-d set both;
 * -co set only this so regen keeps host customizations without a UI. */
static int read_deltas;
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

/*
 * SEARCH MODES. Every phase-1 search carries one; it follows from the pattern's
 * shape (1 for a single line, 0 otherwise, the window generators picking their
 * own) and a SEARCH PATTERN's MODE marker overrides it. emit_search_setup
 * writes the form each implies:
 *   0  "%f>" over the find register cache - the buffer yanked once after open,
 *      where the whole file is one string, so a multi-line window's newlines
 *      are visible to the regex; f+ resumes one char past the previous match.
 *   1  ".,$f>" over the live buffer from the current line's first column, with
 *      ^...$ per-line anchors to disambiguate repeated text ("fr" drops the
 *      cache for the search, "fr 98" puts it back).
 *   2  mode 0 bracketed with "grp 1 .. grp 0", so the find lands on the
 *      captured group (pattern 7's absorbing window).
 *   3  mode 2 run globally: the cursor is saved to WIN_SAVE_MARK, reset to the
 *      top and restored after (patterns 8 and 9's straddle windows).
 */

/* Compatibility-block gate (-co). A compat block only applies to a tree
 * that carries the origin script's change; the gate is the question "is the
 * origin change that causes this collision present here?", asked as an exact
 * multi-line literal search before any edit and answered by quitting (q!0)
 * when the answer is no. Probes come from the origin's own landing (see
 * derive_gates): its inserted lines for GATE_PRESENT, its removed lines for
 * GATE_ABSENT. Every sensor runs before any body writes, so a probe describes
 * the tree with the origin applied and the target not yet - and when this
 * block's file carries no trace of the origin, the probe is taken from another
 * file the origin did change (gate_t's path). */
#define GATE_MAXLINES 8   /* longest probe window: locality beats length */
#define GATE_MAXPROBES 2  /* probe sections per block, ANDed in order */

/* ?? tags reserved by a compat block's gate while its groups regenerate, so
 * gen_group_segments() skips them (the id is shared numeric space with the ??
 * capture tags); empty when no gate is being emitted. */
static int compat_res_marks[GATE_MAXPROBES];
static int ncompat_res;
/* Set while the replay session is the compat one, so replay_blocks() snapshots
 * the post-origin baseline right before handing over to the user. */
static int compat_capturing;
/* Set while a compat block's groups are generated/emitted: they use the exact
 * strategies (slots 1-NPAT) only - a compat patch is a local touch-up, so the
 * file-validated fuzz/grp/straddle generators (which read the pre-origin file
 * that is the wrong text here) are disabled. */
static int compat_building;
static int compat_mode;			/* -co: derive a post-only compat patch */
static const char *compat_origin;	/* the script it is derived against */
/* -co third argument: an already written compat fix (a unified diff or a
 * generated script) applied to the post-origin+target tree before the user
 * is handed the editor, so a known resolution is not retyped. It lands after
 * the baseline snapshot, hence it is part of the derived compat patch. */
static const char *compat_pre;

enum {
	GATE_ALWAYS = 0,  /* no probe: the block is unconditional */
	GATE_PRESENT,     /* quit when the probe is missing (??!) */
	GATE_ABSENT,      /* quit when the probe is found (??) */
};

typedef struct {
	char **lines;     /* probe window, owned (raw text, escaped at emit) */
	int nlines;
	int polarity;     /* GATE_ALWAYS / GATE_PRESENT / GATE_ABSENT */
	int tag;          /* allocated ?? capture id */
	/* the file the probe is searched in, owned; NULL = the block's own first
	 * file. "Is the origin on this tree" is a property of the tree, not of one
	 * file, so a block over a file the origin never touched (or whose landing
	 * the target overwrote) probes a file the origin did change. */
	char *path;
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
	int pat_mode[NSEARCH];     /* per-pattern MODE, see SEARCH MODES */
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
	int ovr_sep;        /* separator byte at capture time */
} grp_delta_t;

typedef struct {
	char *filepath;
	grp_delta_t *grps;
	int ngrps;
	int gcap;
} file_delta_t;

/* A growable per-file delta store: the host's input (read from a script)
 * and output (captured from the editor) sets, plus one per compat block. */
typedef struct { file_delta_t *v; int n, cap; } dstore_t;

static dstore_t out_deltas, in_deltas;

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
static char **raw_lines;
static int nraw, raw_cap;

/* Dynamic string vector: a compat block owns its own === PATCH === lines
 * rather than splicing them into the host's raw_lines[]. */
typedef struct { char **v; int n, cap; } strv_t;
static void arr_append(char ***arr, int *n, int *cap, const char *s);

/* When set, add_raw() appends into this sink instead of the host's raw_lines[]
 * - so a compat diff parsed through parse_diff_text() lands in the compat
 * block's own storage, leaving the host === PATCH === section byte-identical. */
static strv_t *raw_sink;

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

/* Ex command separator byte, likewise picked from the bytes the patch does
 * not use. Both go into the script as raw bytes: the command body is a
 * single-quoted printf argument, so sh passes every byte but ' through. */
static int sep;

static void *ecalloc(size_t n, size_t sz)
{
	void *p = calloc(n, sz);
	if (!p) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return p;
}

/* The store's entry for path, appended if absent. */
static file_delta_t *dstore_get(dstore_t *ds, const char *path)
{
	for (int i = 0; i < ds->n; i++)
		if (!strcmp(ds->v[i].filepath, path))
			return &ds->v[i];
	ARR_PUSH(ds->v, ds->n, ds->cap)
	ds->v[ds->n].filepath = uc_dup(path);
	return &ds->v[ds->n++];
}

/* One store entry per fps[k] (NULL where the file has no delta), the
 * per-file lookup every inject/derive pass starts from. */
static file_delta_t **dstore_per_file(dstore_t *ds, file_patch_t **fps, int n)
{
	file_delta_t **v = ecalloc(n > 0 ? n : 1, sizeof(file_delta_t *));
	for (int k = 0; ds && k < n; k++)
		for (int i = 0; i < ds->n; i++)
			if (!strcmp(ds->v[i].filepath, fps[k]->path)) {
				v[k] = &ds->v[i];
				break;
			}
	return v;
}

static void add_raw(const char *line)
{
	if (raw_sink)
		arr_append(&raw_sink->v, &raw_sink->n, &raw_sink->cap, line);
	else
		arr_append(&raw_lines, &nraw, &raw_cap, line);
}

/* Remove trailing newline, keeping the sbuf length in step so the caller
 * can append to (and cut back) the chomped line. */
static char *chomp_sb(sbuf *sb)
{
	while (sb->s_n && (sb->s[sb->s_n - 1] == '\n' ||
			   sb->s[sb->s_n - 1] == '\r'))
		sb->s_n--;
	sbufn_ret(sb, sb->s)
}

/* Remove trailing newline */
static void chomp(char *s)
{
	int n = strlen(s);
	while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r'))
		s[--n] = '\0';
}

/* Read one line of any length into sb, newline included (as fgets leaves
 * it); NULL at EOF. sb is reused across calls, so callers may modify it in
 * place and leave the loop at any point without freeing. */
static char *read_line(FILE *f, sbuf *sb)
{
	int c;
	sbuf_cut(sb, 0)
	while ((c = getc(f)) != EOF) {
		sbuf_chr(sb, c)
		if (c == '\n')
			break;
	}
	if (!sb->s_n)
		return NULL;
	sbufn_ret(sb, sb->s)
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

/* Free n owned strings and the array holding them. */
static void free_lines(char **v, int n)
{
	for (int i = 0; i < n; i++)
		free(v[i]);
	free(v);
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

/* A file as a line array, newlines stripped. Missing file: no lines, and
 * *is_new is set, so it is diffed as a creation. */
static char **read_lines(const char *path, int *n, int *is_new)
{
	char **v = NULL;
	int cap = 0;
	FILE *f = fopen(path, "r");
	*n = 0;
	*is_new = !f;
	if (!f)
		return NULL;
	sbuf_smake(lb, SB_INIT)
	while (read_line(f, lb)) {
		if (lb->s[lb->s_n - 1] == '\n')
			sbufn_cut(lb, lb->s_n - 1)
		arr_append(&v, n, &cap, lb->s);
	}
	free(lb->s);
	fclose(f);
	return v;
}

/* A whole file as one string, NULL if it cannot be opened. */
static char *file_text(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	sbuf_smake(sb, SB_INIT)
	sbuf_smake(lb, SB_INIT)
	while (read_line(f, lb))
		sbuf_str(sb, lb->s)
	free(lb->s);
	fclose(f);
	sbufn_ret(sb, sb->s)
}

static void load_orig_file(const char *path)
{
	int is_new;
	orig_lines = read_lines(path, &n_orig_lines, &is_new);
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

/* Count consecutive-line matches of an n-line window in orig_lines, where
 * line j of a candidate matches when eq(orig line, win, j) holds. Sets
 * *first to the 0-based start of the first match (-1 if none). */
static int count_window_by(const void *win, int n, int *first,
			   int (*eq)(const char *, const void *, int))
{
	int cnt = 0, i, j;
	*first = -1;
	if (!orig_lines || n <= 0 || n > n_orig_lines)
		return 0;
	for (i = 0; i + n <= n_orig_lines; i++) {
		for (j = 0; j < n && eq(orig_lines[i + j], win, j); j++)
			;
		if (j < n)
			continue;
		if (*first < 0)
			*first = i;
		cnt++;
	}
	return cnt;
}

static int m_exact(const char *ln, const void *win, int j)
{
	return !strcmp(ln, ((char **)win)[j]);
}

/* Count exact consecutive-line matches of window[0..n) in orig_lines. */
static int count_window(char **window, int n, int *first)
{
	return count_window_by(window, n, first, m_exact);
}

/* Count consecutive-line matches of a substring window in orig_lines: each
 * win[j] must occur as a substring of the aligned original line (an empty
 * win[j] matches any line, mirroring ".*.*"). This is the match semantics of
 * the pattern-7 ".*TEXT.*" window. Sets *first to the 0-based start of the
 * first match (-1 if none). */
static int m_substr(const char *ln, const void *win, int j)
{
	const char *w = ((char **)win)[j];
	return !w[0] || strstr(ln, w) != NULL;
}

static int count_window_substr(char **win, int n, int *first)
{
	return count_window_by(win, n, first, m_substr);
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
static int m_fuzzy(const char *ln, const void *win, int j)
{
	return match_fuzzy_line(ln, &((fline_t *)win)[j]);
}

static int count_window_fuzzy(fline_t *win, int n, int *first)
{
	return count_window_by(win, n, first, m_fuzzy);
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

/* Number of (overlapping) occurrences of needle in haystack, both counted. */
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

/* Count occurrences of s[start..end) as a substring of s. */
static int range_occurs(const char *s, int start, int end)
{
	return str_count_occ(s, strlen(s), s + start, end - start);
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

/* The common byte prefix and suffix of two lines, each snapped back to a rune
 * boundary so runes sharing lead bytes (e.g. é vs è) are never split; the
 * suffix never overlaps the prefix. */
static void common_affix(const char *old, const char *new, int *pre, int *suf)
{
	int old_len = strlen(old), new_len = strlen(new), prefix = 0, suffix = 0;
	while (old[prefix] && new[prefix] && old[prefix] == new[prefix])
		prefix++;
	while (prefix > 0 && (old[prefix] & 0xC0) == 0x80)
		prefix--;
	while (suffix < old_len - prefix && suffix < new_len - prefix &&
	       old[old_len - 1 - suffix] == new[new_len - 1 - suffix])
		suffix++;
	while (suffix > 0 && (old[old_len - suffix] & 0xC0) == 0x80)
		suffix--;
	*pre = prefix;
	*suf = suffix;
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
	int prefix, suffix;
	common_affix(old, new, &prefix, &suffix);

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

/* Mark a stored verbatim blob's bytes, minus the two the script it came
 * from reserved for itself. Those are structure, not content: counting
 * them would move the next generation onto different bytes and strand
 * every blob captured under the old pair. */
static void mark_verbatim_bytes(const char *s, int esc, int sp)
{
	for (; *s; s++)
		if (*s != esc && *s != sp)
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

/* One raw byte into a shell double-quoted word. Only the four bytes sh
 * still reads there need escaping; the body is single quoted and needs
 * none of this (see sq_write). */
static void sb_dq(sbuf *out, int c)
{
	if (c == '\\' || c == '$' || c == '`' || c == '"')
		sb_chr(out, '\\');
	sb_chr(out, c);
}

/* Write a command body to the script as a single-quoted printf argument.
 * Single quotes are the one thing such a word cannot hold, so each is
 * closed, escaped and reopened; every other byte, control bytes and the
 * separator included, goes out verbatim. */
static void sq_write(const char *s, int n)
{
	for (int i = 0; i < n; i++) {
		if (s[i] == '\'')
			fputs("'\\''", stdout);
		else
			putchar((unsigned char)s[i]);
	}
}

static void emit_escaped_text(sbuf *out, const char *s);

/* the body register, and the EXINIT that yanks the body buffer into it
 * and runs it; -e fills the register itself and needs neither */
#define P2VI_REG 97
#define P2VI_VICALL "EXINIT='%ya 97:? %@97'"

/*
 * Script state lives in ex registers, not shell variables.
 *
 * A shell variable is a flat string spliced into a site whose nesting depth
 * the shell cannot know, and whose escaping depends on which bytes are
 * special there. A register sidesteps both: its content is escaped once,
 * where it is defined, and "?%@<id>" runs it verbatim at any depth
 * (ex_arg sbuf_mem's the expansion and never rescans it).
 *
 * Definedness is the switch. An undefined register expands to nothing, and
 * "?" with an empty argument runs nothing, so a gate register is simply
 * left undefined when its switch is off; the shell only ever contributes
 * whole commands ("${DBG1:+213reg ...}") that define or clear one.
 *
 * Expansion is turned on ("2sc %") for the call and off again ("2sc")
 * immediately: with xexp live every % in an argument would expand, and
 * arguments carry regexes derived from file content. Argument registers
 * are therefore always written before the window, never inside it. xexe
 * (!) stays 0 throughout - the |sc! prologue zeroes it - since a stray !
 * in a live argument forks a shell.
 */
#define REG_QF1  210	/* phase-1 quit chain, set by QF1=1 */
#define REG_QF2  211	/* phase-2 quit chain, cleared by QF2=1 */
#define REG_INTR 212	/* interrupt chain, set by INTR=1 */
#define REG_ERR1 213	/* phase-1 FAIL gate, set by DBG1=1 */
#define REG_ERR2 214	/* phase-2 FAIL gate, cleared by DBG2=1 */
#define REG_OK1  215	/* phase-1 OK gate, set by DBG1=1 */
#define REG_OK2  216	/* phase-2 OK gate, cleared by DBG2=1 */
#define REG_HDLR 217	/* the FAIL report chain both phases share */
#define REG_LOC  219	/* argument: the FAIL location of the current site */
#define REG_MSG  220	/* argument: the OK report command of the current site */
/*
 * -co sensor flags. In the single-vi-call model (one process, one body) a
 * sensor gate that fires before the host body writes a flag register that a
 * later block reads after it; registers are global and never cleared between
 * chains, so the bit crosses the host body. Two writes per firing sensor:
 *   REG_FLAG_ANY      one shared register, appended to by every firing sensor,
 *                     so "any origin fired" is "this register is non-empty" -
 *                     answered by a single f> at the host's per-site override.
 *   REG_FLAG_BASE+k   one per origin k, read by the per-block back-to-front
 *                     subset test that decides which block asserts.
 * Both sit ABOVE the 210-220 control band so they never overwrite a quit/error
 * register; an unset flag reads false (ex_regget NULL -> "uninitialized
 * register" -> xuerr), so no initialisation pass is needed. The per-block
 * subset anchors use ec_while slot ids >= 10, above every single-digit group
 * chain tag (NSEARCH <= 9), so they never fuse with a group's result.
 */
#define REG_FLAG_ANY  230	/* shared any-origin-fired register */
#define REG_FLAG_BASE 231	/* per-origin flag registers: REG_FLAG_BASE+k */
#define FLAG_SLOT_BASE 10	/* ec_while subset-test anchor slots (>= 10) */
/*
 * Gate probe tags live in their own band, far above both the group chain tags
 * (pattern slot + 1, 1..NSEARCH <= 9) and the per-block subset slots
 * (FLAG_SLOT_BASE + k). xanchor is global to the process and a lookup ORs every
 * recorded entry carrying the id (ec_while, ex.c), so a tag shared with a
 * group's fallback capture - or with another block's gate - would let one
 * sensor answer for another: on a mixed single-origin tree the absent origin's
 * block would fire anyway. Numbering is continuous across blocks (next_gate_tag
 * takes the next id above every tag already stored), so stacked blocks never
 * share one, and a tag read back from storage keeps the sequence going.
 */
#define GATE_TAG_BASE 1000
/* One escape run of n bytes followed by the separator. n = 0 is the plain
 * command separator, 1 escapes it for a ??! block's argument, 3 for a ??
 * then-arg nested one level further in. */
static void emit_esc_sep(sbuf *out, int n)
{
	while (n-- > 0)
		sb_chr(out, dyn_esc ? dyn_esc : '\\');
	sb_chr(out, sep);
}

/* the same, for the one part of a body the shell writes: a double-quoted
 * word, where a raw escape or separator byte may need escaping again */
static void sb_dq_esc_sep(sbuf *out, int n)
{
	while (n-- > 0)
		sb_dq(out, dyn_esc ? dyn_esc : '\\');
	sb_dq(out, sep);
}

#define EMIT_SEP(out) emit_esc_sep(out, 0)
/* escaped separator inside ??! block: <esc><sep> for ex_arg */
#define EMIT_ESCSEP(out) emit_esc_sep(out, 1)
/* triply-escaped separator inside a ?? then-arg nested in a ? cond:
 * <esc><esc><esc><sep> */
#define EMIT_ESC3SEP(out) emit_esc_sep(out, 3)
/* the no-op command that lets a long chain break across source lines */
#define EMIT_LB(out) sb_str(out, "0?\n")

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
 *   w / q! (write, quit) - ec_write / ec_quit
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

/* Emit content lines for a c/i ex command. */
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

/* The ";A[;B]c/d" tail of a horizontal (character-level) edit, emitted after
 * the caller's address prefix (a line number, or "'N" for a mark). An empty
 * replacement over a non-empty span deletes it. */
static void emit_horiz_span(sbuf *out, int start, int end, const char *new_text)
{
	if (!*new_text && start != end) {
		sb_printf(out, ";%d;%dd", start, end);
	} else {
		if (start == end)
			sb_printf(out, ";%dc ", start);
		else
			sb_printf(out, ";%d;%dc ", start, end);
		emit_escaped_text(out, new_text);
	}
	EMIT_SEP(out);
}

/* The same at an absolute line (no-op when there is nothing to change). */
static void emit_horizontal_change(sbuf *out, int line, int start, int end,
				   const char *new_text)
{
	if (!*new_text && start == end)
		return;
	sb_printf(out, "%d", line);
	emit_horiz_span(out, start, end, new_text);
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
 * Relative mode: anchor edits by regex search instead of line number.
 *
 * Phase 1 (resolve). The whole buffer is yanked once into the find
 * register (fr 98) right after the file opens, so every group's search
 * runs against a cache that stays byte-identical to the buffer - no edit
 * happens in this phase. Each group records its target line in a line
 * mark ("+<off>m <id>", ids counting up from 0, skipping nextvi's special
 * ids) and gets up to NSEARCH fallback patterns tried strict-to-loose,
 * first match wins (emit_fallback_chain; slots past NPAT are the
 * file-validated fuzz/grp/straddle windows, slots 1-5 are
 * default_pat_lines'). Searches use %f> (first of a file) / %f+
 * (subsequent) against the cache: a range maps the match back to a buffer
 * position and f+ resumes one char past the previous match start. A lone
 * surviving single-line pattern searches the buffer directly instead
 * (";0 fr .,$f> ^pattern$ .. fr 98"), where ;0 resets xoff and the ^...$
 * anchors disambiguate repeated text. ABS-strategy groups mark their
 * original line number as-is - the buffer is still pristine here, so no
 * cumulative line-delta correction is needed.
 *
 * Phase 2 (commit). Edits address the marks ('0c, '0d, '0,#+Nc,
 * '0s/../../, '0;A;Bc ...), which auto-adjust as edits above them shift
 * lines, so groups apply forward in patch order. Since every search ran
 * before the first edit, a failed anchor aborts with the file untouched.
 *
 * Every search is followed by a ??! check that reports and quits before
 * corrupting the file; each phase-2 edit gets the same check with a FAIL
 * m<mark id> message. All search paths escape uniformly through ex_arg.
 */

/* One separator at the caller's nesting depth: deep = 0 for a command
 * inside a ??/??! argument, 1 for one nested a further level in. */
static void emit_sep_lvl(sbuf *out, int deep)
{
	if (deep)
		EMIT_ESC3SEP(out);
	else
		EMIT_ESCSEP(out);
}

/* Emit the expansion window that calls gate register <gate>: turn % on,
 * run the register's chain, turn % off. The argument register the chain
 * reads must already be written, while % was still inert. */
static void emit_reg_call(sbuf *out, int gate, int deep)
{
	sb_str(out, "2sc %");
	emit_sep_lvl(out, deep);
	sb_printf(out, "? %%@%d", gate);
	emit_sep_lvl(out, deep);
	sb_str(out, "2sc");
}

/* Emit ??! error check after a command that may fail.
 * loc: location text in the FAIL message ("path:line" for phase-1
 * searches, "path:line:m<id>" for phase-2 edits at a mark), handed to the
 * shared report chain in REG_LOC.
 * phase selects the gate register, whose definedness is the DBG<n> switch
 * and whose chain ends in the phase's INTR and QF<n> calls.
 * tags (optional, may be NULL) prefixes the conditional with a DNF
 * capture-id expression so it branches on recorded statuses instead
 * of the last command's. */
static void emit_err_check_loc(sbuf *out, const char *loc, int phase,
			       const char *tags)
{
	if (tags && *tags)
		sb_str(out, tags);
	/* "?" "?!" split: "??!" in one literal is the trigraph for '|' */
	sb_printf(out, "?" "?!%dreg %s", REG_LOC, loc);
	EMIT_ESCSEP(out);
	emit_reg_call(out, phase == 1 ? REG_ERR1 : REG_ERR2, 0);
	EMIT_SEP(out);
}

/* Build the FAIL location and the optional tag list for the check above.
 * Phase 1 (search) reports <path>:<line>, phase 2 (edit at a mark) adds
 * :m<id>; mark_id < 0 means no mark (new-file insert, custom abs command).
 * ids[0..nids) are the capture tags of a fallback chain - every pattern
 * variant in phase 1, every substitute rung in phase 2 - ORed into one DNF
 * expression so the inverted branch fires only if all of them failed. */
static void emit_err_check(sbuf *out, int phase, int line, int mark_id,
			   const int *ids, int nids)
{
	sbuf_smake(loc, SB_INIT)
	sbuf_smake(tags, SB_INIT)
	sb_printf(loc, "%s:%d", cur_file_path ? cur_file_path :
		  phase == 1 ? "?" : "", line);
	if (phase == 2) {
		sb_str(loc, ":m");
		if (mark_id >= 0)
			sb_printf(loc, "%d", mark_id);
	}
	for (int t = 0; t < nids; t++)
		sb_printf(tags, t ? ";%d" : "%d", ids[t]);
	sbuf_null(loc)
	sbuf_null(tags)
	emit_err_check_loc(out, loc->s, phase, tags->s);
	free(loc->s);
	free(tags->s);
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
	sb_str(out, exarg_esc);
	free(exarg_esc);
}

/* Everything a search of the given mode needs before its f> argument, and the
 * search verb itself. Mode 3 saves the cursor to WIN_SAVE_MARK and resets to
 * the top ("1;0") so its window search runs globally; modes 2 and 3 open the
 * grp bracket; mode 1 leaves the register cache ("fr") to search the live
 * buffer from the current line's first column (";0"). lvl is the caller's
 * separator nesting depth: 0 at a body's top level, 1 inside a ??-arm. first
 * selects f> over f+, and a global (mode 3) window always forces f>, since it
 * restarts from the reset top. */
static void emit_search_setup(sbuf *out, int mode, int first, int lvl)
{
	int g3 = mode == 3;
	if (g3) {
		sb_printf(out, "m %d", WIN_SAVE_MARK);
		emit_esc_sep(out, lvl);
		sb_str(out, "1;0");
		emit_esc_sep(out, lvl);
	}
	if (mode == 2 || g3) {
		sb_str(out, "grp 1");
		emit_esc_sep(out, lvl);
	}
	if (mode == 1) {
		sb_str(out, ";0");
		emit_esc_sep(out, lvl);
		sb_str(out, "fr");
		emit_esc_sep(out, lvl);
		sb_str(out, first ? ".,$f> " : ".,$f+ ");
	} else
		sb_str(out, (g3 || first) ? "%f> " : "%f+ ");
}

/* The lone-pattern phase-1 search: the mode's setup and f> (see SEARCH MODES),
 * the anchors, the error check, then "+<offset>m <mark_id>" to mark the target
 * line without moving the cursor. pre_escaped 0 = anchors are raw text (apply
 * regex+exarg escape), 1 = pre-escaped regex (apply exarg only). */
static void emit_search(sbuf *out, char **anchors, int nanchors,
			int offset, int mark_id,
			int target_line, int pre_escaped, int first, int mode)
{
	int single = mode == 1;
	int g3 = mode == 3;
	int grp = mode == 2 || g3;
	emit_search_setup(out, mode, first, 0);
	/* pre-escaped (interactive) patterns carry their own ^...$ from the
	 * displayed default; the user may have removed them */
	if (single && !pre_escaped)
		sb_chr(out, '^');
	for (int i = 0; i < nanchors; i++) {
		if (pre_escaped) {
			char *e = escape_exarg(anchors[i]);
			sb_str(out, e);
			free(e);
		} else {
			char *r = escape_regex(anchors[i]);
			char *e = escape_exarg(r);
			sb_str(out, e);
			free(e);
			free(r);
		}
		if (i < nanchors - 1)
			sb_chr(out, '\n');
	}
	if (single && !pre_escaped)
		sb_str(out, "$");	/* $ anchor */
	/* Ensure trailing newline when last anchor is empty */
	if (nanchors > 0 && !anchors[nanchors - 1][0])
		sb_chr(out, '\n');
	EMIT_SEP(out);
	emit_err_check(out, 1, target_line, -1, NULL, 0);
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
	EMIT_LB(out);
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
	/* For interactive mode (-i): */
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
	int custom_pat_mode[NSEARCH];    /* per-section MODE override */
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
	int ovr_sep;             /* separator the override was captured under */
} group_t;

/* Does the group carry any text a search could anchor on - leading context, a
 * following context line, or a non-empty deleted line? Decides REL vs ABS and
 * which EDIT COMMAND sections are offered. */
static int group_has_anchors(group_t *g)
{
	return g->nanchors >= 2
	       || (g->nanchors == 1 && g->anchors[0] && g->anchors[0][0])
	       || (g->follow_ctx && g->follow_ctx[0])
	       || (g->ndel > 0 && g->del_texts[0] && g->del_texts[0][0]);
}

/* Emit a line with exarg + shell escaping only (no regex escaping).
 * Used for user-edited regex lines in interactive mode. */
static void emit_escaped_exarg_only(sbuf *out, const char *s)
{
	char *e = escape_exarg(s);
	sb_str(out, e);
	free(e);
}

/* One fallback search pattern (phase 1) */
typedef struct {
	char **lines;
	int nlines;
	int pre_escaped;  /* 1 = user regex (exarg only), 0 = raw text */
	int offset;       /* lines from match start to the target line */
	int off_final;    /* 1 = offset from OFFSET marker, no adjustment */
	int mode;         /* search mode, see SEARCH MODES */
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
	int mode;       /* search mode, see SEARCH MODES */
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
		free_lines(w[i].lines, w[i].nlines);
		w[i].lines = NULL;
		w[i].nlines = 0;
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
	return 1;
}

/* Every file-validated relaxed window of one group, slot i holding pattern slot
 * NPAT + i: the fuzz slots first, then GRP_SLOT, WIN_SLOT and WIN2_SLOT. */
typedef struct {
	fuzzwin_t w[NSEARCH - NPAT];
	int has[NSEARCH - NPAT];
} winset_t;

static void gen_extra_windows(group_t *g, winset_t *ws)
{
	memset(ws, 0, sizeof(*ws));
	for (int i = gen_fuzz_windows(g, ws->w, NFUZZ); i-- > 0; )
		ws->has[i] = 1;
	ws->has[GRP_SLOT - NPAT] = gen_grp_window(g, &ws->w[GRP_SLOT - NPAT]);
	ws->has[WIN_SLOT - NPAT] = gen_win_window(g, &ws->w[WIN_SLOT - NPAT], 0);
	ws->has[WIN2_SLOT - NPAT] = gen_win_window(g, &ws->w[WIN2_SLOT - NPAT], 1);
}

static void free_extra_windows(winset_t *ws)
{
	for (int i = 0; i < NSEARCH - NPAT; i++)
		if (ws->has[i])
			free_fuzz_windows(&ws->w[i], 1);
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
		sb_str(out, x);
		free(x);
		free(r);
		if (i < p->nlines - 1)
			sb_chr(out, '\n');
	}
	if (wrap)
		sb_str(out, "$");	/* $ anchor */
	/* Ensure trailing newline when last line is empty */
	if (p->nlines > 0 && !p->lines[p->nlines - 1][0])
		sb_chr(out, '\n');
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
 * Each attempt opens with its own mode setup (see SEARCH MODES); the teardown
 * that setup implies is emitted per attempt too - a mode-1 attempt restores the
 * register cache ("fr 98") on both the success and no-match paths, a mode-2/3
 * one resets the search group ("grp 0"), and a mode-3 one restores the saved
 * cursor unconditionally, before the success-gated 1q. */
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
		 * previous block otherwise), a line-break no-op clause and a real
		 * newline, then the separator before the search setup. Every
		 * attempt thus starts on its own source line. */
		EMIT_ESCSEP(out);
		EMIT_LB(out);
		EMIT_ESCSEP(out);
		/* the mode's own setup, one escape level deeper than a
		 * top-level search; a mode-1 attempt's "fr 98" below puts the
		 * register cache back for the attempts after it */
		emit_search_setup(out, ps[n].mode, first, 1);
		emit_chain_pattern(out, &ps[n]);
		EMIT_ESCSEP(out);
		sb_printf(out, "%d??", ps[n].pid);
		/* Readability line break once the search result is captured
		 * into tag <n>: a line-break (no-op) clause and a real newline split
		 * the long single-line chain so each attempt's match and its
		 * mark action sit on separate source lines. Placed after the
		 * tag capture (and before grp 0 / the action re-test) so it
		 * never separates a tag test from its then-arm. */
		EMIT_ESCSEP(out);
		EMIT_LB(out);
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
		/* fallback (non-primary) match: with DBG1=1 the OK gate is
		 * defined and reports which anchor resolved the group */
		if (n) {
			EMIT_ESC3SEP(out);
			sb_printf(out, "%dreg p OK %s:%d:a%d", REG_MSG,
				  cur_file_path ? cur_file_path : "?",
				  target_line, ps[n].pid);
			EMIT_ESC3SEP(out);
			emit_reg_call(out, REG_OK1, 1);
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
	EMIT_LB(out);
	EMIT_SEP(out);
	for (int n = 0; n < nps; n++)
		pids[n] = ps[n].pid;
	emit_err_check(out, 1, target_line, -1, pids, nps);
	EMIT_LB(out);
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
	emit_err_check(out, 2, line, mark_id, NULL, 0);
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
	emit_err_check(out, 2, line, mark_id, NULL, 0);
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
	emit_err_check(out, 2, line, mark_id, NULL, 0);
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

/* Escape regex pattern for substitute command.
 * Like escape_regex() but also escapes the delimiter for ex_re_read. */
static char *escape_sub_pat_raw(const char *s, char delim)
{
	char set[sizeof(REGEX_META) + 1];
	snprintf(set, sizeof(set), "%s%c", REGEX_META, delim);
	return escape_chars(s, set);
}

/* A NUL-terminated copy of n bytes from s (byte-exact, where
 * uc_sub() would count characters). */
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
 * UTF-8 rune boundaries. */
typedef struct {
	int oa, na, len;
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
	sb_str(out, ea);
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
		emit_err_check(out, 2, line, mark_id, NULL, 0);
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
			 * the OK report non-immediate (mirrors phase 1's
			 * report after "m id") */
			sb_printf(out, "'%d", mark_id);
			EMIT_ESC3SEP(out);
			sb_printf(out, "%dreg p OK %s:%d:s%d", REG_MSG,
				  cur_file_path ? cur_file_path : "?", line, v[n].sid);
			EMIT_ESC3SEP(out);
			emit_reg_call(out, REG_OK2, 1);
			if (n < nv - 1) {
				EMIT_ESC3SEP(out);
				sb_str(out, "1q");
			}
		} else {
			sb_str(out, "1q");
		}
	}
	EMIT_SEP(out);
	EMIT_LB(out);
	EMIT_SEP(out);
	for (int n = 0; n < nv; n++)
		sids[n] = v[n].sid;
	emit_err_check(out, 2, line, mark_id, sids, nv);
	EMIT_LB(out);
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
	/* rung 0: the minimal-span exact substitute, fully escaped */
	v[nv].pat = double_trailing_esc(escape_sub_pat_raw(g->ld_old_text, '/'));
	v[nv].repl = double_trailing_esc(escape_sub_repl_raw(g->ld_new_text, '/'));
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

/* Free every array a grp_delta_t owns (scalars need no cleanup). */
static void free_grp(grp_delta_t *p)
{
	free_lines(p->del_lines, p->ndel_lines);
	free_lines(p->add_lines, p->nadd_lines);
	free_lines(p->custom_text, p->ncustom_text);
	free_lines(p->pre_ctx, p->npre_ctx);
	free_lines(p->post_ctx, p->npost_ctx);
	for (int k = 0; k < NSEARCH; k++)
		free_lines(p->pattern[k], p->npattern[k]);
	free_lines(p->abs_cmd, p->nabs);
	free_lines(p->rel_cmd, p->nrel);
	free_lines(p->relc_cmd, p->nrelc);
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

/* The slot digit right after a "=== <tag>" prefix of n bytes, as a 0-based
 * pattern index; a legacy tag with no digit means the top-context slot
 * (historically the single pattern, now SEARCH PATTERN 3). */
static int pat_slot(const char *line, int n)
{
	char c = line[n];
	return (c >= '1' && c <= '0' + NSEARCH) ? c - '1' : 2;
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
	sbuf_smake(ph, SB_INIT)

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

/* One "=== <name> ===" body of n lines, closed by the write end tag; nothing at
 * all when the array is empty, so an untouched field leaves no section. */
static void emit_delta_sect(sbuf *out, const char *name, char **v, int n)
{
	if (n <= 0)
		return;
	sb_printf(out, "=== %s ===\n", name);
	for (int i = 0; i < n; i++)
		sb_printf(out, "%s\n", v[i]);
	sb_printf(out, "%s\n", end_tag_wr);
}

/* Emit one group's delta in the human-readable structured format */
static void emit_grp_delta(sbuf *out, grp_delta_t *gd)
{
	char name[32];
	sb_printf(out, "=== GROUP %d ===\n", gd->group_idx);
	for (int i = 0; i < gd->ndel_lines; i++)
		sb_printf(out, "-%s\n", gd->del_lines[i]);
	for (int i = 0; i < gd->nadd_lines; i++)
		sb_printf(out, "+%s\n", gd->add_lines[i]);
	sb_printf(out, "%s\n", end_tag_wr);
	sb_printf(out, "=== LEVEL %d%s ===\n", gd->level ? gd->level : 2,
		  gd->has_star ? "*" : "");
	emit_delta_sect(out, "custom_text", gd->custom_text, gd->ncustom_text);
	emit_delta_sect(out, "pre_ctx", gd->pre_ctx, gd->npre_ctx);
	emit_delta_sect(out, "post_ctx", gd->post_ctx, gd->npost_ctx);
	if (gd->strategy != STRAT_DEFAULT) {
		char *sn = gd->strategy == STRAT_REL ? "rel"
			   : gd->strategy == STRAT_RELC ? "relc" : "abs";
		emit_delta_sect(out, "strategy", &sn, 1);
	}
	for (int pi = 0; pi < NSEARCH; pi++) {
		snprintf(name, sizeof(name), "pattern%d", pi + 1);
		emit_delta_sect(out, name, gd->pattern[pi], gd->npattern[pi]);
		if (gd->pat_has_off[pi])
			sb_printf(out, "=== offset%d %+d ===\n",
				  pi + 1, gd->pat_off[pi]);
		if (gd->pat_has_mode[pi])
			sb_printf(out, "=== mode%d %d ===\n",
				  pi + 1, gd->pat_mode[pi]);
	}
	emit_delta_sect(out, "edit_cmd_abs", gd->abs_cmd, gd->nabs);
	emit_delta_sect(out, "edit_cmd_relc", gd->relc_cmd, gd->nrelc);
	emit_delta_sect(out, "edit_cmd_rel", gd->rel_cmd, gd->nrel);
	if (gd->ph1 || gd->ph2) {
		sb_printf(out, "=== verbatim mark %d esc %d sep %d ===\n",
			  gd->ovr_mark, gd->ovr_esc, gd->ovr_sep);
		sb_printf(out, "=== phase1 ===\n%s\n%s\n",
			  gd->ph1 ? gd->ph1 : "", end_tag_wr);
		sb_printf(out, "=== phase2 ===\n%s\n%s\n",
			  gd->ph2 ? gd->ph2 : "", end_tag_wr);
	}
}

/* A c/i command's inline content as the EDIT COMMAND sections show it: the
 * verb takes the first added line on its own line, the rest follow below; a
 * group that adds nothing just ends the verb line. */
static void wg_content(sbuf *fp, group_t *g)
{
	if (!g->nadd) {
		sb_chr(fp, '\n');
		return;
	}
	sb_chr(fp, ' ');
	for (int k = 0; k < g->nadd; k++) {
		sb_str(fp, g->add_texts[k]);
		sb_chr(fp, '\n');
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

		int has_anchors = group_has_anchors(g);

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
		 * emit_mark_edit, which folds custom_offset into "'N+off"),
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
			 * target when this pattern matches; MODE selects the
			 * search form (see SEARCH MODES). */
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
		winset_t ws;
		gen_extra_windows(g, &ws);
		for (int pi = NPAT; pi < NSEARCH; pi++) {
			int i = pi - NPAT;
			/* recorded-delta mode default: the generator's own mode
			 * for the window slots, single-line for a fuzz slot */
			int def_mode = pi == GRP_SLOT ? 2 : pi >= WIN_SLOT ? 3
				       : (gd && gd->npattern[pi] == 1);
			emit_win_section(fp, gd, pi, &ws.w[i], ws.has[i], def_mode);
		}
		free_extra_windows(&ws);

		/* EDIT COMMAND sections */
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
				wg_content(fp, g);
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
				wg_content(fp, g);
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
					wg_content(fp, g);
				} else if (g->del_start) {
					if (g->ndel == 1)
						sb_str(fp, "d\n");
					else
						sb_printf(fp, ",#+%dd\n", g->ndel - 1);
				} else if (g->nadd) {
					sb_str(fp, add_a ? "i" : "-1i");
					wg_content(fp, g);
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
		free_lines(g->custom_pat[pi], g->ncustom_pat[pi]);
		g->custom_pat[pi] = NULL;
		g->ncustom_pat[pi] = 0;
		g->custom_pat_has_off[pi] = 0;
		g->custom_pat_off[pi] = 0;
		g->custom_pat_has_mode[pi] = 0;
		g->custom_pat_mode[pi] = 0;
	}
	free_lines(g->custom_abs_lines, g->custom_abs_nlines);
	g->custom_abs_lines = NULL;
	g->custom_abs_nlines = 0;
	free_lines(g->custom_relc_lines, g->custom_relc_nlines);
	g->custom_relc_lines = NULL;
	g->custom_relc_nlines = 0;
	free_lines(g->custom_rel_lines, g->custom_rel_nlines);
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
	 * custom_offset is the one from the section that appears last in
	 * the file: whichever the user edited last wins. */
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
		tmp.ovr_sep = g->ovr_sep;
		sbuf_smake(sb, SB_INIT)
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
	g->ovr_sep = 0;
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
static void ed_free_bufs(void)
{
	for (int i = 0; i < xbufcur; i++)
		bufs_free(i);
	xbufcur = 0;
	free(bufs);
	bufs = NULL;
	ex_buf = ex_pbuf = ex_tpbuf = NULL;
	xbufsmax = 0;
}

static void ed_free(void)
{
	ed_free_bufs();
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
	sbuf_smake(sb, SB_INIT)
	for (int i = 0; i < lbuf_len(lb); i++) {
		ln = lbuf_get(lb, i);
		sbuf_mem(sb, ln, lbuf_s(ln)->len + 1)
	}
	sbufn_ret(sb, sb->s)
}

/* One derived (or re-read) compatibility block: one whole compatibility patch,
 * i.e. one unified diff over however many files it touches - not one file. It
 * carries its origin (src= label, human annotation only), the files[] range its
 * diff parsed into, its own === PATCH === lines, its delta customizations and
 * its gate probes. A block is always emitted after the host (post); origin is
 * per-block because the compat_origin global only describes the current run.
 * One block = one section = one staged body = one storage region = one -i
 * buffer, so a compat patch is authored and shipped as the single diff it is. */
typedef struct {
	char *origin;		/* src= label; annotation, never a matcher input */
	int first, count;	/* files[] range this block owns */
	strv_t raw;		/* the block's own === PATCH === lines */
	gate_t gates[GATE_MAXPROBES];
	int ngates;
	/* per-block delta customizations, filled either from the editor
	 * (out) or re-read from a stored block (in) */
	dstore_t deltas;
} compat_block_t;
static compat_block_t *compat_blocks;
static int ncompat, compat_cap;

/* The next free gate anchor tag: one above the highest tag any block already
 * holds (stored blocks read back from the target script included), never below
 * GATE_TAG_BASE. Called once per newly derived block, so a stack of blocks -
 * over one file or over several - numbers its gates continuously instead of
 * every derivation restarting and colliding. */
static int next_gate_tag(void)
{
	int top = GATE_TAG_BASE - 1;
	for (int c = 0; c < ncompat; c++)
		for (int j = 0; j < compat_blocks[c].ngates; j++)
			if (compat_blocks[c].gates[j].tag > top)
				top = compat_blocks[c].gates[j].tag;
	return top + 1;
}

/* Do two blocks probe for the same thing? Everything but the anchor tag, which
 * is per block by construction. */
static int gates_agree(compat_block_t *a, compat_block_t *b)
{
	if (a->ngates != b->ngates)
		return 0;
	for (int j = 0; j < a->ngates; j++) {
		gate_t *x = &a->gates[j], *y = &b->gates[j];
		if (x->polarity != y->polarity || x->nlines != y->nlines)
			return 0;
		if (!!x->path != !!y->path ||
		    (x->path && strcmp(x->path, y->path)))
			return 0;
		for (int k = 0; k < x->nlines; k++)
			if (strcmp(x->lines[k], y->lines[k]))
				return 0;
	}
	return 1;
}

/* Warn when two blocks derived against the same origin carry different gates. A
 * block fires iff its origin is present, and every block of one origin is
 * derived under that same condition, so their gates answer the same question and
 * derivation produces them identically. A disagreement means one of them was
 * hand-edited: the two now fire on different trees, and the back-to-front subset
 * test - which reads one flag per origin, not per block - predicts a sequence
 * that cannot happen. Diagnostic only: widening one gate by hand is legitimate
 * if the author means it, so this reports rather than refuses, and it stays out
 * of the emitted script. */
static void check_compat_gates(void)
{
	for (int a = 0; a < ncompat; a++)
		for (int b = a + 1; b < ncompat; b++) {
			compat_block_t *x = &compat_blocks[a];
			compat_block_t *y = &compat_blocks[b];
			if (strcmp(x->origin, y->origin) || gates_agree(x, y))
				continue;
			fprintf(stderr, "patch2vi: warning: compat blocks %d "
				"and %d (src=%s) carry different gates; "
				"they fire on different trees\n",
				a + 1, b + 1, x->origin);
		}
}

/* The block's own files that have groups to emit; *n = how many. */
static file_patch_t **block_files(compat_block_t *cb, int *n)
{
	file_patch_t **v = emalloc((cb->count + 1) * sizeof(*v));
	*n = 0;
	for (int i = cb->first; i < cb->first + cb->count; i++)
		if (files[i].ngroups > 0)
			v[(*n)++] = &files[i];
	return v;
}

/* One editable unit: a named buffer and its initial text. */
struct edit_ub { const char *name; char *text; };

/*
 * Open one built-in nextvi session over nu named buffers (the host unit first,
 * then one per compat block, in application order) plus an optional trailing
 * .rej buffer, and read every unit's buffer back into out[i]. Buffer i is
 * unit i by bufs_open()'s append order; the name-match assert turns a
 * bufs_find collision (two identically named units) or an eviction that
 * dropped a buffer into a hard error rather than silently routing one unit's
 * edits into another. Returns 0 on success, -1 on error (out[] undefined).
 */
static int edit_units(struct edit_ub *u, int nu,
		      const char *rejname, char *rejtext, char **out)
{
	int need = nu + (rejtext ? 1 : 0);
	/* Keep the whole union resident before the session starts: ed_init()
	 * commits xbufsmax from xbufsalloc, and bufs_open() evicts the top
	 * slot once full - so an undersized cap would silently drop a unit's
	 * buffer instead of appending. Sized above nu, eviction only ever
	 * targets buffers the user opened past the union, never a unit's, so
	 * buffer i stays unit i (bufs_open appends, never reorders). */
	xbufsalloc = MAX(need + 1, MAX(64, xbufsalloc));
	if (ed_init(1) < 0)
		return -1;
	for (int i = 0; i < nu; i++)
		ed_loadbuf(u[i].name, u[i].text);
	if (rejtext) {
		xmpt = 0;
		ed_loadbuf(rejname, rejtext);
	}
	if (ed_run() != 0)
		return -1;
	/* A shrunk buffer count means the user closed one of ours (:bd) or an
	 * eviction reached into the union - either way a unit is gone and its
	 * edits would be mis-read from a wrong buffer. Hard error. The buffer
	 * name is NOT re-checked: :w legitimately renames a buffer's path. */
	if (xbufcur < need) {
		fprintf(stderr, "patch2vi: editor buffer count shrank to %d "
			"(expected at least %d); a unit was dropped\n",
			xbufcur, need);
		return -1;
	}
	for (int i = 0; i < nu; i++)
		out[i] = lbuf_text(bufs[i].lb);
	return 0;
}

/*
 * Inject a unit's stored per-file deltas into fps[]->groups so a later emit
 * (or the interactive PHASE baselines) carry the customizations. Mirrors the
 * host interactive path's first pass: build a delta-injected structured blob,
 * parse it right back and apply to the groups, then attach stored verbatim
 * PHASE overrides (reserving their marks). ds is the unit's store (host
 * in_deltas, or a compat block's own); a NULL/empty store leaves the groups
 * untouched. Files are matched to their delta by path.
 */
static void inject_deltas(file_patch_t **fps, int n, dstore_t *ds)
{
	file_delta_t **fd_per = dstore_per_file(ds, fps, n);

	sbuf_smake(tmp_sb, SB_INIT)
	for (int k = 0; k < n; k++) {
		sb_printf(tmp_sb, "=== FILE: %s ===\n", fps[k]->path);
		write_groups_to_file(tmp_sb,
				     fps[k]->groups, fps[k]->ngroups,
				     fd_per[k], fps[k]->is_new,
				     fps[k]->orig_path ? fps[k]->orig_path
				     : fps[k]->path, 0);
		sb_printf(tmp_sb, "%s\n\n", end_tag_wr);
	}
	sbuf_null(tmp_sb)

	grp_delta_t **pre_per = emalloc(n * sizeof(grp_delta_t *));
	for (int k = 0; k < n; k++)
		pre_per[k] = ecalloc(fps[k]->ngroups, sizeof(grp_delta_t));
	parse_grp_blob(tmp_sb->s, fps, n, pre_per);
	for (int k = 0; k < n; k++) {
		for (int gi = 0; gi < fps[k]->ngroups; gi++) {
			apply_grp_edits(&fps[k]->groups[gi], &pre_per[k][gi]);
			free_grp(&pre_per[k][gi]);
		}
		free(pre_per[k]);
	}
	free(pre_per);
	free(tmp_sb->s);

	for (int k = 0; k < n; k++) {
		if (!fd_per[k])
			continue;
		for (int gi = 0; gi < fps[k]->ngroups; gi++) {
			group_t *g = &fps[k]->groups[gi];
			if (!g->del_start && !g->nadd)
				continue;
			grp_delta_t *gd = find_grp_delta(fd_per[k], gi + 1,
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
			g->ovr_sep = gd->ovr_sep;
			if (gd->ovr_esc != dyn_esc)
				fprintf(stderr, "%s group %d: verbatim override "
						"captured under escape byte %d, current is %d\n",
					fps[k]->path, gi + 1, gd->ovr_esc, dyn_esc);
		}
	}
	free(fd_per);
}

/*
 * Derive one unit's structured delta by comparing its no-injection baseline
 * (orig_blob) against the buffer the user edited, storing only changed groups
 * into the unit's out store, then apply the edits onto fps[]->groups so the
 * later emit sees them. ins is the store consulted to preserve a group-locator
 * custom_text a structured-only edit left untouched. This is the exact host
 * single-unit derivation; the only per-unit variance is the in/out store and
 * (for compat) the relative/compat_building window the caller holds.
 */
static void derive_unit(file_patch_t **fps, int n, dstore_t *ins,
			char *orig_blob, char *edited, dstore_t *out,
			const char *rejname)
{
	file_delta_t **fd_per = dstore_per_file(ins, fps, n);

	grp_delta_t **edit_per = emalloc(n * sizeof(grp_delta_t *));
	for (int k = 0; k < n; k++)
		edit_per[k] = ecalloc(fps[k]->ngroups, sizeof(grp_delta_t));
	parse_grp_blob(edited, fps, n, edit_per);

	grp_delta_t **orig_per = emalloc(n * sizeof(grp_delta_t *));
	for (int k = 0; k < n; k++)
		orig_per[k] = ecalloc(fps[k]->ngroups, sizeof(grp_delta_t));
	parse_grp_blob(orig_blob, fps, n, orig_per);

	for (int k = 0; k < n; k++) {
		file_delta_t *od = NULL;
		for (int gi = 0; gi < fps[k]->ngroups; gi++) {
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
			group_t *g = &fps[k]->groups[gi];
			const char *d1 = g->ph1_ovr ? g->ph1_ovr : g->ph1_gen;
			const char *d2 = g->ph2_ovr ? g->ph2_ovr : g->ph2_gen;
			int verb_ch = (eg->ph1 && d1 && strcmp(eg->ph1, d1) != 0) ||
				      (eg->ph2 && d2 && strcmp(eg->ph2, d2) != 0);
			if (verb_ch) {
				if (struct_ch)
					fprintf(stderr, "%s group %d: structured "
							"edit shadowed by verbatim PHASE "
							"edit\n", fps[k]->path, gi + 1);
				char *n1 = uc_dup(eg->ph1 ? eg->ph1 : (d1 ? d1 : ""));
				char *n2 = uc_dup(eg->ph2 ? eg->ph2 : (d2 ? d2 : ""));
				free(g->ph1_ovr);
				free(g->ph2_ovr);
				g->ph1_ovr = n1;
				g->ph2_ovr = n2;
				g->ovr_mark = eg->ovr_mark > 0 ? eg->ovr_mark
					      : g->mark_id;
				g->ovr_esc = dyn_esc;
				g->ovr_sep = sep;
			} else if (struct_ch && (g->ph1_ovr || g->ph2_ovr)) {
				discard_verbatim_ovr(fps[k]->path, gi + 1,
						     g, rejname);
			}
			int has_ovr = g->ph1_ovr || g->ph2_ovr;

			if (!struct_ch && !has_ovr)
				continue;

			if (!od)
				od = dstore_get(out, fps[k]->path);
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
				  fps[k]->groups[gi].del_texts, fps[k]->groups[gi].ndel);
			arr_clone(&gout->add_lines, &gout->nadd_lines, &gout->add_cap,
				  fps[k]->groups[gi].add_texts, fps[k]->groups[gi].nadd);
			arr_clone(&gout->pre_ctx, &gout->npre_ctx, &gout->pre_cap,
				  fps[k]->groups[gi].all_pre_ctx, fps[k]->groups[gi].nall_pre_ctx);
			arr_clone(&gout->post_ctx, &gout->npost_ctx, &gout->post_cap,
				  fps[k]->groups[gi].post_ctx, fps[k]->groups[gi].npost_ctx);
			/* customization from user's edits; kept even under a
			 * verbatim override — custom_text doubles as the
			 * group-locator regex for starred LEVEL 2/4 matching,
			 * so dropping it would degrade re-entry matching to
			 * index-only (or first-group at level 4). */
			if (custom_ch) {
				arr_clone(&gout->custom_text, &gout->ncustom_text, &gout->custom_text_cap,
					  eg->custom_text, eg->ncustom_text);
			} else if (fd_per[k]) {
				/* preserve existing customization from stored delta */
				grp_delta_t *stored = find_grp_delta(fd_per[k], gi + 1,
								     fps[k]->groups[gi].del_texts, fps[k]->groups[gi].ndel,
								     fps[k]->groups[gi].add_texts, fps[k]->groups[gi].nadd,
								     fps[k]->groups[gi].all_pre_ctx, fps[k]->groups[gi].nall_pre_ctx,
								     fps[k]->groups[gi].post_ctx, fps[k]->groups[gi].npost_ctx,
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
				gout->ovr_sep = g->ovr_sep;
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

	for (int k = 0; k < n; k++) {
		for (int gi = 0; gi < fps[k]->ngroups; gi++)
			free_grp(&orig_per[k][gi]);
		free(orig_per[k]);
	}
	free(orig_per);

	/* Apply edit_per to groups[], transferring ownership of arrays. */
	for (int k = 0; k < n; k++) {
		for (int gi = 0; gi < fps[k]->ngroups; gi++) {
			apply_grp_edits(&fps[k]->groups[gi], &edit_per[k][gi]);
			free_grp(&edit_per[k][gi]);
		}
		free(edit_per[k]);
	}
	free(edit_per);
	free(fd_per);
}

/* An edit unit driven by interactive_edit_all_files: its file range, its
 * inject/preserve store (in), its derived-delta store (out), and - for a
 * compat block - the relative/compat_building window and gate probes its
 * blobs must be built and emitted under. Buffer 0 is always the host; units
 * 1..N are compat blocks in application order. */
typedef struct {
	file_patch_t **fps;
	int n;
	dstore_t *ins;		/* inject/preserve store (in_deltas or cb) */
	dstore_t *out;		/* derived-delta store (out_deltas or cb) */
	int compat;		/* 1 = compat block: hold the compat window */
	gate_t *gates;
	int ngates;
	char name[288];		/* buffer label (unique across units) */
	char *orig;		/* no-injection baseline blob (owned) */
	char *phased;		/* editor text: injected + MARK/PHASE (owned) */
} unit_t;

/* Enter/leave the compat emission window: relative anchoring, the
 * file-validated generators off (they read the pre-origin file, the wrong text
 * for a compat block) and the block's gate tags reserved so regenerated group
 * tags never fuse with them. Held around a block's blob build, its derivation
 * and its body emission alike. sv holds the saved relative_mode for restore. */
static void compat_win_enter(gate_t *gates, int ngates, int *sv)
{
	*sv = relative_mode;
	relative_mode = 1;
	compat_building = 1;
	ncompat_res = ngates;
	for (int j = 0; j < ngates; j++)
		compat_res_marks[j] = gates[j].tag;
}

static void compat_win_leave(int sv)
{
	ncompat_res = 0;
	compat_building = 0;
	relative_mode = sv;
}

/* Build a unit's two blobs: the no-injection baseline (for the later diff)
 * and the injected + MARK/PHASE text shown in the editor. Wraps the compat
 * window for compat units. */
static void build_unit_blobs(unit_t *u)
{
	int sv = 0;
	if (u->compat)
		compat_win_enter(u->gates, u->ngates, &sv);

	file_delta_t **fd_per = dstore_per_file(u->ins, u->fps, u->n);

	sbuf_smake(orig, SB_INIT)
	for (int k = 0; k < u->n; k++) {
		sb_printf(orig, "=== FILE: %s ===\n", u->fps[k]->path);
		write_groups_to_file(orig, u->fps[k]->groups, u->fps[k]->ngroups,
				     NULL, u->fps[k]->is_new,
				     u->fps[k]->orig_path ? u->fps[k]->orig_path
				     : u->fps[k]->path, 0);
		sb_printf(orig, "%s\n\n", end_tag_wr);
	}
	sbuf_null(orig)
	u->orig = orig->s;

	inject_deltas(u->fps, u->n, u->ins);
	for (int k = 0; k < u->n; k++)
		gen_group_segments(u->fps[k]);

	sbuf_smake(ph, SB_INIT)
	for (int k = 0; k < u->n; k++) {
		sb_printf(ph, "=== FILE: %s ===\n", u->fps[k]->path);
		write_groups_to_file(ph, u->fps[k]->groups, u->fps[k]->ngroups,
				     fd_per[k], u->fps[k]->is_new,
				     u->fps[k]->orig_path ? u->fps[k]->orig_path
				     : u->fps[k]->path, 1);
		sb_printf(ph, "%s\n\n", end_tag_wr);
	}
	sbuf_null(ph)
	u->phased = ph->s;

	free(fd_per);
	if (u->compat)
		compat_win_leave(sv);
}

/* Does a stored delta's recorded content match this group at strictness lvl?
 * 1 accepts anything, 2/4 compare the -/+ lines (a starred delta compares its
 * custom_text as one regex instead), 3/5 the whole hunk with its context. */
static int delta_matches_group(grp_delta_t *st, group_t *g, int lvl)
{
	switch (lvl) {
	case 1:
		return 1;
	case 2:
		if (st->has_star && st->level == 2)
			return grp_content_regex_matches(st, g->del_texts, g->ndel,
							 g->add_texts, g->nadd);
		return grp_content_matches(st, g->del_texts, g->ndel,
					   g->add_texts, g->nadd);
	case 4:
		return (st->has_star && st->level == 4
			&& grp_content_regex_matches(st, g->del_texts, g->ndel,
						     g->add_texts, g->nadd))
		       || grp_content_matches(st, g->del_texts, g->ndel,
					      g->add_texts, g->nadd);
	default:   /* 3, 5 */
		return grp_full_hunk_matches(st, g->all_pre_ctx, g->nall_pre_ctx,
					     g->del_texts, g->ndel,
					     g->add_texts, g->nadd,
					     g->post_ctx, g->npost_ctx);
	}
}

/* Will this stored delta find a group to re-apply to? Levels 1-3 test the group
 * at its stored index, levels 4-5 any group of the file. A delta that finds
 * none is rejected: dumped to the .rej buffer and stripped of its star. */
static int delta_applies(grp_delta_t *st, file_patch_t *fp, int lvl)
{
	if (lvl >= 4) {
		for (int i = 0; i < fp->ngroups; i++)
			if (delta_matches_group(st, &fp->groups[i], lvl))
				return 1;
		return 0;
	}
	return st->group_idx <= fp->ngroups
	       && delta_matches_group(st, &fp->groups[st->group_idx - 1], lvl);
}

/*
 * Interactive editing of the host groups and every compat block, one built-in
 * nextvi buffer per unit (host buffer 0, then one per compat block in
 * application order), all held in RAM. Pattern lines are shown regex-escaped
 * (as they'll appear to the regex engine); an untouched buffer parses back to
 * the customizations that were written into it, so unedited units reproduce
 * their input. Host edits land in out_deltas (serialized by the DELTA tail);
 * compat edits are written back into each block's cb->deltas (serialized by
 * emit_compat_storage), and the edited groups feed emit_one_call directly.
 */
static void interactive_edit_all_files(file_patch_t **active, int nactive)
{
	unit_t *units = ecalloc(1 + ncompat, sizeof(*units));
	int nu = 0;
	sbuf *rej = NULL;
	/* Buffer labels, not files: reference the original input (the patch,
	 * or the previously generated script under -d) when it has a name;
	 * .diff/.rej pick up nextvi's diff highlighting. */
	char rejname[288];
	const char *base = input_file ? input_file : "patch2vi";
	snprintf(rejname, sizeof(rejname), "%s.p2v.rej", base);

	/* --- Host unit (buffer 0): its reject pass mutates in_deltas before
	 * injection, so it runs here, not in build_unit_blobs. --- */
	file_delta_t **in_fd_per = dstore_per_file(delta_mode ? &in_deltas : NULL,
						   active, nactive);
	if (nactive > 0) {
		/* Rejection check: before building the editor buffer so we can
		 * clear has_star on rejected deltas (prevents custom_text
		 * injection). */
		for (int k = 0; k < nactive; k++) {
			file_delta_t *in_fd = in_fd_per[k];
			if (!in_fd)
				continue;
			int file_header_written = 0;
			for (int gi = 0; gi < in_fd->ngrps; gi++) {
				grp_delta_t *stored = &in_fd->grps[gi];
				int lvl = delta_mode > 0 ? delta_mode
					  : (stored->level ? stored->level : 2);
				int rejected = !delta_applies(stored, active[k], lvl);
				if (rejected) {
					stored->has_star = 0;
					if (!rej) {
						sbuf_make(rej, SB_INIT)
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
		 * preserved; the deltas are dumped to the .rej buffer so the
		 * user can re-apply them by hand, mirroring -d's reject flow. */
		if (!delta_mode && in_deltas.n) {
			sbuf_make(rej, SB_INIT)
			sb_printf(rej, "# Rejected: interactive (-i)"
				  " discards all stored deltas\n\n");
			for (int di = 0; di < in_deltas.n; di++) {
				file_delta_t *in_fd = &in_deltas.v[di];
				sb_printf(rej, "=== FILE: %s ===\n", in_fd->filepath);
				for (int gi = 0; gi < in_fd->ngrps; gi++)
					emit_grp_delta(rej, &in_fd->grps[gi]);
				sb_printf(rej, "%s\n\n", end_tag_wr);
			}
		}
		if (rej)
			sbuf_null(rej)

		unit_t *hu = &units[nu++];
		hu->fps = active;
		hu->n = nactive;
		hu->ins = delta_mode ? &in_deltas : NULL;
		hu->out = &out_deltas;
		hu->compat = 0;
		snprintf(hu->name, sizeof(hu->name), "%s.p2v.diff", base);
		build_unit_blobs(hu);
	}

	/* --- Compat units: one buffer per block, in application order. Its
	 * baseline is its own stored deltas (no fresh diff to fall back to), so
	 * -i does not discard them; an untouched buffer re-derives them. --- */
	file_patch_t ***ca_store = ecalloc(ncompat + 1, sizeof(*ca_store));
	for (int c = 0; c < ncompat; c++) {
		compat_block_t *cb = &compat_blocks[c];
		int nca;
		file_patch_t **ca = block_files(cb, &nca);
		ca_store[c] = ca;
		if (!nca) {
			free(ca);
			ca_store[c] = NULL;
			continue;
		}
		unit_t *cu = &units[nu];
		cu->fps = ca;
		cu->n = nca;
		cu->ins = &cb->deltas;
		cu->out = &cb->deltas;
		cu->compat = 1;
		cu->gates = cb->gates;
		cu->ngates = cb->ngates;
		/* index prefix keeps two blocks over one file from colliding on
		 * bufs_find (ex.c matches on the name) */
		snprintf(cu->name, sizeof(cu->name), "%d.%s.compat-post.p2v.diff",
			 c, cb->origin ? cb->origin : "");
		build_unit_blobs(cu);
		nu++;
	}

	if (nu == 0) {
		free(units);
		free(ca_store);
		free(in_fd_per);
		return;
	}

	/* Assert buffer names are unique before opening: the index prefix
	 * guarantees it, but a mis-map would route one unit's edits into
	 * another, so verify rather than trust. */
	for (int i = 0; i < nu; i++)
		for (int j = i + 1; j < nu; j++)
			if (strcmp(units[i].name, units[j].name) == 0) {
				fprintf(stderr, "patch2vi: duplicate editor "
					"buffer name \"%s\"\n", units[i].name);
				exit(1);
			}

	struct edit_ub *ubs = ecalloc(nu, sizeof(*ubs));
	char **outs = ecalloc(nu, sizeof(*outs));
	for (int i = 0; i < nu; i++) {
		ubs[i].name = units[i].name;
		ubs[i].text = units[i].phased;
	}
	int rc = edit_units(ubs, nu, rejname, rej ? rej->s : NULL, outs);
	for (int i = 0; i < nu; i++)
		free(units[i].phased);
	if (rej)
		sbuf_free(rej)
	if (rc < 0) {
		for (int i = 0; i < nu; i++)
			free(units[i].orig);
		nu = 0;
	}

	/* Derive each unit's delta from its own buffer. Compat units clear
	 * their store first (a snapshot preserves the pre-edit deltas for the
	 * custom_text preservation lookup) and hold the compat window, since
	 * apply_grp_edits regenerates segments. */
	for (int i = 0; i < nu; i++) {
		unit_t *u = &units[i];
		dstore_t *ins = u->ins;
		dstore_t snap = {0};
		int sv = 0;
		if (u->compat) {
			compat_win_enter(u->gates, u->ngates, &sv);
			/* in and out alias the block's store: copy the pre-edit
			 * entries out, then reset so derive appends fresh. */
			if (u->ins->n > 0) {
				snap.n = u->ins->n;
				snap.v = emalloc(snap.n * sizeof(*snap.v));
				memcpy(snap.v, u->ins->v,
				       snap.n * sizeof(*snap.v));
				ins = &snap;
			}
			u->out->n = 0;
		}
		derive_unit(u->fps, u->n, ins, u->orig, outs[i], u->out,
			    rejname);
		free(snap.v);
		free(outs[i]);
		free(u->orig);
		if (u->compat)
			compat_win_leave(sv);
	}

	for (int c = 0; c < ncompat; c++)
		free(ca_store[c]);
	free(ca_store);
	free(in_fd_per);
	free(units);
	free(ubs);
	free(outs);
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
				/* ;c positions are rune indexes, so the affix is
				 * rune-snapped: a split rune would shift
				 * ldc_start/ldc_end and splice invalid UTF-8 */
				int prefix, suffix;
				common_affix(old, new, &prefix, &suffix);
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

/* Free a group's heap data after emission */
static void free_group(group_t *g)
{
	free(g->del_texts);
	free(g->add_texts);
	free(g->all_pre_ctx);
	free(g->post_ctx);
	for (int pi = 0; pi < NSEARCH; pi++)
		free_lines(g->custom_pat[pi], g->ncustom_pat[pi]);
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

/* A custom EDIT COMMAND at the group's mark. A nonzero custom_offset (a +N/-N
 * pulled off the verb) rides on the mark address as "'N+off", so an
 * insert-above-line-1 ("'N-1i", resolving to the line-0 insert) survives the
 * template round-trip instead of being lost against the patterns' explicit
 * OFFSETs. */
static void emit_mark_edit(sbuf *out, group_t *g, int tline,
			   char **lines, int nlines)
{
	if (g->custom_offset)
		sb_printf(out, "'%d%+d", g->mark_id, g->custom_offset);
	else
		sb_printf(out, "'%d", g->mark_id);
	emit_custom_edit_lines(out, lines, nlines);
	EMIT_SEP(out);
	emit_err_check(out, 2, tline, g->mark_id, NULL, 0);
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
	if (!fp->is_new && !compat_building)
		load_orig_file(fp->orig_path ? fp->orig_path : fp->path);

	/* Drop stale segments from a previous (pre-editor display) run and
	 * reserve every override's mark before any allocation. */
	nreserved_marks = 0;
	/* a compat block's gate holds allocated ?? tags; reserve them so the
	 * group tags below never fuse with them through xanchor */
	for (int ri = 0; ri < ncompat_res; ri++)
		reserve_mark(compat_res_marks[ri]);
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
		sbuf_smake(out, SB_INIT)
		int target_line = g->del_start ? g->del_start : g->add_after;

		/* Strategy: the flags decide non-interactively (relative_mode
		 * -> REL, else ABS); interactively it is the user's choice,
		 * with STRAT_DEFAULT resolved here. */
		int strat = g->strategy;

		int has_anchors = group_has_anchors(g);

		if (!interactive_mode)
			strat = (relative_mode && has_anchors) ? STRAT_REL : STRAT_ABS;
		else if (strat == STRAT_DEFAULT)
			strat = has_anchors ? STRAT_REL : STRAT_ABS;

		/* fall back where the group lacks what the strategy needs */
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
		winset_t ws;           /* owned relaxed windows (plain -r path) */
		memset(&ws, 0, sizeof(ws));
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
			gen_extra_windows(g, &ws);
			for (int pi = NPAT; pi < NSEARCH && nps < NSEARCH; pi++)
				if (ws.has[pi - NPAT])
					nps = push_win_pat(ps, nps, &ws.w[pi - NPAT],
							   pi + 1, pi >= GRP_SLOT);
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
			free_extra_windows(&ws);
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
		free_extra_windows(&ws);
		free(raw);
ph1_done:
		sbuf_null(out)
		g->ph1_gen = out->s;
	}

	/*
	 * Phase 2 (commit): apply edits at the marks, forward order.
	 * Marks auto-adjust as edits shift lines above them.
	 */

	for (int gi = 0; gi < ngroups; gi++) {
		group_t *g = &groups[gi];
		if (!g->del_start && !g->nadd)
			continue;
		sbuf_smake(out, SB_INIT)
		int strat = g->res_strat;
		int tline = g->del_start ? g->del_start : g->add_after;
		EMIT_LB(out);
		EMIT_SEP(out);

		/* Custom abs/rel edit commands apply regardless of del/add shape */
		if (strat == STRAT_ABS && g->custom_abs_lines) {
			emit_custom_edit_lines(out, g->custom_abs_lines,
					       g->custom_abs_nlines);
			EMIT_SEP(out);
			emit_err_check(out, 2, tline, g->mark_id, NULL, 0);
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
				emit_mark_edit(out, g, tline,
					       g->custom_rel_lines,
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
					emit_horiz_span(out, g->ldc_start,
							g->ldc_end,
							g->ldc_new_text);
				}
				emit_err_check(out, 2, tline, g->mark_id, NULL, 0);
			} else if (strat == STRAT_REL && g->ndel == 1 && g->nadd == 1
				   && g->has_line_diff) {
				emit_mark_substitute(out, tline, g->mark_id, g);
			} else if (strat == STRAT_ABS && g->ndel == 1 && g->nadd == 1
				   && g->has_line_diff) {
				sb_printf(out, "'%d", g->mark_id);
				emit_horiz_span(out, g->ldc_start, g->ldc_end,
						g->ldc_new_text);
				emit_err_check(out, 2, tline, g->mark_id, NULL, 0);
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

/* One file's groups as ex commands: absolute mode writes them bottom-to-top
 * (line numbers stay valid, no searches and no marks), the forward modes write
 * every group's phase-1 segment and then every group's phase-2 one. Requires
 * fp->groups from build_file_groups(), plus interactive editing where it
 * applies; the groups are freed here. */
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

/* The state registers, defined at the top of every $VI body. Registers are
 * per editor process and every block is its own $VI, so each body carries
 * its own copy; the body defines the default state first and the shell then
 * contributes whole commands (never fragments) that flip individual
 * switches. Any non-empty value counts as set.
 *
 * The chains: REG_HDLR captures the FAIL line into register 112 (where the
 * INTR chain reads it back), prints it and calls INTR; a phase gate calls
 * REG_HDLR and then the phase's quit chain, so QF only ever fires where the
 * report does, as it did when both were shell fragments. */
static void emit_reg_defaults(sbuf *out)
{
	sb_printf(out, "%dreg ya!112", REG_HDLR);
	EMIT_ESCSEP(out);
	sb_str(out, "prp");
	EMIT_ESCSEP(out);
	sb_printf(out, "p FAIL %%@%d", REG_LOC);
	EMIT_ESCSEP(out);
	sb_str(out, "pr");
	EMIT_ESCSEP(out);
	sb_printf(out, "? %%@%d", REG_INTR);
	EMIT_SEP(out);
	/* phase 2 reports and quits by default, phase 1 does neither */
	sb_printf(out, "%dreg ? %%@%d", REG_ERR2, REG_HDLR);
	EMIT_ESCSEP(out);
	sb_printf(out, "? %%@%d", REG_QF2);
	EMIT_SEP(out);
	sb_printf(out, "%dreg ? %%@%d", REG_OK2, REG_MSG);
	EMIT_SEP(out);
	sb_printf(out, "%dreg vis 2", REG_QF2);
	EMIT_ESCSEP(out);
	sb_str(out, "q!1");
	EMIT_SEP(out);
}

/* The switches, as whole commands the shell either contributes or not.
 * This is the only part of a body sh writes, and it is written into a
 * double-quoted word, so its raw separator bytes are escaped for it.
 * Backslash-newline breaks split the long word for readability; inside the
 * double quotes they are line continuations, so the value is unchanged. */
static void emit_reg_switches(sbuf *out)
{
	sb_printf(out, "${DBG1:+%dreg ? %%@%d", REG_ERR1, REG_HDLR);
	sb_dq_esc_sep(out, 1);
	sb_printf(out, "? %%@%d", REG_QF1);
	sb_dq_esc_sep(out, 0);
	sb_printf(out, "%dreg ? %%@%d", REG_OK1, REG_MSG);
	sb_dq_esc_sep(out, 0);
	sb_str(out, "}\\\n");
	sb_printf(out, "${DBG2:+ya!%d", REG_ERR2);
	sb_dq_esc_sep(out, 0);
	sb_printf(out, "ya!%d", REG_OK2);
	sb_dq_esc_sep(out, 0);
	sb_str(out, "}\\\n");
	sb_printf(out, "${QF1:+%dreg vis 2", REG_QF1);
	sb_dq_esc_sep(out, 1);
	sb_str(out, "q!1");
	sb_dq_esc_sep(out, 0);
	sb_str(out, "}\\\n");
	sb_printf(out, "${QF2:+ya!%d", REG_QF2);
	sb_dq_esc_sep(out, 0);
	sb_str(out, "}\\\n");
	/* the failing site is where the script wrote the location register,
	 * so INTR searches itself for that command's argument */
	sb_printf(out, "${INTR:+%dreg |sc|", REG_INTR);
	sb_dq_esc_sep(out, 1);
	sb_printf(out, "vis 2:fr 0:e $0:83reg %%@47:%%f> %dreg %%@%d:&Q:b0:"
		  "|sc! ", REG_LOC, REG_LOC);
	sb_dq_esc_sep(out, 3);
	sb_str(out, "|:vis 3");
	sb_dq_esc_sep(out, 1);
	sb_str(out, "q1");
	sb_dq_esc_sep(out, 0);
	sb_str(out, "}");
}

/* The tail a $VI body ends with: leave raw ex mode, write each of the nbufs
 * real files (b0..bN-1, the order the call opened them in) and quit. */
static void emit_write_tail(sbuf *out, int nbufs)
{
	sb_str(out, "vis 2");
	EMIT_SEP(out);
	for (int i = 0; i < nbufs; i++) {
		sb_printf(out, "b%d", i);
		EMIT_SEP(out);
		sb_str(out, "w");
		EMIT_SEP(out);
	}
	sb_str(out, "2q");
}

/* The specials prologue every body opens with: "|sc! <esc><sep>|" declares the
 * escape and separator bytes to ex (with the default backslash escape the loc
 * halves a doubled one), then "vis 3" enters raw ex mode. */
static void emit_prologue(sbuf *out)
{
	sb_str(out, "|sc! ");
	sb_chr(out, dyn_esc ? dyn_esc : '\\');
	if (!dyn_esc)
		sb_chr(out, '\\');
	sb_chr(out, sep);
	sb_str(out, "|:vis 3");
	EMIT_SEP(out);
}

/* Open the "printf '%s%s%s\n' ..." that stages a body and write its first two
 * arguments: the prologue plus the default register state (single quoted, so
 * every byte goes out verbatim) and the switches the shell contributes (a
 * double-quoted word). The printf is left open on its third argument - the body
 * proper - which the caller writes. regs = 0 omits both register halves, as a
 * plain absolute script has no state to switch. osb is scratch, left empty. */
static void emit_body_head(sbuf *osb, int regs)
{
	sbuf_cut(osb, 0)
	emit_prologue(osb);
	if (regs)
		emit_reg_defaults(osb);
	fputs("printf '%s%s%s\\n' '", stdout);
	sq_write(osb->s, osb->s_n);
	fputs("'\\\n\"", stdout);
	sbuf_cut(osb, 0)
	if (regs)
		emit_reg_switches(osb);
	sbuf_null(osb)
	fputs(osb->s, stdout);
	fputs("\"\\\n'", stdout);
	sbuf_cut(osb, 0)
}

/* Emit one "$VI -e" invocation: the printf body (|sc! prologue, per-file
 * b<k>/%ya 98/groups, vis 2, the writes and the 2q) staged into $P2VIF, then
 * the EXINIT $VI line naming the files in b<k> order. The host block passes no
 * gate; a compat block passes its gate probes, emitted after the guarded
 * (first) file's register is cached and before its groups, so a tree without
 * the origin's change quits (q!0) before any edit. Buffer indices are the
 * position in active[], which is what vi opens. */
static void emit_vi_block(file_patch_t **active, int nactive)
{
	int forward = relative_mode || interactive_mode;
	int regs = relative_mode || interactive_mode || compat_mode;
	sbuf_smake(osb, SB_INIT)
	/* the three printf arguments sit on their own source lines, joined by
	 * backslash-newline continuations: outside the quotes these splice the
	 * adjacent words with no separator, so the printf output is unchanged */
	emit_body_head(osb, regs);
	if (forward) {
		sb_str(osb, "fr 98");
		EMIT_SEP(osb);
	}
	for (int k = 0; k < nactive; k++) {
		int cache = forward && !active[k]->is_new;
		sb_printf(osb, "b%d", k);
		EMIT_SEP(osb);
		if (cache) {
			sb_str(osb, "%ya 98");
			EMIT_SEP(osb);
		}
		cur_file_path = active[k]->path;
		emit_file_script(osb, active[k]);
	}
	emit_write_tail(osb, nactive);
	sq_write(osb->s, osb->s_n);
	fputs("' > \"$P2VIF\"\n" P2VI_VICALL " $VI -e", stdout);
	for (int k = 0; k < nactive; k++)
		fprintf(stdout, " '%s'", active[k]->path);
	fputs(" \"$P2VIF\"\n", stdout);
	free(osb->s);
}

/* Global buffer index of fp in the unified file list uf[] (the order in which
 * the single $VI call opens the real files as b0..bN-1). Matched by path: the
 * host's and a compat block's entries for the same file are distinct
 * file_patch_t but one physical file, so one shared buffer. */
static int uf_index_path(file_patch_t **uf, int nuf, const char *path)
{
	for (int i = 0; i < nuf; i++)
		if (!strcmp(uf[i]->path, path))
			return i;
	return -1;
}

static int uf_index(file_patch_t **uf, int nuf, file_patch_t *fp)
{
	return uf_index_path(uf, nuf, fp->path);
}

/* Emit a gate's probe search as top-level commands, recording the result into
 * the anchor slot identified by g->tag. The driver's conditional call later
 * reads that anchor to decide whether to run the block.  No sub-chain wrapper
 * is needed: the commands run at the driver's top level with plain EMIT_SEP
 * separators.
 * Single-line patterns use %f> over the live buffer with fr 0 (so ^...$ are
 * per-line anchors). Multi-line patterns fall back to the register cache
 * (fr 98 + %ya 98) where the whole file is one string, so the regex engine
 * sees the embedded newlines and matches correctly. */
static void emit_gate_record(sbuf *out, gate_t *g, int gbuf)
{
	pat_spec_t ps;
	if (g->polarity == GATE_ALWAYS || g->nlines <= 0)
		return;
	memset(&ps, 0, sizeof(ps));
	ps.lines = g->lines;
	ps.nlines = g->nlines;
	sb_printf(out, "b%d", gbuf);
	EMIT_SEP(out);
	/* Multi-line gate: search the register cache, where the whole file is one
	 * string and the embedded newlines are visible to the regex. */
	sb_str(out, ps.nlines > 1 ? "%ya 98" : "1;0");
	EMIT_SEP(out);
	sb_str(out, ps.nlines > 1 ? "fr 98" : "fr 0");
	EMIT_SEP(out);
	sb_str(out, "%f> ");
	emit_chain_pattern(out, &ps);
	EMIT_SEP(out);
	sb_printf(out, "%d??", g->tag);
	EMIT_SEP(out);
}

/* One section's edit body (no prologue, no register defaults, no gate and no
 * vis 2/w/2q tail): the per-file buffer select, its register cache and its
 * generated groups. Buffer indices are global (uf_index), because one $VI call
 * opens every file. The body is staged as its own buffer and executed verbatim
 * through a %@ call, so its top-level separators are raw (EMIT_SEP): %@ inserts
 * the yanked bytes with no rescanning, so the body runs at whatever depth the
 * call sits without any change to its escaping. */
static void emit_section_body(sbuf *out, file_patch_t **files, int nf,
			      file_patch_t **uf, int nuf)
{
	/* The driver expands this body through "2sc % : ?%@<reg>", so xexp is
	 * still '%' when the body begins - but the body's own %ya/%f> use '%'
	 * as the all-lines range, not as an expansion trigger. Reset xexp to 0
	 * up front (the driver prologue's |sc! does the same for the driver);
	 * the body's error sites re-enable it locally through emit_reg_call. */
	sb_str(out, "2sc");
	EMIT_SEP(out);
	sb_str(out, "fr 98");
	EMIT_SEP(out);
	for (int k = 0; k < nf; k++) {
		int gi = uf_index(uf, nuf, files[k]);
		int cache = !files[k]->is_new;
		sb_printf(out, "b%d", gi);
		EMIT_SEP(out);
		if (cache) {
			sb_str(out, "%ya 98");
			EMIT_SEP(out);
		}
		cur_file_path = files[k]->path;
		emit_file_script(out, files[k]);
	}
	/* Strip any trailing separator emitted by the last error check in
	 * emit_file_script: when this body is yanked into a register and
	 * executed via %@, a dangling separator produces an empty command
	 * that ex prints as "unknown command". */
	if (out->s_n > 0 && out->s[out->s_n - 1] == sep)
		out->s_n--;
}

/* A compat block announces itself as the *last* command of its own body: the
 * body only runs when the block's gate resolved present (the driver's %@ call is
 * gated), and reaching its end means every edit in it applied, so the print is
 * proof of application rather than of intent. Silent on a clean tree, where the
 * body is never called. No DBG switch hides it - it is the only outside evidence
 * that a compat block ran. */
static void emit_compat_announce(sbuf *out, char *origin)
{
	EMIT_SEP(out);
	sb_printf(out, "p compat applied: src=%s", origin ? origin : "");
}

/* Stage one section body as a shell here-string into "$P2VIF".<idx>, the file
 * the single $VI call opens as a buffer. */
static void stage_section(sbuf *body, int idx)
{
	printf("printf '%%s\\n' '");
	sq_write(body->s, body->s_n);
	printf("' > \"$P2VIF\".%d\n", idx);
}

/* A section to run in the single call: its files, its register, and (for a
 * compat block) its gate and the block it customizes from. */
typedef struct {
	file_patch_t **files;
	int nf;
	gate_t *gates;
	int ngates;
	int reg;		/* register the driver yanks/executes the body from */
	int secbuf;		/* global buffer index of the staged body */
	int flagk;		/* per-origin flag slot (REG_FLAG_BASE+flagk); -1 host */
	compat_block_t *cb;	/* NULL for the host section */
} section_t;

/* Emit a section's combined gate expression - "tag1,tag2??" (present) or
 * "tag1,tag2??!" (absent), or a bare "?" when the section has no real gate.
 * ',' is AND; every gate in a block shares one polarity, so the trailing "!"
 * is decided by the last real gate. Written raw at the driver's top level. */
static void emit_gate_expr(sbuf *out, section_t *s)
{
	int present = 1, real = 0;
	for (int j = 0; j < s->ngates; j++) {
		gate_t *g = &s->gates[j];
		if (g->polarity == GATE_ALWAYS || g->nlines <= 0)
			continue;
		sb_printf(out, "%s%d", real ? "," : "", g->tag);
		present = g->polarity == GATE_PRESENT;
		real++;
	}
	if (real)
		sb_printf(out, "??%s ", present ? "" : "!");
	else
		sb_str(out, "? ");
}

/* Whether a section carries a real (non-ALWAYS, non-empty) gate, i.e. it emits
 * a sensor and thus a per-origin / shared flag. The host never does. */
static int section_has_gate(section_t *s)
{
	for (int j = 0; j < s->ngates; j++)
		if (s->gates[j].polarity != GATE_ALWAYS && s->gates[j].nlines > 0)
			return 1;
	return 0;
}

/* Do two sections edit any file in common (by path)? Blocks only stack over a
 * shared file, so the per-block subset test only considers later blocks that
 * touch the same file. */
static int sections_share_file(section_t *a, section_t *b)
{
	for (int i = 0; i < a->nf; i++)
		for (int j = 0; j < b->nf; j++)
			if (!strcmp(a->files[i]->path, b->files[j]->path))
				return 1;
	return 0;
}

/* Redefine REG_QF2 (211) to the plain assert form ("vis 2; q!1"): an error site
 * that calls 211 reports (already done upstream) and quits 1. deep=0 emits at
 * the driver's top level (like emit_reg_defaults); deep=1 emits inside a ??/??!
 * then-arg, one ex_exec level down, so the separator 211 must *capture* carries
 * the extra escape level.
 * The trailing separator is always raw: it ends this command in the stream the
 * driver's top level parses, and ex_arg stops a ??-arm's argument at the first
 * unescaped separator. Escaping it would fold every following driver command
 * into the arm - the arm's own body would then only run when the arm fired, and
 * the block's buffer select and %ya would go with it. */
static void emit_qf2_assert(sbuf *out, int deep)
{
	sb_printf(out, "%dreg vis 2", REG_QF2);
	emit_sep_lvl(out, deep);
	sb_str(out, "q!1");
	EMIT_SEP(out);
}

/* Redefine REG_QF2 (211) to empty: suppress the quit so error sites in the
 * following body still report (via REG_ERR2 -> REG_HDLR) but do not abort.
 * deep is unused for the same reason the assert's terminator is raw - there is
 * no payload separator here, only the terminator. */
static void emit_qf2_clear(sbuf *out)
{
	sb_printf(out, "%dreg", REG_QF2);
	EMIT_SEP(out);
}

/* Host quit override, emitted once before the host body when sensors exist:
 * "211reg fr <ANY>:f> 1:??!vis 2:q!1". A miss on the shared any-origin flag
 * (no sensor fired, e.g. a clean tree) is an error that ??! catches and quits
 * on, exactly as a non-compat script does; a hit (some origin present) leaves
 * 211's ??! silent, so the host is best-effort and falls through its own
 * mismatches. This lives inside 211 and re-runs per error site, so the shared
 * flag is read with a register f>, never an accumulating anchor.
 *
 * The trailing "fr 98" is not decoration: ex_find's register redirection is the
 * global xfr, so reading the flag through "fr <ANY>" leaves every later search
 * pointed at that register. Since this chain fires from inside a body - at an
 * error site, on the very tree where the host is expected to miss - the next
 * group's search would read the flag's "1" instead of the file cache and miss
 * too, and the whole rest of the body would fall over silently. Restore the
 * body's invariant (a group's search reads the cache) on the way out; the
 * firing side quits, so it needs none. */
static void emit_host_override(sbuf *out)
{
	sb_printf(out, "%dreg fr %d", REG_QF2, REG_FLAG_ANY);
	EMIT_ESCSEP(out);
	sb_str(out, "f> 1");
	EMIT_ESCSEP(out);
	sb_str(out, "?" "?!vis 2");
	EMIT_ESC3SEP(out);
	sb_str(out, "q!1");
	EMIT_ESCSEP(out);
	sb_str(out, "fr 98");
	EMIT_SEP(out);
}

/* Block-head quit policy: a compat block asserts iff no later block over the
 * same file has a fired origin. secs[i] is the block; scan secs[i+1..nsec) for
 * later same-file blocks and record each origin's presence as an anchor slot,
 * then OR the slots to (re)define 211 for this block's body: any later present
 * -> suppress (clear 211); none later -> assert (default 211). A statically-last
 * block (no later same-file blocks) skips the test and unconditionally asserts,
 * since the host override left 211 relaxed and it must be restored. */
static void emit_block_qf2(sbuf *out, section_t *secs, int nsec, int i)
{
	int nlater = 0, slot;
	for (int j = i + 1; j < nsec; j++)
		if (secs[j].cb && secs[j].flagk >= 0 &&
		    sections_share_file(&secs[i], &secs[j]))
			nlater++;
	if (!nlater) {
		emit_qf2_assert(out, 0);
		return;
	}
	slot = FLAG_SLOT_BASE;
	for (int j = i + 1; j < nsec; j++) {
		if (!(secs[j].cb && secs[j].flagk >= 0 &&
		      sections_share_file(&secs[i], &secs[j])))
			continue;
		sb_printf(out, "fr %d", REG_FLAG_BASE + secs[j].flagk);
		EMIT_SEP(out);
		sb_str(out, "f> 1");
		EMIT_SEP(out);
		sb_printf(out, "%d?" "?", slot++);
		EMIT_SEP(out);
	}
	for (int k = 0; k < nlater; k++)
		sb_printf(out, "%s%d", k ? ";" : "", FLAG_SLOT_BASE + k);
	sb_str(out, "?" "?");
	emit_qf2_clear(out);
	for (int k = 0; k < nlater; k++)
		sb_printf(out, "%s%d", k ? ";" : "", FLAG_SLOT_BASE + k);
	sb_str(out, "?" "?!");
	emit_qf2_assert(out, 1);
}

/* Sensor half of a section's orchestration: run its gate searches, recording
 * each under its own tag. The host section has no gates and contributes
 * nothing. Every sensor runs before any body (the driver emits all sensors
 * first), and their tags persist in xanchor to the call half below - the whole
 * driver is one top-level ex_exec, so a tag recorded here is still readable when
 * the matching call runs after the host body. */
static void emit_driver_sensors(sbuf *out, section_t *s,
				file_patch_t **uf, int nuf, int *anyinit)
{
	int real = 0;
	for (int j = 0; j < s->ngates; j++) {
		gate_t *g = &s->gates[j];
		if (g->polarity == GATE_ALWAYS || g->nlines <= 0)
			continue;
		/* a cross-file probe names its own file; otherwise the block's */
		emit_gate_record(out, g, g->path
				 ? uf_index_path(uf, nuf, g->path)
				 : uf_index(uf, nuf, s->files[0]));
		real++;
	}
	if (!real)
		return;
	/* Define both flags as "0" before anything reads them. An unset register
	 * also reads false, but through the "uninitialized register" error, which
	 * the reader prints: every clean-tree subset test would announce itself.
	 * "0" misses the flags' "f> 1" search silently, and a later "reg+ 1"
	 * appends to it, so the fired side still reads true. */
	if (!*anyinit) {
		sb_printf(out, "%dreg 0", REG_FLAG_ANY);
		EMIT_SEP(out);
		*anyinit = 1;
	}
	sb_printf(out, "%dreg 0", REG_FLAG_BASE + s->flagk);
	EMIT_SEP(out);
	/* On the fired side, raise this block's per-origin flag (read by the
	 * per-block subset test) and append to the shared any-origin flag (read
	 * once by the host override). A miss leaves both at "0" (= false). */
	emit_gate_expr(out, s);
	sb_printf(out, "%dreg 1", REG_FLAG_BASE + s->flagk);
	EMIT_SEP(out);
	emit_gate_expr(out, s);
	sb_printf(out, "%dreg+ 1", REG_FLAG_ANY);
	EMIT_SEP(out);
}

/* Call half: yank the section body into its register, then %@-call it -
 * unconditionally for the host, or gated on the sensor tags for a compat block.
 * Every %@ call is bracketed with the "2sc %" / "2sc" expansion window
 * (emit_reg_call's discipline) because the driver prologue's |sc! leaves xexp
 * inert. */
static void emit_driver_call(sbuf *out, section_t *secs, int nsec, int i,
			     file_patch_t **uf, int nuf)
{
	section_t *s = &secs[i];
	/* Rewind every real file this section touches to line 1 before running
	 * it: the gate sensors and any earlier block leave the cursor deep in
	 * the buffer, and the body's relative searches (";0fr.,$f>") key off
	 * the current line, so a leftover row would steer them past matches. */
	for (int k = 0; k < s->nf; k++) {
		sb_printf(out, "b%d", uf_index(uf, nuf, s->files[k]));
		EMIT_SEP(out);
		sb_str(out, "1");
		EMIT_SEP(out);
	}
	/* Set this block's quit policy before its body runs: assert if it is the
	 * last firing block over its file, suppress otherwise. */
	if (s->cb)
		emit_block_qf2(out, secs, nsec, i);
	sb_printf(out, "b%d", s->secbuf);
	EMIT_SEP(out);
	sb_printf(out, "%%ya %d", s->reg);
	EMIT_SEP(out);
	sb_str(out, "2sc %");
	EMIT_SEP(out);
	emit_gate_expr(out, s);
	sb_printf(out, "%%@%d", s->reg);
	EMIT_SEP(out);
	sb_str(out, "2sc");
	EMIT_SEP(out);
}

/* Emit the whole patch as a single $VI call: the real files as b0..bN-1, then
 * one staged buffer per section (the host and each compat block), then a driver
 * buffer that EXINIT yanks into register 97 and runs. The driver defines the
 * state registers once, runs each section in application order (host, then
 * every post-compat block) through a %@ call, and finally writes every real file and
 * quits. A gate that misses simply skips its block's call; nothing quits the
 * shared process, so later sections still run. */
static void emit_one_call(file_patch_t **active, int nactive)
{
	section_t *secs = emalloc((ncompat + 1) * sizeof(*secs));
	int nsec = 0, compat_reg = 50;
	int nprobe = 0, nwrite;
	file_patch_t *probes;
	file_patch_t **uf = emalloc((nfiles + ncompat * GATE_MAXPROBES + 1) *
				    sizeof(*uf));
	int nuf = 0;
	for (int i = 0; i < nfiles; i++)
		if (files[i].ngroups > 0 && uf_index(uf, nuf, &files[i]) < 0)
			uf[nuf++] = &files[i];
	/* Files only a cross-file gate probe reads: the call must open them so
	 * the sensor has a buffer to search, but nothing edits them, so they go
	 * after every edited file and the write tail stops short of them. */
	nwrite = nuf;
	probes = ecalloc(ncompat * GATE_MAXPROBES + 1, sizeof(*probes));
	for (int c = 0; c < ncompat; c++)
		for (int j = 0; j < compat_blocks[c].ngates; j++) {
			char *gp = compat_blocks[c].gates[j].path;
			if (!gp || uf_index_path(uf, nuf, gp) >= 0)
				continue;
			probes[nprobe].path = gp;
			uf[nuf++] = &probes[nprobe++];
		}

	/* Sections in run order: host, then every compat block (all post). */
	if (nactive > 0) {
		secs[nsec].files = active;
		secs[nsec].nf = nactive;
		secs[nsec].gates = NULL;
		secs[nsec].ngates = 0;
		secs[nsec].reg = P2VI_REG;
		secs[nsec].flagk = -1;
		secs[nsec].cb = NULL;
		nsec++;
	}
	int nflag = 0;
	for (int c = 0; c < ncompat; c++) {
		compat_block_t *cb = &compat_blocks[c];
		int nca;
		file_patch_t **ca = block_files(cb, &nca);
		if (!nca) {
			free(ca);
			continue;
		}
		secs[nsec].files = ca;
		secs[nsec].nf = nca;
		secs[nsec].gates = cb->gates;
		secs[nsec].ngates = cb->ngates;
		secs[nsec].reg = compat_reg++;
		secs[nsec].flagk = nflag++;
		secs[nsec].cb = cb;
		nsec++;
	}

	fputs("# Body too large for EXINIT/argv: stage it in a file\n"
	      "( : > /tmp/p2vi.$$.d ) 2>/dev/null && P2VIF=/tmp/p2vi.$$ || P2VIF=./p2vi.$$\n"
	      "trap 'rm -f \"$P2VIF\".*' EXIT\n", stdout);

	/* Assign each section its global buffer index up front so the driver can
	 * reference them before the bodies are staged. */
	for (int i = 0; i < nsec; i++)
		secs[i].secbuf = nuf + i;

	/* Stage the driver (".d") first: it carries every gate, and the gates
	 * must run before any section body, so it heads the list of printfs.
	 * Layout: prologue + register defaults (arg1), shell switches (arg2),
	 * orchestration + final writes (arg3). */
	sbuf_smake(osb, SB_INIT)
	emit_body_head(osb, 1);
	/* All sensors first (they record their tags), then the bodies in run
	 * order: host, then every compat block. Sensors-before-bodies is the C1
	 * layout - the host body can consult the sensor results. */
	int any_sensor = 0, anyinit = 0;
	for (int i = 0; i < nsec; i++) {
		emit_driver_sensors(osb, &secs[i], uf, nuf, &anyinit);
		if (secs[i].cb && section_has_gate(&secs[i]))
			any_sensor = 1;
	}
	/* Host quit override goes after every sensor (they set the flags) and
	 * before any body. Only when a sensor exists; otherwise 211 stays at its
	 * emit_reg_defaults value and non-compat scripts remain byte-identical. */
	if (any_sensor)
		emit_host_override(osb);
	for (int i = 0; i < nsec; i++)
		emit_driver_call(osb, secs, nsec, i, uf, nuf);
	emit_write_tail(osb, nwrite);
	sq_write(osb->s, osb->s_n);
	printf("' > \"$P2VIF\".d\n");
	free(osb->s);

	/* Stage each section body after the driver. */
	for (int i = 0; i < nsec; i++) {
		section_t *s = &secs[i];
		int sv_rel = 0;
		if (s->cb) {
			printf("# Compat (post) from %s\n",
			       s->cb->origin ? s->cb->origin : "");
			compat_win_enter(s->cb->gates, s->cb->ngates, &sv_rel);
			if (!interactive_mode)
				inject_deltas(s->files, s->nf, &s->cb->deltas);
		}
		sbuf_smake(bsb, SB_INIT)
		emit_section_body(bsb, s->files, s->nf, uf, nuf);
		if (s->cb)
			emit_compat_announce(bsb, s->cb->origin);
		sbuf_null(bsb)
		stage_section(bsb, i);
		free(bsb->s);
		if (s->cb)
			compat_win_leave(sv_rel);
	}

	/* The single call: real files, section bodies, driver last (current at
	 * EXINIT, so %ya 97 yanks it). */
	fputs(P2VI_VICALL " $VI -e", stdout);
	for (int i = 0; i < nuf; i++)
		printf(" '%s'", uf[i]->path);
	for (int i = 0; i < nsec; i++)
		printf(" \"$P2VIF\".%d", i);
	printf(" \"$P2VIF\".d\n");

	for (int i = 0; i < nsec; i++)
		if (secs[i].cb)
			free(secs[i].files);
	free(secs);
	free(uf);
	free(probes);
}

/* Serialize a delta store as === DELTA <path> === sections; empty entries
 * (a file whose groups were all left alone) are skipped. */
static void emit_dstore(dstore_t *ds)
{
	sbuf_smake(sb, SB_INIT)
	for (int i = 0; i < ds->n; i++) {
		file_delta_t *od = &ds->v[i];
		if (od->ngrps == 0)
			continue;
		printf("=== DELTA %s ===\n", od->filepath);
		sbuf_cut(sb, 0)
		for (int j = 0; j < od->ngrps; j++)
			emit_grp_delta(sb, &od->grps[j]);
		sbuf_null(sb)
		fputs(sb->s, stdout);
		printf("%s\n", end_tag_wr);
	}
	free(sb->s);
}

static const char *gate_polarity_word(int p)
{
	return p == GATE_PRESENT ? "present"
	     : p == GATE_ABSENT  ? "absent" : "always";
}

/* Serialize every compat block into a terminator-fenced tail region after
 * exit 0 and before the host === PATCH2VI PATCH === (which stays last, to EOF,
 * so the patch(1) fallback's sed is unaffected). One region per compat patch,
 * self-contained - its gate probes, its delta customizations and its whole
 * unified diff, every file of it - so -d regenerates it and -i edits it without
 * re-running the origin. === COMPAT PATCH === is that diff and nothing else: a
 * verbatim store, so a -co third argument comes back out of it as the patch the
 * author handed in (modulo the user's edits on top).
 * The inner sub-sections are === END ===-closed exactly like the host DELTA
 * sub-sections, so the reader closes them through its existing end_tag handling
 * and reaches === END COMPAT === with no section open. */
static void emit_compat_storage(void)
{
	for (int c = 0; c < ncompat; c++) {
		compat_block_t *cb = &compat_blocks[c];
		printf("=== PATCH2VI COMPAT post src=%s ===\n",
		       cb->origin ? cb->origin : "");
		for (int j = 0; j < cb->ngates; j++) {
			gate_t *g = &cb->gates[j];
			/* a cross-file probe records the file it searches; a
			 * plain one has none and reads the block's own file */
			if (g->path)
				printf("=== GATE %d %s tag %d probe %s ===\n",
				       j + 1, gate_polarity_word(g->polarity),
				       g->tag, g->path);
			else
				printf("=== GATE %d %s tag %d ===\n", j + 1,
				       gate_polarity_word(g->polarity), g->tag);
			for (int k = 0; k < g->nlines; k++)
				printf("%s\n", g->lines[k]);
			printf("%s\n", end_tag_wr);
		}
		printf("=== COMPAT DELTA ===\n");
		emit_dstore(&cb->deltas);
		printf("%s\n", end_tag_wr);
		printf("=== COMPAT PATCH ===\n");
		for (int i = 0; i < cb->raw.n; i++)
			fputs(cb->raw.v[i], stdout);
		printf("%s\n", end_tag_wr);
		printf("=== END COMPAT ===\n");
	}
}

/* set by "--- /dev/null", consumed by the next "+++" */
static int pending_is_new;
/* "---" path (pre-patch original), consumed by the next "+++" */
static char *pending_orig_path;

static void new_file(const char *path)
{
	ARR_PUSH(files, nfiles, files_cap)
	files[nfiles].path = uc_dup(path);
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
	ARR_PUSH(fp->ops, fp->nops, fp->ops_cap)
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

static shvar_t *shvars;
static int nshvars, shvars_cap;

static const char *sh_get(const char *name)
{
	for (int i = 0; i < nshvars; i++)
		if (!strcmp(shvars[i].name, name))
			return shvars[i].val;
	return getenv(name);
}

/* Header assignments belong to one script; a session replaying two of
 * the target (-co) must not let the first script's assignments shadow the
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
	ARR_PUSH(shvars, nshvars, shvars_cap)
	shvars[nshvars].name = uc_dup(name);
	shvars[nshvars].val = uc_dup(val);
	nshvars++;
}

static int sh_err(const char *what, const char *s)
{
	fprintf(stderr, "-e: unsupported %s: %s\n", what, s);
	return -1;
}

/* The [A-Za-z0-9_] run at *s as a fresh string, advancing *s past it.
 * Empty when *s does not start a name; the caller frees. */
static char *sh_name(const char **s)
{
	sbuf_smake(sb, 32)
	while (isalnum((unsigned char)**s) || **s == '_')
		sbuf_chr(sb, *(*s)++)
	sbufn_ret(sb, sb->s)
}

/* Expand one double-quoted shell word: ${VAR}, ${VAR:-default} and
 * ${VAR:+alternate} (both nestable), $VAR, $0, $(printf '\NNN') and the
 * escapes that survive double quotes. Everything else is literal. */
static int sh_expand(const char *s, sbuf *out)
{
	char *name;
	int j;
	while (*s) {
		if (*s == '\\' && s[1] == '\n') {	/* line continuation */
			s += 2;
			continue;
		}
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
			name = sh_name(&s);
			j = *name != '\0';
			val = j ? sh_get(name) : NULL;
			free(name);
			if (!j)
				return sh_err("expansion", s);
			if (*s == '}') {
				s++;
				if (val)
					sbuf_str(out, val)
				continue;
			}
			if (s[0] != ':' || (s[1] != '-' && s[1] != '+'))
				return sh_err("expansion", s);
			int alt = s[1] == '+';
			s += 2;
			/* the word runs to the matching brace and is
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
			/* ":-" takes the word when unset, ":+" when set */
			if ((val && *val ? 1 : 0) != alt) {
				if (!alt)
					sbuf_str(out, val)
			} else {
				sbuf_smake(def, SB_INIT)
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
			name = sh_name(&s);
			if ((val = sh_get(name)))
				sbuf_str(out, val)
			free(name);
			continue;
		}
		return sh_err("expansion", s - 1);
	}
	return 0;
}

/* NAME=value, with value optionally double quoted */
static int sh_assign(const char *s)
{
	char *name = sh_name(&s);
	int ret;
	if (!*name || *s != '=') {
		free(name);
		return sh_err("assignment", s);
	}
	s++;
	sbuf_smake(val, SB_INIT)
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
	sbuf_smake(out, SB_INIT)
	if (!(ret = sh_expand(s, out))) {
		sbuf_null(out)
		sh_set(name, out->s);
	}
	free(out->s);
	free(val->s);
	return ret;
}

typedef struct {
	char **paths;	/* real files, in $VI argument order */
	int npaths;
	char *body;	/* the printf'd ex command body (driver body, new shape) */
	int sep;	/* the script's separator byte, its commands' delimiter */
	char **sects;	/* new shape: one staged section body per buffer, in index
			 * order; NULL/0 for the old single-body shape */
	int nsects;
} p2vi_block_t;

/* One editor lifetime: the real files as b0..bN-1, then (new shape) one
 * in-RAM buffer per staged section body b<N>..b<N+nsects-1>, then the driver
 * body. EXINIT only exists to lift the body out of the buffer the shell had
 * to pass it in; -e holds the body already, so it fills the register the body
 * may recurse through and runs the chain itself. In the new multi-buffer
 * shape the driver does its own per-section "b<k>:%ya <reg>:...%@<reg>", so
 * the section bodies must be resident as buffers before the driver runs -
 * their buffer indices are exactly the emitter's uf-count + section index. */
static int run_body(p2vi_block_t *blk)
{
	int st;
	if (ed_init(0) < 0)
		return -1;
	xvis |= 2;
	xbufsalloc = MAX(blk->npaths + blk->nsects + 1, MAX(64, xbufsalloc));
	ec_setbufsmax(NULL, NULL, "");
	for (int i = 0; i < blk->npaths; i++) {
		xmpt = 0;
		ec_edit("", "e", blk->paths[i]);
	}
	for (int i = 0; i < blk->nsects; i++) {
		char sname[32];
		snprintf(sname, sizeof(sname), "*p2vi-sec-%d*", i);
		xmpt = 0;
		ed_loadbuf(sname, blk->sects[i]);
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
	for (int i = 0; i < blk->nsects; i++)
		free(blk->sects[i]);
	free(blk->sects);
	free(blk->body);
	memset(blk, 0, sizeof(*blk));
}

/* The script's separator byte, the delimiter of the body's commands. It is
 * declared where ex learns it, in the body's own "|sc! <esc><sep>|"
 * prologue: with a dynamic escape byte the loc holds the two bytes as they
 * are, with the default backslash escape it holds a doubled one that the
 * loc's ex_se_read halves. Read per block, since a replay may span two
 * scripts and each body keeps its own separator. */
static int body_sepbyte(const char *body)
{
	const char *p = body, *e;
	if (!strncmp(p, "|sc! ", 5) && (e = strchr(p + 5, '|'))) {
		p += 5;
		if (e - p == 2)
			return (unsigned char)p[1];
		if (e - p == 3 && p[0] == '\\' && p[1] == '\\')
			return (unsigned char)p[2];
	}
	fprintf(stderr, "replay: body has no specials prologue\n");
	return -1;
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
			/* new shape names section/driver buffers as "$P2VIF".<sfx>;
			 * the section bodies are loaded from the staged printfs, so
			 * here the suffix is only skipped, not turned into a path */
			if (*s == '.')
				while (*s && *s != ' ')
					s++;
			continue;
		}
		if (*s != '\'' || !(p = strchr(s + 1, '\'')))
			return sh_err("vi call", s);
		blk->paths = erealloc(blk->paths,
				      (blk->npaths + 1) * sizeof(char *));
		blk->paths[blk->npaths++] = dup_n(s + 1, p - s - 1);
		s = p + 1;
	}
	return 0;
}

/* One shell word: adjacent single-quoted, double-quoted and unquoted runs
 * concatenate into it. A single-quoted run is verbatim (and holds no
 * quote of its own, the generator writes '\'' for those), a double-quoted
 * one goes through sh_expand. Advances *sp past the word. */
static int sh_word(const char **sp, sbuf *out)
{
	const char *s = *sp, *e;
	int ret = 0;
	while (*s && *s != ' ' && *s != '\n') {
		if (*s == '\'') {
			if (!(e = strchr(++s, '\'')))
				return sh_err("quoting", s);
			sbuf_mem(out, s, e - s)
			s = e + 1;
			continue;
		}
		if (*s != '"') {
			/* backslash-newline is a line continuation: both bytes
			 * vanish, so a word wrapped across lines stays one word
			 * (this is what splices the three printf arguments) */
			if (*s == '\\' && s[1] == '\n') {
				s += 2;
				continue;
			}
			if (*s == '\\' && s[1])
				s++;
			sbuf_chr(out, *s++)
			continue;
		}
		for (e = ++s; *e && *e != '"'; e++)
			e += *e == '\\' && e[1];
		if (!*e)
			return sh_err("quoting", s);
		sbuf_smake(dq, SB_INIT)
		sbuf_mem(dq, s, e - s)
		sbuf_null(dq)
		ret = sh_expand(dq->s, out);
		free(dq->s);
		if (ret < 0)
			return ret;
		s = e + 1;
	}
	*sp = s;
	return 0;
}

/* The staged body: "printf <format> <word>..." with a format that only
 * concatenates its arguments and ends the line. The words carry the
 * command chain; sh writes at most one of them, and only ever whole
 * commands, so nothing here can change how the rest is parsed. */
static int sh_printf_body(const char *s, sbuf *out)
{
	int ret;
	sbuf_smake(fmt, SB_INIT)
	if (strncmp(s, "printf ", 7)) {
		free(fmt->s);
		return sh_err("body", s);
	}
	s += 7;
	if (!(ret = sh_word(&s, fmt))) {
		const char *p = fmt->s;
		sbuf_null(fmt)
		while (p[0] == '%' && p[1] == 's')
			p += 2;
		if (p == fmt->s || strcmp(p, "\\n"))
			ret = sh_err("body format", fmt->s);
	}
	free(fmt->s);
	while (ret >= 0 && *s == ' ') {
		while (*s == ' ')
			s++;
		ret = sh_word(&s, out);
	}
	if (ret >= 0)
		sbuf_chr(out, '\n')
	return ret;
}

/* Expand one raw "printf <fmt> <word>..." command into its emitted text. */
static int expand_body(const char *raw, char **out)
{
	int ret;
	sbuf_smake(exp, SB_INIT)
	ret = sh_printf_body(raw, exp);
	sbuf_null(exp)
	if (ret >= 0)
		*out = uc_dup(exp->s);
	free(exp->s);
	return ret;
}

/* Staged bodies gathered between two "$VI" calls, keyed by the "$P2VIF"
 * suffix the printf redirects into: "" for the old single-body shape,
 * "d" for the new driver, and "0","1",... for the section bodies. */
typedef struct {
	char **raw;	/* the raw printf command, one per staged body */
	char **suf;	/* its "$P2VIF" suffix */
	int n;
} pend_t;

static void pend_push(pend_t *p, const char *raw, const char *suf)
{
	p->raw = erealloc(p->raw, (p->n + 1) * sizeof(char *));
	p->suf = erealloc(p->suf, (p->n + 1) * sizeof(char *));
	p->raw[p->n] = uc_dup(raw);
	p->suf[p->n] = uc_dup(suf);
	p->n++;
}

static void pend_clear(pend_t *p)
{
	for (int i = 0; i < p->n; i++) {
		free(p->raw[i]);
		free(p->suf[i]);
	}
	free(p->raw);
	free(p->suf);
	memset(p, 0, sizeof(*p));
}

/* Turn the gathered staged bodies into one block. Old shape: a single "" body
 * becomes blk.body. New shape: the "d" body is the driver (blk.body) and the
 * numeric bodies are the per-buffer section bodies (blk.sects, in index
 * order). parse_vi_call has already filled blk.paths. */
static int pend_finish(pend_t *p, p2vi_block_t *blk)
{
	int drv = -1, old = -1, nsec = 0, ret = 0;
	for (int i = 0; i < p->n; i++) {
		if (!p->suf[i][0])
			old = i;
		else if (!strcmp(p->suf[i], "d"))
			drv = i;
		else if (atoi(p->suf[i]) + 1 > nsec)
			nsec = atoi(p->suf[i]) + 1;
	}
	if (old >= 0) {
		ret = expand_body(p->raw[old], &blk->body);
	} else if (drv >= 0) {
		blk->sects = ecalloc(nsec, sizeof(char *));
		blk->nsects = nsec;
		for (int i = 0; ret >= 0 && i < p->n; i++) {
			if (p->suf[i][0] == 'd' || !p->suf[i][0])
				continue;
			ret = expand_body(p->raw[i], &blk->sects[atoi(p->suf[i])]);
		}
		if (ret >= 0)
			ret = expand_body(p->raw[drv], &blk->body);
	} else
		ret = sh_err("body", "no staged body before $VI call");
	if (ret >= 0)
		blk->sep = body_sepbyte(blk->body);
	return ret;
}

/* Read the script's executable region (everything before "exit 0") into
 * one block per editor invocation. Parsing is separate from running: -e
 * runs each block in its own editor lifetime, while the compat session
 * replays them all in one, and that needs the whole list up front. */
static int parse_p2vi_script(FILE *in, p2vi_block_t **blks, int *nblks)
{
	const char *body_end = " > \"$P2VIF\"";
	p2vi_block_t blk = {0};
	pend_t pend = {0};
	int skip = 0, in_body = 0, ret = 0, j;
	char *line, *sufdup = NULL;
	sbuf_smake(lb, SB_INIT)
	sbuf_smake(body, SB_INIT)
	while (ret >= 0 && (line = read_line(in, lb))) {
		char *seg = line;
		chomp(line);
		if (!in_body && !strncmp(line, "printf '", 8)) {
			sbuf_cut(body, 0)
			in_body = 1;
		}
		if (in_body) {
			/* the printf ends where it is redirected; its words
			 * span lines, so they are gathered raw and read as
			 * words once the whole command is in hand. The redirect
			 * is "> $P2VIF" (old single body) or "> $P2VIF.<sfx>"
			 * (new driver ".d" / sections ".0",".1",...); the suffix
			 * routes the body in pend_finish. */
			int n = strlen(seg), el = strlen(body_end);
			const char *suf = NULL;
			if (n >= el && !strcmp(seg + n - el, body_end)) {
				seg[n - el] = '\0';
				suf = "";
			} else for (int i = n - el; i >= 0; i--) {
				if (strncmp(seg + i, body_end, el) || seg[i+el] != '.')
					continue;
				suf = seg + i + el + 1;
				seg[i] = '\0';
				break;
			}
			sbuf_str(body, seg)
			if (!suf) {
				sbuf_chr(body, '\n')
				continue;
			}
			in_body = 0;
			sbuf_null(body)
			free(sufdup);
			sufdup = uc_dup(suf);
			pend_push(&pend, body->s, sufdup);
			sbuf_cut(body, 0)
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
			if ((ret = parse_vi_call(line, &blk)) < 0)
				break;
			if (!(ret = pend_finish(&pend, &blk))) {
				*blks = erealloc(*blks, (*nblks + 1)
						 * sizeof(**blks));
				(*blks)[(*nblks)++] = blk;
				memset(&blk, 0, sizeof(blk));
			} else
				free_block(&blk);
			pend_clear(&pend);
			continue;
		}
		/* the name of a leading NAME=value assignment */
		j = 0;
		while (isalnum((unsigned char)line[j]) || line[j] == '_')
			j++;
		if (j && line[j] == '=')
			ret = sh_assign(line);
		else
			ret = sh_err("command", line);
	}
	free(body->s);
	free(lb->s);
	free(sufdup);
	pend_clear(&pend);
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
 * inside the quit/interrupt register chains and so anchors nothing. */
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
	sbuf_smake(out, SB_INIT)
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

/* Post-origin baseline: each named buffer's text at the moment the origin
 * (and, for -co, the target) has been replayed but before the user edits it.
 * The compat diff is measured from here to the buffer's final state. */
typedef struct { char *path, *text; } snap_t;
typedef struct { snap_t *v; int n, cap; } snaps_t;
static snaps_t compat_base;

/* Post-origin, pre-target text of each buffer the origin opened: the tree a
 * clean apply of the origin alone leaves behind. This - not the baseline - is
 * what a gate probe must name, because every gate sensor runs before any
 * section body writes (emit_one_call's sensors-before-bodies layout), so the
 * text a gate sees at run time is the tree with the origin applied and the
 * target not yet. A probe read off the baseline could be a line the target
 * rewrote, which never exists when the sensor looks. */
static snaps_t compat_x1;

/* Replace sn with one entry per named buffer in the live session. */
static void snap_bufs(snaps_t *sn)
{
	for (int i = 0; i < sn->n; i++) {
		free(sn->v[i].path);
		free(sn->v[i].text);
	}
	sn->n = 0;
	for (int i = 0; i < xbufcur; i++) {
		if (!bufs[i].path || !bufs[i].path[0])
			continue;
		ARR_PUSH(sn->v, sn->n, sn->cap)
		sn->v[sn->n].path = uc_dup(bufs[i].path);
		sn->v[sn->n++].text = lbuf_text(bufs[i].lb);
	}
}

/* The snapshotted text of path, NULL if it was not snapshotted. */
static char *snap_find(snaps_t *sn, const char *path)
{
	for (int i = 0; i < sn->n; i++)
		if (!strcmp(sn->v[i].path, path))
			return sn->v[i].text;
	return NULL;
}

/* Give path a baseline entry holding its on-disk text, unless it already has
 * one. A path no baseline block named is untouched by the baseline, so disk is
 * its post-replay state; without the entry compat_derive() would write it off
 * as "opened after the baseline" and drop the edits made to it. */
static void snap_seed(snaps_t *sn, const char *path)
{
	char **v;
	int n, is_new;
	if (snap_find(sn, path))
		return;
	v = read_lines(path, &n, &is_new);
	sbuf_smake(sb, SB_INIT)
	for (int i = 0; i < n; i++) {
		sbuf_str(sb, v[i])
		sbuf_chr(sb, '\n')
	}
	sbuf_null(sb)
	free_lines(v, n);
	ARR_PUSH(sn->v, sn->n, sn->cap)
	sn->v[sn->n].path = uc_dup(path);
	sn->v[sn->n++].text = sb->s;
}

/* -co third argument in its unified-diff form, applied to the live buffers
 * of the handover session (its script form replays as another block). */
static int compat_apply_diff(const char *path);
static int compat_pre_script;	/* the third argument is a generated script */

/* Run every block in one session. With handover, the last block leaves
 * the editor to the user (a full vi(1), on the terminal) instead of
 * returning at the end of its body. The session's buffers are left alive
 * for the caller to read back; ed_free() drops them. snap_blk names the block
 * the compat baseline is taken after (-1 = the last one): the blocks past it
 * are a pre-applied resolution, which belongs to the derived patch and so must
 * land above the baseline, not in it. snap1_blk names the block the origin ends
 * with, snapshotted into compat_x1 for gate derivation (-1 = never). */
static int replay_blocks(p2vi_block_t *blks, int nblks, int handover,
			 int snap_blk, int snap1_blk)
{
	char **paths = NULL, *body, *ln;
	int npaths = 0, *bmap = NULL, nmap = 0, i, k, st = 0, sep, bad = 0;
	if (snap_blk < 0 || snap_blk >= nblks)
		snap_blk = nblks - 1;
	/* sized for the union of every block's files: an eviction would
	 * silently drop an edited buffer from the derived patch */
	xbufsalloc = MAX(64, xbufsalloc);
	for (i = 0; i < nblks && st == 0; i++) {
		int last = handover && i == nblks - 1;
		if ((sep = blks[i].sep) < 0) {
			st = -1;
			break;
		}
		/* In a handover session every block claims the tty, not just the
		 * last: term_init() always draws, and a non-final block left on
		 * fd 1 would leak its status line into stdout (the script). A
		 * headless pass runs handover 0 but grabs the tty itself. */
		if (ed_init(handover) < 0) {
			st = -1;
			break;
		}
		xvis |= 2;
		/* real files map to session-global buffers; a new-shape block's
		 * staged section bodies load as extra scaffolding buffers above
		 * them, and its driver references both by index (b<real> and
		 * b<secbuf>), so the map must cover the whole span. */
		int nb = blks[i].npaths + blks[i].nsects;
		if (nb > nmap) {
			nmap = nb;
			bmap = erealloc(bmap, nmap * sizeof(int));
		}
		for (k = 0; k < blks[i].npaths; k++) {
			bmap[k] = sess_buf(&paths, &npaths, blks[i].paths[k]);
			xmpt = 0;
			ec_edit("", "e", blks[i].paths[k]);
		}
		for (k = 0; k < blks[i].nsects; k++) {
			char sname[32];
			snprintf(sname, sizeof(sname), "*p2vi-sec-%d*", k);
			bmap[blks[i].npaths + k] = xbufcur;	/* appended next */
			/* Remap the section body's b<N> references to match the
			 * session's buffer indices, exactly as the driver body is
			 * remapped below: earlier blocks may have opened additional
			 * files that shift the real-file buffer numbers, and the
			 * hardcoded b<N> in the section body would point to the
			 * wrong buffer. */
			char *sec_body = uc_dup(blks[i].sects[k]);
			char *remapped;
			if (!(remapped = remap_bufnums(sec_body, sep, bmap,
							 blks[i].npaths + k + 1))) {
				free(sec_body);
				ed_done();
				st = -1;
				break;
			}
			free(sec_body);
			xmpt = 0;
			ed_loadbuf(sname, remapped);
			free(remapped);
		}
		if (st < 0)
			break;
		xmpt = 0;
		xvis &= ~4;
		body = uc_dup(blks[i].body);
		if (strip_body_tail(body, sep) < 0
		    || !(ln = remap_bufnums(body, sep, bmap, nb))) {
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
		/* Drop the section scaffolding: the driver's %ya/%@ calls have
		 * run and applied their edits to the real buffers, so the handover
		 * session and the read-back must see only the real files, exactly
		 * as the old single-body shape left them. The section buffers are
		 * the topmost xbufcur slots (loaded after the real files); switch
		 * to a real buffer first so ex_buf/ex_pbuf never dangle. */
		if (blks[i].nsects) {
			bufs_switch(bmap[0]);
			ex_pbuf = ex_tpbuf = ex_buf;
			for (k = 0; k < blks[i].nsects; k++)
				bufs_free(--xbufcur);
		}
		/* the origin (and target, for -co) has now been replayed: this is
		 * the baseline the compat diff measures from, captured before the
		 * pre-applied resolution and the user's own edits. Every path only
		 * the later blocks name is seeded from disk so its edits are not
		 * mistaken for an unrelated buffer's. */
		/* the origin alone has now run: the tree a gate probe will see */
		if (compat_capturing && !xquit && i == snap1_blk)
			snap_bufs(&compat_x1);
		if (compat_capturing && !xquit && i == snap_blk) {
			snap_bufs(&compat_base);
			for (k = i + 1; k < nblks; k++)
				for (int p = 0; p < blks[k].npaths; p++)
					snap_seed(&compat_base, blks[k].paths[p]);
			if (compat_pre && !compat_pre_script &&
					compat_apply_diff(compat_pre) < 0) {
				ed_done();
				st = -1;
				break;
			}
		}
		/* a body that quit did so on failure (its own quit tail is
		 * gone), so the user is handed nothing and the status
		 * stands */
		if (last && !xquit) {
			/* Present the baseline as a clean, saved file. The script's
			 * trailing "w" (which would have marked it saved) was stripped
			 * with the tail, so without this the handover editor shows every
			 * buffer as modified and its undo stack still holds the replay's
			 * own edits. Clear the history and the modified flag so :w/:q see
			 * no change and the user's undo reaches only their own edits. */
			for (k = 0; k < xbufcur; k++)
				lbuf_saved(bufs[k].lb, 1);
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
			for (k = 0; k < xbufcur; k++) {
				struct lbuf *lb = bufs[k].lb;
				lbuf_saved(lb, 1);
				/* exbuf_save() persists the cursor per buffer,
				 * and marks live on the lbuf, so the next block
				 * would :e each file with the previous block's
				 * row/off/top loaded and its marks still set. A
				 * non-fatal phase-1 miss then falls through to a
				 * phase-2 edit steered by that stale cursor or
				 * landing on a stale mark. Deny both: rewind the
				 * buffer to the top and drop its marks, as a
				 * freshly opened file has none. Zeroing the
				 * counts is enough (the arrays are reused). */
				bufs[k].row = bufs[k].off = bufs[k].top = 0;
				lb->mark_n = 0;
				lb->mark_sb[0] = lb->mark_se[0] = -1;
			}
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
 * session whose buffers the caller reads back: -co replays the origin and
 * then the target, so the user is handed the state both have been applied
 * to. The scripts run with their
 * own default phase policy (no env forced here): the shell header is the single
 * source of truth. Stale state is instead denied at the block boundary — every
 * buffer is rewound and its marks cleared — so a non-fatal phase-1 miss cannot
 * fall through to a phase-2 edit at a previous block's mark or cursor. */
/* Append one script's blocks to *blks. Header assignments are per script
 * (sh_reset) and each block carries its own separator, so two scripts'
 * headers never mix. */
static int parse_script(const char *path, p2vi_block_t **blks, int *nblks)
{
	FILE *f = fopen(path, "r");
	int st;
	if (!f) {
		perror(path);
		return -1;
	}
	sh_reset();
	exec_script = path;
	st = parse_p2vi_script(f, blks, nblks);
	fclose(f);
	return st;
}

/* snap_sc is the script index the compat baseline is taken after (-1 = the
 * last one) and snap1_sc the one the origin ends with (-1 = none), translated
 * into the block indices replay_blocks() wants: a script contributes as many
 * blocks as it has $VI calls. */
static int replay_scripts(const char **paths, int nscripts, int handover,
			  int snap_sc, int snap1_sc)
{
	p2vi_block_t *blks = NULL;
	int nblks = 0, st = 0, i, snap_blk = -1, snap1_blk = -1;
	for (i = 0; i < nscripts && st >= 0; i++) {
		st = parse_script(paths[i], &blks, &nblks);
		if (i == snap_sc)
			snap_blk = nblks - 1;
		if (i == snap1_sc)
			snap1_blk = nblks - 1;
	}
	if (st >= 0)
		st = replay_blocks(blks, nblks, handover, snap_blk, snap1_blk);
	free_blocks(blks, nblks);
	return st;
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
 * else: unique there (or at least present, for an ANDed pair) and absent from
 * the pre-origin text. Those two texts are the only trees the sensor can be
 * looking at - it runs before any body writes - so they are the whole test. */
static int probe_ok(char **win, int n, char **pre, int npre,
		    char **post, int npost, int uniq)
{
	int cnt = count_in(post, npost, win, n);
	if (uniq ? cnt != 1 : cnt < 1)
		return 0;
	return !count_in(pre, npre, win, n);
}

/* The shortest window of post[r->lo .. r->hi) that qualifies, growing one
 * line at a time up to GATE_MAXLINES. Returns the window length, 0 when the
 * region yields none. Nothing is excluded on account of the compat hunk's own
 * edit span: the sensor reads its probe before any body runs, so a block
 * cannot destroy the condition it is gated on. */
static int probe_from_region(gate_t *g, chg_t *r, char **pre, int npre,
			     char **post, int npost, int uniq)
{
	int len, s, i;
	for (len = 1; len <= GATE_MAXLINES && len <= r->hi - r->lo; len++) {
		for (s = r->lo; s + len <= r->hi; s++) {
			if (!probe_ok(post + s, len, pre, npre, post, npost,
				      uniq))
				continue;
			memset(g, 0, sizeof(*g));
			g->lines = emalloc(len * sizeof(char *));
			for (i = 0; i < len; i++)
				g->lines[i] = uc_dup(post[s + i]);
			g->nlines = len;
			g->polarity = GATE_PRESENT;
			return len;
		}
	}
	return 0;
}

/* Delete-only region: nothing the origin inserted is available to probe, so
 * probe a line it removed and invert the polarity - quit when the probe IS
 * found. The window must be gone from the post-origin text and unique in the
 * pre-origin one, which is the mirror of probe_ok(). */
static int probe_removed(gate_t *g, chg_t *r, char **pre, int npre,
			 char **post, int npost)
{
	int len, s, i;
	for (len = 1; len <= GATE_MAXLINES && len <= r->ndel; len++) {
		for (s = 0; s + len <= r->ndel; s++) {
			if (count_in(post, npost, r->del + s, len))
				continue;
			if (count_in(pre, npre, r->del + s, len) != 1)
				continue;
			memset(g, 0, sizeof(*g));
			g->lines = emalloc(len * sizeof(char *));
			for (i = 0; i < len; i++)
				g->lines[i] = uc_dup(r->del[s + i]);
			g->nlines = len;
			g->polarity = GATE_ABSENT;
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
		free(g[i].path);
		g[i].path = NULL;
	}
}

/* Derive the gate for one compat block over one file. pre[]/post[] are the
 * file before and after the origin's blocks ran - the two trees the sensor can
 * find at run time, since it reads its probe before any body writes. [alo,ahi)
 * is the compat hunk's anchor span, used only to order the candidate regions
 * (it is in baseline coordinates and post[] is in post-origin ones, so the
 * distance is a preference, never a constraint).
 *
 * Regions are tried nearest the anchor first, and a single unique probe wins;
 * failing that, two individually ambiguous probes from distinct regions are
 * ANDed (sequential early exits, no nesting). Returns the number of gate
 * sections, 0 when no probe validates - a hard error for the caller, never a
 * reason to ship a weak gate. */
static int derive_gates(gate_t *g, int maxg, char **pre, int npre,
			char **post, int npost, int alo, int ahi)
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
		if (probe_from_region(g, c, pre, npre, post, npost, 1))
			n = 1;
		else if (c->lo == c->hi && probe_removed(g, c, pre, npre,
							 post, npost))
			n = 1;
	}
	/* no region names the origin's landing on its own: AND two that each
	 * rule out the pre-origin text */
	if (!n && maxg > 1) {
		int got = 0;
		for (j = 0; j < nr && got < maxg; j++)
			if (probe_from_region(&g[got], &r[ord[j]], pre, npre,
					      post, npost, 0))
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

static void parse_diff_text(const char *text);
static void parse_diff_reset(void);

/* Which form the -co third argument takes: 1 = a generated patch2vi script
 * (it replays as another block), 0 = a unified diff (spliced into the live
 * buffers), -1 = unreadable. Sniffed from the first line, exactly as the
 * ordinary input is. */
static int compat_pre_isscript(const char *path)
{
	FILE *f = fopen(path, "r");
	int rc = 0;
	if (!f) {
		perror(path);
		return -1;
	}
	sbuf_smake(lb, SB_INIT)
	if (read_line(f, lb))
		rc = !strncmp(lb->s, "#!/bin/sh", 9);
	free(lb->s);
	fclose(f);
	return rc;
}

/* Where img[] sits in lines[], searching at or after from and preferring the
 * occurrence nearest hint (the diff's own coordinate, stale by construction on
 * a tree the origin and the target already changed). An empty image is a pure
 * insertion: hint itself, clamped to what is left. -1 = nowhere. */
static int find_image(char **lines, int nlines, char **img, int nimg,
		      int from, int hint)
{
	int best = -1, i, j;
	if (!nimg)
		return hint < from ? from : (hint > nlines ? nlines : hint);
	for (i = from; i + nimg <= nlines; i++) {
		for (j = 0; j < nimg && !strcmp(lines[i + j], img[j]); j++)
			;
		if (j < nimg)
			continue;
		if (best < 0 || abs(i - hint) < abs(best - hint))
			best = i;
	}
	return best;
}

/* The live session buffer holding path, opened if no replayed block named it
 * (in which case disk is its baseline text, so it is snapshotted too).
 * NULL if the editor would not open it. */
static struct lbuf *compat_openbuf(char *path)
{
	for (int i = 0; i < xbufcur; i++)
		if (bufs[i].path && !strcmp(bufs[i].path, path))
			return bufs[i].lb;
	snap_seed(&compat_base, path);
	xmpt = 0;
	ec_edit("", "e", path);
	for (int i = 0; i < xbufcur; i++)
		if (bufs[i].path && !strcmp(bufs[i].path, path))
			return bufs[i].lb;
	return NULL;
}

/* Apply one parsed file of the pre-applied diff to its session buffer. Each
 * hunk's pre-image (its context and deleted lines) is searched for rather than
 * trusted at its line number, and the buffer is rebuilt around the post-image.
 * A hunk whose pre-image is gone is a hard error: dropping it silently would
 * ship a compat patch the author did not write. */
static int compat_apply_file(file_patch_t *fp)
{
	struct lbuf *lb;
	char **lines, **out = NULL, **old = NULL, **new = NULL, *text;
	int nlines, nout = 0, ocap = 0, ncap = 0, cap = 0;
	int nold, nnew, cur = 0, i, k, lo, hi, pos, hint, st = 0;
	if (!(lb = compat_openbuf(fp->path))) {
		fprintf(stderr, "%s: cannot open %s\n", compat_pre, fp->path);
		return -1;
	}
	text = lbuf_text(lb);	/* consumed in place by split_lines() */
	lines = split_lines(text, &nlines);
	for (i = 0; i < fp->nops && st == 0;) {
		lo = fp->ops[i].hunk_lo;
		hi = fp->ops[i].hunk_hi;
		nold = nnew = 0;
		while (i < fp->nops && fp->ops[i].hunk_lo == lo &&
				fp->ops[i].hunk_hi == hi) {
			op_t *o = &fp->ops[i++];
			if (o->type != 'a')
				arr_append(&old, &nold, &ocap, o->text);
			if (o->type != 'd')
				arr_append(&new, &nnew, &ncap, o->text);
		}
		/* the hunk's first original line, 0-based; a pure insertion goes
		 * after original line lo, which is that same index */
		hint = nold ? lo - 1 : lo;
		pos = find_image(lines, nlines, old, nold, cur, hint);
		if (pos < 0) {
			fprintf(stderr, "%s: %s: hunk at line %d does not apply\n",
				compat_pre, fp->path, lo);
			st = -1;
			break;
		}
		for (k = cur; k < pos; k++)
			arr_append(&out, &nout, &cap, lines[k]);
		for (k = 0; k < nnew; k++)
			arr_append(&out, &nout, &cap, new[k]);
		cur = pos + nold;
		free_lines(old, nold);
		free_lines(new, nnew);
		old = new = NULL;
		nold = nnew = ocap = ncap = 0;
	}
	if (st == 0) {
		for (k = cur; k < nlines; k++)
			arr_append(&out, &nout, &cap, lines[k]);
		sbuf_smake(sb, SB_INIT)
		for (k = 0; k < nout; k++) {
			sbuf_str(sb, out[k])
			sbuf_chr(sb, '\n')
		}
		sbuf_null(sb)
		lbuf_edit(lb, sb->s, 0, lbuf_len(lb), 0, 0);
		free(sb->s);
	}
	free_lines(old, nold);
	free_lines(new, nnew);
	free_lines(out, nout);
	free_lines(lines, nlines);
	free(text);
	return st;
}

/* -co third argument, unified-diff form: parse it into its own files[] range
 * (and its own raw sink, so the host === PATCH === stays byte-identical) and
 * splice every hunk into the live buffers. The range is dropped right after:
 * the diff is applied, not shipped, and the emitter must not see it as a host
 * file - what the user is left holding gets re-diffed from the baseline. */
static int compat_apply_diff(const char *path)
{
	strv_t sink;
	char *text = file_text(path);
	int first = nfiles, st = 0, i;
	memset(&sink, 0, sizeof(sink));
	if (!text) {
		perror(path);
		return -1;
	}
	raw_sink = &sink;
	parse_diff_reset();
	parse_diff_text(text);
	raw_sink = NULL;
	for (i = first; i < nfiles && st == 0; i++)
		st = compat_apply_file(&files[i]);
	for (i = first; i < nfiles; i++) {
		for (int j = 0; j < files[i].nops; j++)
			free(files[i].ops[j].text);
		free(files[i].ops);
		free(files[i].path);
		free(files[i].orig_path);
	}
	nfiles = first;
	free_lines(sink.v, sink.n);
	free(text);
	return st;
}

/* The baseline lines the compat edit rewrites, in baseline coordinates:
 * common head/tail trimming leaves [*lo,*hi). A pure insertion gives lo == hi
 * at the insertion point. Anchors the gate's locality: candidate probe regions
 * are ordered by their distance from here. */
static void diff_span(char **base, int nbase, char **fin, int nfin,
		      int *lo, int *hi)
{
	int pre = 0, suf = 0;
	while (pre < nbase && pre < nfin && !strcmp(base[pre], fin[pre]))
		pre++;
	while (suf < nbase - pre && suf < nfin - pre &&
	       !strcmp(base[nbase - 1 - suf], fin[nfin - 1 - suf]))
		suf++;
	*lo = pre;
	*hi = nbase - suf;
}

/* Cross-file gate: derive the origin's probe from a file the block does not
 * itself edit, for a block whose own files yield none - either the origin never
 * touched them, or the target overwrote the origin's only trace there so pre and
 * post-origin read alike. The question a gate answers ("did this origin land on
 * this tree") is per origin, not per file, so any file the origin demonstrably
 * changed answers it. Every file the origin opened is tried in snapshot order
 * (those the block edits were tried first by the caller and failed, so retrying
 * them costs a failure each); the derived gates carry the probe's path so the
 * sensor selects that buffer. Returns the number of gate sections, 0 when no
 * file yields one. */
static int derive_gates_crossfile(gate_t *g)
{
	for (int i = 0; i < compat_x1.n; i++) {
		char **pre, **post;
		char *dup;
		int npre, npost, is_new, n;
		dup = uc_dup(compat_x1.v[i].text);
		post = split_lines(dup, &npost);
		pre = read_lines(compat_x1.v[i].path, &npre, &is_new);
		/* no anchor to be near: another file's coordinates say nothing
		 * about where this block edits, so every region ranks alike */
		n = derive_gates(g, GATE_MAXPROBES, pre, npre, post, npost, 0, 0);
		for (int j = 0; j < n; j++)
			g[j].path = uc_dup(compat_x1.v[i].path);
		free_lines(pre, npre);
		free(post);
		free(dup);
		if (n)
			return n;
	}
	return 0;
}

/* Derive the one compat block this run produces. Replays the origin (and, for
 * -co, the target) into one session, hands it to the user, then measures every
 * changed buffer from its post-origin baseline to its final state and
 * concatenates the results into a single unified diff: that diff, over however
 * many files it spans, *is* the compatibility patch, and one landing of the
 * origin gates all of it. The block is stored (not emitted); its bytes are
 * marked used so the script-global SEP/ESC, chosen after this, cover them.
 * Returns 0 on success, -1 on any hard error (a nonzero handover status, an
 * underivable gate, or a session that changed nothing), in which case main()
 * writes nothing. */
static int compat_derive(void)
{
	gate_t g[GATE_MAXPROBES];
	int i, j, k, next_id, n, nsc = 2, nchanged = 0;
	const char *sc[3] = { compat_origin, input_file, NULL };
	sbuf_smake(diff, SB_INIT)
	/* A pre-applied resolution in script form is simply one more block of the
	 * replay, run after the baseline is taken; its diff form is spliced into
	 * the buffers at that same point (compat_apply_diff). Either way the user
	 * still gets the editor on top of it. */
	if (compat_pre) {
		if ((compat_pre_script = compat_pre_isscript(compat_pre)) < 0)
			return -1;
		if (compat_pre_script)
			sc[nsc++] = compat_pre;
	}
	compat_capturing = 1;
	/* Replay origin then target into one session so the new block derives on
	 * top of every block the target already carries; existing compat blocks
	 * stack in stored order (post-only, one group). */
	if (replay_scripts(sc, nsc, 1, 1, 0) != 0) {
		ed_free();
		return -1;
	}
	compat_capturing = 0;
	n = 0;
	/* One diff over every buffer the user reshaped, in buffer order: the
	 * compat patch is that whole diff, so it lands in one block, one section
	 * and one storage region, exactly as its author would have written it. */
	for (i = 0; i < xbufcur; i++) {
		char **pre, **base, **fin, **x1 = NULL;
		char *basetext = NULL, *fintext, *bdup, *fdup, *x1dup = NULL;
		int npre, nbase, nfin, nx1 = 0, is_new, alo, ahi, xlo, xhi;
		if (!bufs[i].path || !bufs[i].path[0])
			continue;
		basetext = snap_find(&compat_base, bufs[i].path);
		if (!basetext)		/* opened after the baseline: not ours */
			continue;
		fintext = lbuf_text(bufs[i].lb);
		if (!strcmp(basetext, fintext)) {	/* user left it as it was */
			free(fintext);
			continue;
		}
		bdup = uc_dup(basetext);
		fdup = uc_dup(fintext);
		base = split_lines(bdup, &nbase);
		fin = split_lines(fdup, &nfin);
		pre = read_lines(bufs[i].path, &npre, &is_new);
		emit_unified_diff(diff, bufs[i].path, is_new, base, nbase,
				  fin, nfin);
		nchanged++;
		/* Gate: the first of these files whose own landing is visible
		 * answers for the whole block, probed near where that file is
		 * edited. The tree the sensor reads is the file with the origin
		 * applied and the target not yet; a file the origin never opened
		 * has no entry, so it has no landing of its own to probe. */
		if (!n && (x1dup = snap_find(&compat_x1, bufs[i].path))) {
			x1dup = uc_dup(x1dup);
			x1 = split_lines(x1dup, &nx1);
			diff_span(base, nbase, fin, nfin, &xlo, &xhi);
			alo = xlo;
			ahi = xhi > xlo ? xhi : xlo;
			n = derive_gates(g, GATE_MAXPROBES, pre, npre,
					 x1, nx1, alo, ahi);
			for (j = 0; j < n; j++)
				g[j].path = uc_dup(bufs[i].path);
		}
		free(fintext); free(bdup); free(fdup); free(x1dup);
		free(base); free(fin); free(x1);
		free_lines(pre, npre);
	}
	ed_free();
	sbuf_null(diff)
	if (!nchanged) {
		fprintf(stderr, "no compat patch derived\n");
		free(diff->s);
		return -1;
	}
	/* No edited file shows the origin: some other file it changed still does,
	 * since "did this origin land" is a question about the tree. */
	if (!n)
		n = derive_gates_crossfile(g);
	if (!n) {
		fprintf(stderr, "gate: %s: no probe validates, supply a gate "
			"by hand\n", compat_origin ? compat_origin : "");
		free(diff->s);
		return -1;
	}
	/* the phase-1 fallback chain's per-pattern ?? capture tags are fixed at
	 * the pattern slot + 1 (1..NSEARCH for the host, whose file-validated
	 * slots are on), and the DNF failure check ANDs every one of them; the
	 * gate's tag comes from its own band above that range, continued across
	 * blocks, so xanchor never fuses the gate's result into a group's or into
	 * another block's gate */
	next_id = next_gate_tag();
	for (j = 0; j < n; j++)
		g[j].tag = next_mark_id(&next_id);
	ARR_PUSH(compat_blocks, ncompat, compat_cap)
	compat_block_t *cb = &compat_blocks[ncompat++];
	cb->origin = uc_dup(compat_origin ? compat_origin : "");
	for (j = 0; j < n; j++)
		cb->gates[j] = g[j];	/* ownership transferred */
	cb->ngates = n;
	/* the block's diff parses into a fresh files[] range and its own raw
	 * sink, so the host === PATCH === stays byte-identical */
	raw_sink = &cb->raw;
	parse_diff_reset();
	cb->first = nfiles;
	parse_diff_text(diff->s);
	cb->count = nfiles - cb->first;
	raw_sink = NULL;
	mark_bytes_used(diff->s);
	for (j = 0; j < n; j++)
		for (k = 0; k < g[j].nlines; k++)
			mark_bytes_used(g[j].lines[k]);
	free(diff->s);
	return 0;
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

/* A parse always begins at a file header, so there is no cursor state to save
 * when switching destinations (a compat diff into its own files[] range); just
 * clear what a mid-file header would otherwise carry over. */
static void parse_diff_reset(void)
{
	diff_in_hunk = 0;
	diff_old_line = 0;
	pending_is_new = 0;
	free(pending_orig_path);
	pending_orig_path = NULL;
}

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
	const char *p, *nl;
	sbuf_smake(lb, SB_INIT)
	for (p = text; *p; p = nl + 1) {
		nl = strchr(p, '\n');
		int len = nl ? (int)(nl - p) : (int)strlen(p);
		sbuf_cut(lb, 0)
		sbuf_mem(lb, p, len)
		sbufn_chr(lb, '\n')
		add_raw(lb->s);
		sbufn_cut(lb, len)
		parse_diff_line(lb->s);
		if (!nl)
			break;
	}
	free(lb->s);
}

/*
 * Read a generated script's tail metadata in one left-to-right pass: the
 * host === DELTA === sections and every === PATCH2VI COMPAT === region
 * (its GATE probes, its own DELTA sub-sections and its === COMPAT PATCH ===
 * diff). Regions nest one deep and are fenced by === END COMPAT ===, never
 * by a line count, so a hand-edit that adds or drops a line still parses.
 * Stops at === PATCH2VI PATCH ===, leaving the host diff to the caller.
 * Returns 0, or -1 on damaged metadata.
 */
/* The DELTA sub-sections that only select where the following body lines go. */
static const struct { const char *tag; int sect; } gsects[] = {
	{ "=== custom_text ===", GS_CUSTOM },
	{ "=== pre_ctx ===", GS_PRE },
	{ "=== post_ctx ===", GS_POST },
	{ "=== strategy ===", GS_STRAT },
	{ "=== edit_cmd_abs ===", GS_ABS },
	{ "=== edit_cmd_relc ===", GS_RELC },
	{ "=== edit_cmd_rel ===", GS_REL },
};

static int read_delta_sections(FILE *in)
{
	char *line;
	int j;
	sbuf_smake(lb, SB_INIT)
	/* Skip until "exit 0" line */
	while ((line = read_line(in, lb))) {
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
	/* Compat tail-region state (single pass, depth 1). cur_cb
	 * redirects DELTA sub-sections into the block's own array,
	 * cur_gate holds an open GATE probe list, in_compat_patch
	 * routes the block's === COMPAT PATCH === diff body into its
	 * own files[] range + raw sink; all closed by === END ===,
	 * the region by === END COMPAT ===. */
	compat_block_t *cur_cb = NULL;
	gate_t *cur_gate = NULL;
	int in_compat_patch = 0;
	sbuf_smake(ph, SB_INIT)
	while (read_line(in, lb)) {
		line = chomp_sb(lb);
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
					mark_verbatim_bytes(*dst, cur_gd->ovr_esc,
							    cur_gd->ovr_sep);
				}
				sbufn_cut(ph, 0)
				in_ph = 0;
			} else {
				sbuf_str(ph, line)
				sbuf_chr(ph, '\n')
			}
			continue;
		}
		/* === COMPAT PATCH === diff body: raw diff lines
		 * (prefixed +/-/ /@@/---/+++), so a source line that
		 * looks like a section tag is harmless; only a
		 * column-0 === END === closes it. Parsed into the
		 * block's own files[] range with its own raw sink so
		 * the host === PATCH === stays byte-identical. */
		if (in_compat_patch) {
			if (strcmp(line, end_tag_rd) == 0) {
				cur_cb->count = nfiles - cur_cb->first;
				raw_sink = NULL;
				in_compat_patch = 0;
			} else {
				sbufn_chr(lb, '\n')
				add_raw(lb->s);
				sbufn_cut(lb, lb->s_n - 1)
				parse_diff_line(lb->s);
			}
			continue;
		}
		/* GATE probe lines: raw until the section's === END ===,
		 * their bytes marked so SEP/ESC avoid them. */
		if (cur_gate) {
			if (strcmp(line, end_tag_rd) == 0) {
				cur_cb->ngates++;
				cur_gate = NULL;
			} else {
				cur_gate->lines = erealloc(cur_gate->lines,
					(cur_gate->nlines + 1) * sizeof(char *));
				cur_gate->lines[cur_gate->nlines++] = uc_dup(line);
				mark_bytes_used(line);
			}
			continue;
		}
		if (strncmp(line, "=== PATCH2VI COMPAT ", 20) == 0) {
			if (cur_cb) {
				fprintf(stderr, "nested COMPAT region\n");
				return -1;
			}
			ARR_PUSH(compat_blocks, ncompat, compat_cap)
			cur_cb = &compat_blocks[ncompat++];
			/* "=== PATCH2VI COMPAT post [src=<origin>] ===" */
			char *src = strstr(line + 20, " src=");
			char *e = src ? strstr(src, " ===") : NULL;
			if (e)
				*e = '\0';
			cur_cb->origin = uc_dup(src ? src + 5 : "");
			cur_fd = NULL;
			cur_gd = NULL;
			continue;
		}
		if (cur_cb && strcmp(line, "=== END COMPAT ===") == 0) {
			cur_cb = NULL;
			cur_fd = NULL;
			cur_gd = NULL;
			continue;
		}
		if (cur_cb && strncmp(line, "=== GATE ", 9) == 0) {
			/* "=== GATE <n> <pol> tag <id> [probe <path>] ===" */
			if (cur_cb->ngates >= GATE_MAXPROBES) {
				fprintf(stderr, "too many gates\n");
				return -1;
			}
			cur_gate = &cur_cb->gates[cur_cb->ngates];
			memset(cur_gate, 0, sizeof(*cur_gate));
			char *t = strstr(line, " tag ");
			cur_gate->tag = t ? atoi(t + 5) : 0;
			cur_gate->polarity =
				strstr(line, " present ") ? GATE_PRESENT :
				strstr(line, " absent ") ? GATE_ABSENT :
				GATE_ALWAYS;
			if ((t = strstr(line, " probe "))) {
				char *e = strstr(t + 7, " ===");
				int len = e ? (int)(e - (t + 7))
					    : (int)strlen(t + 7);
				cur_gate->path = emalloc(len + 1);
				memcpy(cur_gate->path, t + 7, len);
				cur_gate->path[len] = '\0';
			}
			continue;
		}
		if (cur_cb && strcmp(line, "=== COMPAT DELTA ===") == 0) {
			cur_fd = NULL;
			cur_gd = NULL;
			continue;
		}
		if (cur_cb && strcmp(line, "=== COMPAT PATCH ===") == 0) {
			in_compat_patch = 1;
			raw_sink = &cur_cb->raw;
			parse_diff_reset();
			cur_cb->first = nfiles;
			continue;
		}
		if (strncmp(line, "=== PATCH2VI PATCH ===", 22) == 0) {
			if (cur_cb) {
				fprintf(stderr, "unterminated COMPAT region\n");
				return -1;
			}
			break;
		}
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
			/* Route a compat block's own DELTA sub-sections into
			 * its per-block store (always read, so the region
			 * round-trips); host DELTA only when re-applying. */
			dstore_t *dst = cur_cb ? &cur_cb->deltas :
					read_deltas ? &in_deltas : NULL;
			cur_fd = NULL;
			if (dst) {
				char *end = strstr(line + 10, " ===");
				if (end)
					*end = '\0';
				cur_fd = dstore_get(dst, line + 10);
			}
			continue;
		}
		if (!cur_fd)
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
			for (j = 0; j < (int)LEN(gsects); j++)
				if (!strcmp(line, gsects[j].tag))
					break;
			if (j < (int)LEN(gsects)) {
				in_sect = gsects[j].sect;
				continue;
			}
			if (strncmp(line, "=== pattern", 11) == 0) {
				pat_idx = pat_slot(line, 11);
				in_sect = GS_PAT;
				continue;
			}
			if (strncmp(line, "=== offset", 10) == 0) {
				/* "=== offset<1-NSEARCH> <%+d> ===" */
				if (cur_gd) {
					j = pat_slot(line, 10);
					cur_gd->pat_off[j] = atoi(line + 11);
					cur_gd->pat_has_off[j] = 1;
				}
				continue;
			}
			if (strncmp(line, "=== mode", 8) == 0) {
				/* "=== mode<1-NSEARCH> <%d> ===" */
				if (cur_gd) {
					j = pat_slot(line, 8);
					cur_gd->pat_mode[j] = atoi(line + 9);
					cur_gd->pat_has_mode[j] = 1;
				}
				continue;
			}
			if (strncmp(line, "=== verbatim mark ", 18) == 0) {
				if (cur_gd) {
					cur_gd->ovr_mark = atoi(line + 18);
					char *e = strstr(line + 18, " esc ");
					if (e)
						cur_gd->ovr_esc = atoi(e + 5);
					if ((e = strstr(line + 18, " sep ")))
						cur_gd->ovr_sep = atoi(e + 5);
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
	free(lb->s);
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-arih] [-d[N]] [-er TAG] [-ew TAG] [input.patch]\n"
		"       %s -e script.sh\n"
		"       %s [-ari]E [nextvi-opts...]\n"
		"       %s -co origin.sh target.sh [compat.diff|compat.sh]\n",
		prog, prog, prog, prog);
	fputs("Converts unified diff to shell script using nextvi ex commands\n"
	      "Input can be a unified diff or a previously generated patch2vi script\n"
	      "  -a    Use absolute line numbers\n"
	      "  -r    Use relative regex patterns instead of line numbers\n"
	      "  -i    Interactive mode: edit search patterns in the built-in nextvi\n"
	      "        Each group's PHASE 1/2 sections hold its verbatim ex-body\n"
	      "        bytes; editing them supersedes the structured sections for\n"
	      "        that group (latest edit wins, tie goes to verbatim)\n"
	      "  -d    Delta mode: re-apply previous customizations (-d implies -i)\n"
	      "  -d1   Delta mode: match by group index only\n"
	      "  -d2   Delta mode: match by group index + deleted/inserted text or"
	      " regex if custom\n"
	      "  -d3   Delta mode: match by group index + entire hunk\n"
	      "  -d4   Delta mode: match by deleted/inserted text or regex if custom\n"
	      "  -d5   Delta mode: match by entire hunk\n"
	      "  -e    Execute a generated script with the built-in nextvi,\n"
	      "        no shell involved (one editor per script block)\n"
	      "  -E    Edit the named files in the built-in nextvi and convert\n"
	      "        the edits into a script on stdout; no file is ever\n"
	      "        written, and files opened with :e during the session\n"
	      "        join the same script. Everything after -E is a plain\n"
	      "        nextvi command line, EXINIT included\n", stderr);
	fprintf(stderr, "  -er   Read section end tag (default: \"%s\")\n"
		"  -ew   Write section end tag (default: \"%s\")\n",
		end_tag_rd, end_tag_wr);
	fputs("  -co   Compat patch: interactively resolve a collision against\n"
	      "        origin.sh, then ship the fix as a gated block emitted\n"
	      "        AFTER the target block, on the post-origin+target tree;\n"
	      "        the block self-skips when the origin change is absent\n"
	      "        (patch2vi -co origin.sh target.sh)\n"
	      "        A third argument is an already written fix - a unified\n"
	      "        diff or a generated script - applied to that tree before\n"
	      "        the editor is handed over, so a known resolution is not\n"
	      "        retyped; it is part of the derived block, and the session\n"
	      "        is still interactive on top of it\n"
	      "  -h    Show this help\n", stderr);
	exit(1);
}

/* The argument of a two-letter option, attached (-erTAG) or separate
 * (-er TAG). Missing argument is fatal. */
static const char *opt_arg(int argc, char **argv, int *i)
{
	if (argv[*i][3])
		return argv[*i] + 3;
	if (*i + 1 < argc)
		return argv[++*i];
	fprintf(stderr, "Option -%.2s requires an argument\n", argv[*i] + 1);
	usage(argv[0]);
	return NULL;
}

int main(int argc, char **argv)
{
	int i, j;

	/* Parse arguments */
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (argv[i][1] == '-' && !argv[i][2]) {
			i++;
			break;
		}
		if (argv[i][1] == 'e' && argv[i][2] == 'r') {
			end_tag_rd = opt_arg(argc, argv, &i);
			continue;
		}
		if (argv[i][1] == 'e' && argv[i][2] == 'w') {
			end_tag_wr = opt_arg(argc, argv, &i);
			continue;
		}
		/* -co origin.sh: derive a compatibility patch against that
		 * script, applied AFTER the target (the ordinary positional
		 * input). Post-only; replaces the old -co split. */
		if (argv[i][1] == 'c' && argv[i][2] == 'o') {
			compat_mode = 1;
			read_deltas = 1;
			compat_origin = opt_arg(argc, argv, &i);
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
			else if (argv[i][j] == 'i') {
				interactive_mode = 1;
				read_deltas = 1;
			}
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
				read_deltas = 1;
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
	/* -co takes a third positional: an already written compat fix, applied
	 * before the editor is handed over */
	if (compat_mode && i + 1 < argc && !edit_mode)
		compat_pre = argv[i + 1];

	/* Mark chars that cannot be ex separators. */
	static const char *forbidden =
		" \t0123456789+-.,<>/$';%*#|" /* ex range syntax */
		"@&!?bpaefidgmqrwusxycjtohlv=" /* ex commands */
		":\"\\`\n\r";                  /* default sep, shell quote/escape/backtick, newline */
	for (const char *p = forbidden; *p; p++)
		byte_used[(unsigned char)*p] = 1;

	if (relative_mode || interactive_mode || compat_mode)
		mark_bytes_used("FAIL OK");

	/* -E: the diff is not read, it is made. Everything patch2vi's own
	 * option loop did not consume is a nextvi command line - flags after
	 * "--", then files (a missing one counts as a creation) - and the
	 * buffers that session leaves behind are diffed against their disk
	 * copies, that diff going through the parser in place of an input
	 * stream. The script itself goes to stdout, like every other mode. */
	sbuf_smake(dsb, SB_INIT)
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
	sbuf_smake(lb, SB_INIT)
	if (in && read_line(in, lb)) {
		if (!strncmp(lb->s, "#!/bin/sh", 9)) {
			if (read_delta_sections(in) < 0)
				return 1;
			check_compat_gates();
		} else {
			/* Not a script; store and process this first line */
			add_raw(lb->s);
			chomp(lb->s);
			parse_diff_line(lb->s);
		}
	}
	while (in && read_line(in, lb)) {
		add_raw(lb->s);
		chomp(lb->s);
		parse_diff_line(lb->s);
	}
	free(lb->s);

	if (in && in != stdin)
		fclose(in);

	/* -co: replay the origin script in one session and hand the
	 * tree it leaves behind to the user, who reshapes it so the target
	 * applies. Runs before the separator is picked, so the bytes of
	 * whatever the session produces are seen by find_unused_byte(). */
	if (compat_mode) {
		if (compat_derive() < 0)
			return 1;
		/* The host (target) is regenerated search-anchored too: on a tree
		 * the compat block already merged, its own hunk must fail to find
		 * its anchor and be skipped (QF1 empty), rather than an absolute
		 * edit clobbering a line by number. */
		relative_mode = 1;
	}

	/* Find separator character */
	sep = find_unused_byte();
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
	sbuf_smake(osb, SB_INIT)
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
	if (relative_mode || interactive_mode || compat_mode)
		fputs("# Env switches:\n"
		      "# Phase 1 (search/mark) reports nothing by default\n"
		      "#   DBG1=1 reports failures and which fallback anchor\n"
		      "#   resolved a group, QF1=1 also quits on failure\n"
		      "# Phase 2 (edits) reports and quits by default\n"
		      "#   DBG2=1 silences it, QF2=1 keeps going after an error\n"
		      "# INTR=1 enters vi at the failing code line in this\n"
		      "#   script, for state inspection mid execution\n\n", stdout);

	/* Build groups for every file (host and compat) */
	for (int i = 0; i < nfiles; i++)
		build_file_groups(&files[i]);

	/* Host active = files with groups outside every compat block's range;
	 * each compat block's own files are emitted as its own $VI invocation. */
	file_patch_t **active = emalloc((nfiles + 1) * sizeof(*active));
	int nactive = 0;
	for (int i = 0; i < nfiles; i++) {
		int owned = 0;
		if (files[i].ngroups == 0)
			continue;
		for (int c = 0; c < ncompat; c++)
			if (i >= compat_blocks[c].first &&
			    i < compat_blocks[c].first + compat_blocks[c].count)
				owned = 1;
		if (!owned)
			active[nactive++] = &files[i];
	}

	/* Interactive editing: one built-in editor session for all files */
	if (interactive_mode)
		interactive_edit_all_files(active, nactive);

	/* With compat blocks present, the whole patch is one $VI call: host and
	 * every compat block share one process so a gate's answer crosses the
	 * host body through global registers/anchors. Without them the common
	 * case stays a single host block, emitted byte-identically as before. */
	if (ncompat) {
		emit_one_call(active, nactive);
	} else if (nactive > 0) {
		/* A large body overflows EXINIT/argv, so the $VI invocation stages
		 * its ex command body in a temp file the shell expands. */
		fputs("# Body too large for EXINIT/argv: stage it in a file\n"
		      "( : > /tmp/p2vi.$$ ) 2>/dev/null && P2VIF=/tmp/p2vi.$$ || P2VIF=./p2vi.$$\n"
		      "trap 'rm -f \"$P2VIF\"' EXIT\n", stdout);
		fputs("\n# Patch:", stdout);
		for (int k = 0; k < nactive; k++)
			fprintf(stdout, " %s", active[k]->path);
		fputc('\n', stdout);
		emit_vi_block(active, nactive);
	}

	/* Embed delta and original patch after exit 0 */
	printf("\nexit 0\n");
	printf("=== PATCH2VI DELTA ===\n");
	emit_dstore(&out_deltas);
	emit_compat_storage();
	printf("=== PATCH2VI PATCH ===\n");
	for (int i = 0; i < nraw; i++)
		fputs(raw_lines[i], stdout);

	free(osb->s);
	free(dsb->s);
	return 0;
}
