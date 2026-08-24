/*
 * patch2vi - turn a unified diff into a /bin/sh script driving nextvi's ex
 * engine, and back.
 *
 * Usage: patch2vi [-arih] [-d[N]] [-o FILE] [-er TAG] [-ew TAG]
 *                 [input.patch] [nextvi-opts...]
 *        patch2vi -e script.sh [script2.sh...]
 *        patch2vi [-ari]I [nextvi-opts...]
 *        patch2vi [-ario]E script.sh [nextvi-opts...]
 *        patch2vi [-o]C origin.sh [-C origin2.sh...] target.sh \
 *                 [fix.diff|fix.sh|''] [nextvi-opts...]
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
 *   - turns a plain editing session into a script (-I): everything past -I
 *     is a nextvi command line - its flags, its files, EXINIT - and every
 *     buffer it leaves behind is diffed against its disk copy to produce
 *     the input the converter normally reads;
 *   - updates a script (-E): replays it, hands the tree it leaves over to
 *     the user and re-emits it through that same diff pass; naming a stored
 *     compat block's flag register rebuilds that one block instead, its own
 *     src= origins replayed ahead of the target;
 *   - replays two or more scripts (-C) to derive a compatibility patch,
 *     applied after the target behind an identity gate on the applied set
 *     ($P2VI_PATCH) - one -C per origin, and the block runs only where every
 *     origin named in its label is present; an optional second positional
 *     (diff or script, '' to skip it) pre-applies a known compat patch the
 *     session continues from, and whatever follows is a nextvi command line
 *     for the handover.
 * No session ever writes a buffer back; quitting is what emits, to stdout or
 * (-o) atomically onto a named file.
 */
#include "vi.c"

/* nextvi's own main(), renamed for this build by build_patch2vi.sh; -E
 * runs a whole editing session through it, flags, EXINIT and all */
int nextvi_main(int argc, char *argv[]);

/* Drops -o's temporary twin, once out_redirect() has made one; a no-op
 * before that. Called on the failure exits patch2vi itself notices. */
static void out_cleanup(void);

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
 * -C set only this so regen keeps host customizations without a UI. */
static int read_deltas;
/* -1=per-group stored levels, 0=off, 1-5=forced level */
static int delta_mode;
/* patch (or previously generated script) path, NULL = stdin */
static const char *input_file;
static const char *end_tag_rd = "=== END ===";
static const char *end_tag_wr = "=== END ===";

/* The emit layer builds everything in memory. Function (not macro) wrappers
 * around the sbuf appenders let call sites sit in unbraced if/else bodies. */
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

/* printf into a fresh string, so a label built from user paths has no
 * length limit. */
static char *str_fmt(const char *fmt, ...)
{
	va_list ap;
	int n;
	char *s;
	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	s = emalloc(n + 1);
	va_start(ap, fmt);
	vsnprintf(s, n + 1, fmt, ap);
	va_end(ap);
	return s;
}

/* The section terminator, and a plain body of n lines. */
static void sb_end(sbuf *fp)
{
	sb_printf(fp, "%s\n", end_tag_wr);
}

static void sb_lines(sbuf *fp, char **v, int n)
{
	for (int i = 0; i < n; i++)
		sb_printf(fp, "%s\n", v[i]);
}

/* f> anchor search strategies (SEARCH PATTERN slots), tried strict-to-loose,
 * first match wins: NPAT exact ones (default_pat_lines), then the
 * file-validated relaxed windows - fuzz (gen_fuzz_windows), the :grp-capture
 * window 7 (gen_grp_window) and the straddle windows 8 and 9 (gen_win_window,
 * skip 0 and 1). */
#define NPAT 5
#define NFUZZ 1   /* max file-validated fuzzed candidates per group (loosest kept) */
#define NGRP 1    /* file-validated :grp-capture window (TEXT.*? + last captured) */
#define NWIN 1    /* file-validated "top.*(bottom)" straddle window (pattern 8) */
#define NWIN2 1   /* second straddle window, anchors farther out (pattern 9) */
#define GRP_SLOT (NPAT + NFUZZ)         /* 0-based slot index of the grp window */
#define WIN_SLOT (NPAT + NFUZZ + NGRP)  /* 0-based slot index of the straddle window */
#define WIN2_SLOT (NPAT + NFUZZ + NGRP + NWIN)  /* 0-based slot index of the farther straddle window */
#define NSEARCH (NPAT + NFUZZ + NGRP + NWIN + NWIN2)  /* must stay <= 9: section numbers are 1 digit */

/* Cursor save/restore scratch for the global (mode 3) searches; edit marks
 * start at 1. */
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

/* THE IDENTITY GATE. A compat block (the fix for a collision with its src=
 * origins) fires iff every origin named in its label is in the applied set,
 * the basenames carried in $P2VI_PATCH: the block is gated on the identity of
 * what already ran, never on the text it left behind. A script inherits the
 * set from its caller, appends itself, and hands the remaining arguments to
 * the next script, so every level of a chain sees exactly what preceded it.
 * $P2VI_PATCH is the escape hatch for trees that did not start clean (a change
 * applied by hand, absorbed upstream, or arriving pre-patched) and is what -C
 * asserts while deriving a new block.
 *
 * The decision itself is the editor's: the set goes into a register whole and
 * every gate searches it for its own origins, so the shell contributes one
 * variable and no logic. -e and the replay path set that same variable from
 * their own accumulated set, which is why the two paths cannot drift - there
 * is only one implementation of the rule, and it is in the ex body. */

/* Set while the replay session is the compat one, so replay_blocks() snapshots
 * the post-origin baseline right before handing over to the user. */
static int compat_capturing;
/* Set while a compat block's groups are generated/emitted: exact strategies
 * only, since the file-validated generators would read the pre-origin file -
 * the wrong text for a block derived on top of it. */
static int compat_building;
static int compat_mode;			/* -C: derive a post-only compat patch */
/* -E's optional block selector: the flag register naming the one stored compat
 * block this run rebuilds. Without it -E is what it always was and the blocks
 * are discarded. */
static int amend_sel = -1;
/* The scripts it is derived against, in replay order: -C repeats, one per
 * origin, and one identity gate tests all of them at once. One origin is the
 * ordinary case and behaves exactly as the single -C always did. */
static const char **compat_origins;
static int ncompat_origin, compat_origin_cap;
/* -C second positional: an already written fix (diff or script) applied to the
 * post-origin+target tree before handover. It lands after the baseline
 * snapshot, so it is part of the derived compat patch. */
static const char *compat_pre;

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

/* Per-file delta store: the host's input (read from a script) and output
 * (captured from the editor) sets, plus one per compat block. */
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

/* Raw input lines, re-emitted as the === PATCH === tail */
static char **raw_lines;
static int nraw, raw_cap;

/* String vector; a compat block owns its === PATCH === lines this way. */
typedef struct { char **v; int n, cap; } strv_t;
static void arr_append(char ***arr, int *n, int *cap, const char *s);

/* When set, add_raw() appends here instead of raw_lines[], so a compat diff
 * lands in its block's storage and the host === PATCH === stays byte-identical. */
static strv_t *raw_sink;

/* Bytes appearing in the patch content, so SEP/ESC can avoid them */
static unsigned char byte_used[256];

/* Ex escape byte, picked from the bytes the patch does not use (0 = none left,
 * keep the default backslash paths). With it, backslash is not special to
 * ex_arg and content/regex escapes pass through unmodified. */
static int dyn_esc;

/* Ex command separator, likewise. Both go into the script as raw bytes: the
 * body is a single-quoted printf argument, so sh passes all but ' through. */
static int sep;

static void *ecalloc(size_t n, size_t sz)
{
	void *p = calloc(n, sz);
	if (!p) {
		fprintf(stderr, "out of memory\n");
		out_cleanup();
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

/* A fresh zeroed group slot in fd, carrying its 1-based index. */
static grp_delta_t *fd_add_grp(file_delta_t *fd, int idx)
{
	ARR_PUSH(fd->grps, fd->ngrps, fd->gcap)
	fd->grps[fd->ngrps].group_idx = idx;
	return &fd->grps[fd->ngrps++];
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

static void chomp(char *s)
{
	int n = strlen(s);
	while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r'))
		s[--n] = '\0';
}

/* One line of any length into sb, newline included; NULL at EOF. sb is reused,
 * so callers may modify it in place and leave the loop without freeing. */
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

/* Double backslashes for ex_arg level escaping.
 * ex_arg treats \\ as escaped \, so \\\\ is needed to preserve \\.
 * With a dynamic escape byte, backslash is not special to ex_arg and
 * passes through as-is (the escape byte never occurs in content). */
static char *escape_exarg(const char *s)
{
	return escape_chars(s, dyn_esc ? "" : "\\");
}

/* Append s through the ex_arg escape layer: content text, a user-edited regex
 * and an already sub-escaped s/// field all need exactly this and nothing more. */
static void sb_exarg(sbuf *out, const char *s)
{
	char *e = escape_exarg(s);
	sb_str(out, e);
	free(e);
}

/* Append a copy of s to a dynamic array. */
static void arr_append(char ***arr, int *n, int *cap, const char *s)
{
	if (*n >= *cap) {
		*cap = *cap ? *cap * 2 : 4;
		*arr = erealloc(*arr, *cap * sizeof(char *));
	}
	(*arr)[(*n)++] = uc_dup(s);
}

/* A NUL-terminated copy of n bytes of s (byte-exact, where uc_sub() would
 * count characters). */
static char *dup_n(const char *s, int n)
{
	char *r = emalloc(n + 1);
	memcpy(r, s, n);
	r[n] = '\0';
	return r;
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
 * File-aware anchor validation. The strict-to-loose fallback chain is emitted
 * blind and resolved by nextvi at apply time; when the pre-patch original is
 * readable (it usually is - the script applies in the same tree), candidate
 * anchors are counted against it so only proven-unique ones are added. The
 * chain is emitted either way, so the script stays portable.
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

/* count_window with substring semantics: win[j] must occur inside the aligned
 * original line (empty matches any), as pattern 7's ".*TEXT.*" window does. */
static int m_substr(const char *ln, const void *win, int j)
{
	const char *w = ((char **)win)[j];
	return !w[0] || strstr(ln, w) != NULL;
}

/* Orig lines in [from..to] containing s. Proves a pattern-8 bottom anchor
 * unambiguous: greedy ".*(bottom)" takes the last occurrence, so the chosen
 * line must be the only one carrying that text down to EOF. */
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
 * File-validated fuzzed (relaxed) anchors: an exact anchor with selected runes
 * replaced by '.', kept only if the relaxed form still resolves uniquely to the
 * right place. Length-preserving (one '.' = one rune), so it tolerates in-place
 * drift - a renamed equal-length token, a changed digit - and nothing else.
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

/* count_window_by with fuzzed-window semantics: the aligned original line must
 * match win[j]'s literal runes, its masked ones standing for anything. */
static int m_fuzzy(const char *ln, const void *win, int j)
{
	return match_fuzzy_line(ln, &((fline_t *)win)[j]);
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

/* Seeded string hash; the fuzz seed and the diff line census share it. */
static unsigned hash_str(const char *s, unsigned h)
{
	while (*s)
		h = h * 131u + (unsigned char)*s++;
	return h;
}

/* Per-position pseudo-value, content-seeded so fuzzing is reproducible. */
static unsigned hash_pos(unsigned seed, int i)
{
	unsigned h = seed ^ 0x9e3779b9u;
	h ^= (unsigned)i * 2654435761u;
	h ^= h >> 13;
	h *= 0x85ebca6bu;
	h ^= h >> 16;
	return h;
}

/* Highest fuzz level tried; level 0 is the lightest relaxation. */
#define FUZZ_MAXLVL 8
#define FUZZ_MASK_MAX 800   /* per-mille: mask up to ~80% of runes (~20% literal) */

/* Fill mask[0..nrune) for fuzz level lvl over a window-global rune index
 * starting at *gi. The drop threshold grows with lvl but never past
 * FUZZ_MASK_MAX; at least one rune always stays literal. */
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

/* Do gd's stored del/add lines match this content (or weren't recorded)? */
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

/* Does gd's stored full hunk (pre + del + add + post) match this content? */
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

/* Does a stored delta's recorded content match this hunk at strictness lvl?
 * 1 accepts anything, 2/4 compare the -/+ lines (a starred delta of that same
 * level may instead match its custom_text as one regex), 3/5 the whole hunk
 * with its context. The index check that levels 1-3 add is the caller's. */
static int delta_matches(grp_delta_t *st, char **del, int ndel,
			 char **add, int nadd, char **pre, int npre,
			 char **post, int npost, int lvl)
{
	if (lvl == 1)
		return 1;
	if (lvl == 2 || lvl == 4)
		return (st->has_star && st->level == lvl
			&& grp_content_regex_matches(st, del, ndel, add, nadd))
		       || grp_content_matches(st, del, ndel, add, nadd);
	return grp_full_hunk_matches(st, pre, npre, del, ndel, add, nadd,
				     post, npost);
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

/* Common byte prefix and suffix of two lines, each snapped back to a rune
 * boundary so runes sharing lead bytes are never split; they never overlap. */
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
	int old_diff_start = prefix;
	int old_diff_end = old_len - suffix;
	int new_diff_start = prefix;
	int new_diff_end = new_len - suffix;

	/* at least half the line common, and less than half of it changing */
	int common = prefix + suffix;
	if (common < old_len / 2 && common < new_len / 2)
		return 0;
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

	*old_text = dup_n(old + old_diff_start, old_diff_end - old_diff_start);
	*new_text = dup_n(new + new_diff_start, new_diff_end - new_diff_start);
	return 1;
}

static void mark_bytes_used(const char *s)
{
	for (; *s; s++)
		byte_used[(unsigned char)*s] = 1;
}

/* A stored verbatim blob's bytes, minus the two its script reserved: those are
 * structure, and counting them would move the next generation onto different
 * bytes and strand every blob captured under the old pair. */
static void mark_verbatim_bytes(const char *s, int esc, int sp)
{
	for (; *s; s++)
		if (*s != esc && *s != sp)
			byte_used[(unsigned char)*s] = 1;
}

/* The lowest unused byte; the low (non-printable) ones come first so printable
 * chars stay available for ex commands and patterns. */
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
	sb_str(out, "# Available separators:");
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
		sb_str(out, " (none)");
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

/* One raw byte into a shell double-quoted word: only the four bytes sh still
 * reads there need escaping. */
static void sb_dq(sbuf *out, int c)
{
	if (c == '\\' || c == '$' || c == '`' || c == '"')
		sb_chr(out, '\\');
	sb_chr(out, c);
}

/* A command body as a single-quoted printf argument. A quote is the one byte
 * such a word cannot hold, so each is closed, escaped and reopened; everything
 * else, control bytes and the separator included, goes out verbatim. */
static void sq_write(const char *s, int n)
{
	for (int i = 0; i < n; i++) {
		if (s[i] == '\'')
			fputs("'\\''", stdout);
		else
			putchar((unsigned char)s[i]);
	}
}

/* A file path as a whole single-quoted shell word: an argument of the $VI
 * line, quoted by the same rule sq_write applies inside a printf argument.
 * A path is data, not a word the shell parses, so nothing else in it needs
 * escaping - single quotes are only special where this quoting opens. */
static void sq_path(const char *s)
{
	putchar('\'');
	for (; *s; s++) {
		if (*s == '\'')
			fputs("'\\''", stdout);
		else
			putchar((unsigned char)*s);
	}
	putchar('\'');
}

/* the body register, and the EXINIT that yanks the body buffer into it
 * and runs it; -e fills the register itself and needs neither */
#define P2VI_REG 97
#define P2VI_VICALL "EXINIT='%ya 97:? %@97'"

/*
 * Script state lives in ex registers, not shell variables. A shell variable is
 * a flat string spliced into a site whose nesting depth the shell cannot know;
 * a register is escaped once where it is defined and "?%@<id>" runs it verbatim
 * at any depth (ex_arg sbuf_mem's the expansion, never rescanning it).
 *
 * Definedness is the switch: an undefined register expands to nothing and "?"
 * with an empty argument runs nothing, so the shell only ever contributes whole
 * commands ("${DBG1:+213reg ...}") that define or clear one.
 *
 * Expansion is turned on ("2sc %") for a call and off again ("2sc!") at once,
 * since with xexp live every % in an argument would expand and arguments carry
 * file-derived regexes; argument registers are therefore written before the
 * window, never inside it. xexe (!) stays 0 throughout - a stray ! in a live
 * argument would fork a shell.
 */
#define REG_QF1  210	/* phase-1 quit chain, set by QF1=1 */
#define REG_QF2  211	/* phase-2 quit chain, emptied through REG_QF2A by QF2=1 */
#define REG_INTR 212	/* interrupt chain, set by INTR=1 */
#define REG_ERR1 213	/* phase-1 FAIL chain, set by DBG1=1 */
#define REG_ERR2 214	/* phase-2 FAIL chain, cleared by DBG2=1 */
#define REG_OK1  215	/* phase-1 OK chain, set by DBG1=1 */
#define REG_OK2  216	/* phase-2 OK chain, cleared by DBG2=1 */
#define REG_HDLR 217	/* the FAIL report chain both phases share */
#define REG_LOC  219	/* argument: the FAIL location of the current site */
#define REG_MSG  220	/* argument: the OK report command of the current site */
/* The FAIL log: the report chain redirects its print here ("prF") instead of to
 * the terminal alone, so every failure of a QF2=1 run accumulates in one
 * register the run can be read back from. An upper-case id is the point - that
 * is the range ex_cprint newline-terminates each append in, which makes the
 * register a line per failure and so parseable (see FAILURE PLACEMENT). */
#define REG_FLOG 'F'
/* The "vis 2; q!1" body every assert runs through, and the one register QF2=1
 * clears. REG_QF2 only ever points at it ("? %@221"), so the -C blocks that
 * rewrite REG_QF2 at run time - the host override and each block's quit policy -
 * rewrite which assert applies, never whether asserting is enabled at all: with
 * QF2=1 in the environment 221 is empty and every one of those rewrites lands
 * on a body that reports and falls through. */
#define REG_QF2A 221
/*
 * -C registers. Registers are global to the process and never cleared between
 * chains, so a value a block's call writes crosses the host body:
 *   REG_APPLIED       the applied set: $P2VI_PATCH verbatim, padded with a
 *                     space at each end, the one thing the shell contributes.
 *                     The driver decides every identity gate out of it with
 *                     "fr <REG_APPLIED>" and a search, so the decision is the
 *                     editor's and the -e path reaches it by setting the same
 *                     variable.
 *   REG_FLAG_ANY      "any origin present", read once by the host override to
 *                     relax its quit chain.
 *   REG_FLAG_BASE+k   one per compat block k, = "every src= of that block is
 *                     in the applied set", read by the block's gated call and
 *                     its quit policy.
 * All three sit above the 210-220 control band. The anchor slots use ec_while
 * ids >= 10, above every single-digit group chain tag, so they never fuse; two
 * blocks reusing the same slots is fine, since each records them immediately
 * before reading them and a lookup takes the last record.
 */
#define REG_APPLIED   229	/* the applied set, i.e. $P2VI_PATCH */
#define REG_FLAG_ANY  230	/* shared any-origin-fired register */
#define REG_FLAG_BASE 231	/* per-compat-block flag registers: base+k */
#define FLAG_SLOT_BASE 10	/* ec_while subset-test anchor slots (>= 10) */
#define SRC_SLOT_BASE  20	/* ec_while src= membership anchor slots */

static void emit_esc_sep(sbuf *out, int n)
{
	while (n-- > 0)
		sb_chr(out, dyn_esc ? dyn_esc : '\\');
	sb_chr(out, sep);
}

/* the same in the one part of a body the shell writes: a double-quoted word,
 * where a raw escape or separator byte may need escaping again */
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
/* Whether the buffer already ends in a line break ("0?" and its newline)
 * followed by nothing but separator/escape decoration. Cuts that decoration
 * back, so the next thing appended supplies its own separator. */
static int lb_pending(sbuf *out)
{
	int esc = dyn_esc ? dyn_esc : '\\';
	int n = out->s_n;
	while (n > 0 && (out->s[n - 1] == sep || out->s[n - 1] == esc))
		n--;
	if (n < 3 || out->s[n - 1] != '\n' || out->s[n - 2] != '?' ||
			out->s[n - 3] != '0')
		return 0;
	/* a break is always emitted after a separator or at the start of a
	 * segment, which is what tells it apart from body text ending in "0?" */
	if (n > 3 && out->s[n - 4] != sep && out->s[n - 4] != esc &&
			out->s[n - 4] != '\n')
		return 0;
	out->s_n = n;
	return 1;
}

/* The no-op command that lets a long chain break across source lines.
 * Callers emit a separator before and after it, and the clause they meant to
 * put between two breaks is sometimes empty (an error check that reports
 * nothing, say), which would leave a bare second break behind. */
static void emit_lb(sbuf *out)
{
	if (!lb_pending(out))
		sb_str(out, "0?\n");
}
#define EMIT_LB(out) emit_lb(out)

/* Append a generated (or overridden) group segment. Segments start with their
 * own line break, redundant when the segment before ended with one. */
static void sb_seg(sbuf *out, const char *seg)
{
	if (!strncmp(seg, "0?\n", 3) && lb_pending(out))
		seg += 3;
	sb_str(out, seg);
}

/*
 * The ex commands emitted here, and what they do to xrow (every one defaults to
 * the current line when given no range, per ex_region()):
 *   i / c / d      advance xrow past what they inserted or deleted; "0i"
 *                  inserts above line 1
 *   f> / f+ / f-   xrow = matched line, xoff = match position
 *   bare address   xrow = end - 1; this is how +N / -N move without a command
 *   s / p          leave xrow and xoff alone
 *   m              sets a line mark ("+2m 0" marks cursor+2 as <0>); the mark
 *                  auto-adjusts in lbuf_replace() as edits above it shift lines
 *   'N             addresses that mark ("'0c" edits the marked line)
 *   vis, w, q!, sc!, ??!   setup, writes, quits, specials, conditionals
 */

/* The content lines of a c/i command. */
static void emit_content(sbuf *out, char **texts, int ntexts)
{
	for (int i = 0; i < ntexts; i++) {
		sb_exarg(out, texts[i]);
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
		sb_str(out, "i ");
	else if (line <= 0)
		sb_str(out, "0i ");
	else
		sb_printf(out, "%di ", line);
	emit_content(out, texts, ntexts);
	EMIT_SEP(out);
}

/* Delete lines N to M inclusive. */
static void emit_delete(sbuf *out, int from, int to)
{
	if (from == to)
		sb_printf(out, "%dd", from);
	else
		sb_printf(out, "%d,%dd", from, to);
	EMIT_SEP(out);
}

/* The ";A[;B]c/d" tail of a character-level edit, after the caller's address
 * prefix. An empty replacement over a non-empty span deletes it. */
static void emit_horiz_span(sbuf *out, int start, int end, const char *new_text)
{
	if (!*new_text && start != end) {
		sb_printf(out, ";%d;%dd", start, end);
	} else {
		if (start == end)
			sb_printf(out, ";%dc ", start);
		else
			sb_printf(out, ";%d;%dc ", start, end);
		sb_exarg(out, new_text);
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

/* Change lines N to M (an empty replacement deletes them). */
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
 * Phase 1 (resolve). The whole buffer is yanked once into the find register
 * (fr 98) right after the file opens, so every group's search runs against a
 * cache that stays byte-identical to the buffer - nothing is edited in this
 * phase. Each group records its target line in a line mark ("+<off>m <id>") and
 * gets up to NSEARCH fallback patterns, strict to loose, first match wins
 * (emit_fallback_chain). ABS-strategy groups mark their original line number
 * as-is: the buffer is still pristine, so no line-delta correction is needed.
 *
 * Phase 2 (commit). Edits address the marks ('0c, '0d, '0,#+Nc, '0s/../../,
 * '0;A;Bc ...), which auto-adjust as edits above them shift lines, so groups
 * apply forward in patch order. Since every search ran before the first edit, a
 * failed anchor aborts with the file untouched.
 *
 * Every search and every phase-2 edit is followed by a ??! check that reports
 * and quits before corrupting the file.
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

/* The expansion window calling chain register <reg>: % on, run the chain, %
 * off. Its argument register must already be written, while % was inert. */
static void emit_reg_call(sbuf *out, int reg, int deep)
{
	sb_str(out, "2sc %");
	emit_sep_lvl(out, deep);
	sb_printf(out, "? %%@%d", reg);
	emit_sep_lvl(out, deep);
	sb_str(out, "2sc!");
}

/* Emit the ??! error check after a command that may fail, with the FAIL
 * location it reports through the shared report chain in REG_LOC: phase 1
 * (search) reports <path>:<line>, phase 2 (edit at a mark) adds :m<id>, and
 * mark_id < 0 means no mark (new-file insert, custom abs command).
 * phase selects the report register, whose definedness is the DBG<n> switch
 * and whose chain ends in the phase's INTR and QF<n> calls.
 * ids[0..nids) are the capture tags of a fallback chain - every pattern
 * variant in phase 1, every substitute rung in phase 2 - ORed into one DNF
 * expression prefixing the conditional, so it branches on those recorded
 * statuses instead of the last command's and the inverted branch fires only
 * if all of them failed. */
static void emit_err_check(sbuf *out, int phase, int line, int mark_id,
			   const int *ids, int nids)
{
	sbuf_smake(loc, SB_INIT)
	sb_printf(loc, "%s:%d", cur_file_path ? cur_file_path :
		  phase == 1 ? "?" : "", line);
	if (phase == 2) {
		sb_str(loc, ":m");
		if (mark_id >= 0)
			sb_printf(loc, "%d", mark_id);
	}
	sbuf_nul(loc)
	for (int t = 0; t < nids; t++)
		sb_printf(out, t ? ";%d" : "%d", ids[t]);
	/* "?" "?!" split: "??!" in one literal is the trigraph for '|' */
	sb_printf(out, "?" "?!%dreg %s", REG_LOC, loc->s);
	EMIT_ESCSEP(out);
	emit_reg_call(out, phase == 1 ? REG_ERR1 : REG_ERR2, 0);
	EMIT_SEP(out);
	free(loc->s);
}


/* Everything a search of the given mode needs before its f> argument, plus the
 * verb itself (see SEARCH MODES). lvl is the caller's separator nesting depth,
 * first selects f> over f+; a global mode-3 window always forces f>, since it
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

/* The lone-pattern phase-1 search: setup and f>, the anchors, the error check,
 * then "+<offset>m <mark_id>" to mark the target without moving the cursor.
 * pre_escaped 0 = raw text (regex+exarg escape), 1 = regex (exarg only). */
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
		sb_chr(out, '$');
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

/* Mark ids reserved per file by verbatim overrides (their blobs reference a
 * fixed id), so regenerated groups cannot collide with them. next_mark_id also
 * skips the ids the editor rewrites itself: <'> <*> <[> <]> <`>. */
static int *reserved_marks;
static int nreserved_marks, reserved_marks_cap;

static void reserve_mark(int id)
{
	ARR_PUSH(reserved_marks, nreserved_marks, reserved_marks_cap)
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

static int delta_matches_group(grp_delta_t *st, group_t *g, int lvl)
{
	return delta_matches(st, g->del_texts, g->ndel, g->add_texts, g->nadd,
			     g->all_pre_ctx, g->nall_pre_ctx,
			     g->post_ctx, g->npost_ctx, lvl);
}

/* The store's delta for group idx of this file, or NULL. Levels 1-3 test the
 * group at the stored index, 4-5 any group whose content matches. */
static grp_delta_t *find_grp_delta(file_delta_t *fd, int idx, group_t *g)
{
	for (int i = 0; fd && i < fd->ngrps; i++) {
		grp_delta_t *gd = &fd->grps[i];
		int lvl = delta_mode > 0 ? delta_mode
			  : gd->level ? gd->level : 2;   /* old format: 2 */
		if (lvl < 4 && gd->group_idx != idx)
			continue;
		if (delta_matches_group(gd, g, lvl))
			return gd;
	}
	return NULL;
}

/* Any text a search could anchor on - leading context, a following context
 * line, a non-empty deleted line? Decides REL vs ABS. */
static int group_has_anchors(group_t *g)
{
	return g->nanchors >= 2
	       || (g->nanchors == 1 && g->anchors[0] && g->anchors[0][0])
	       || (g->follow_ctx && g->follow_ctx[0])
	       || (g->ndel > 0 && g->del_texts[0] && g->del_texts[0][0]);
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

/* Default (non-edited) lines for fallback pattern pi, strict to loose:
 *   0 = whole hunk (pre-ctx + deleted + following ctx)
 *   1 = deleted + following ctx: pre-context ambiguous, trailing not
 *   2 = deleted lines only
 *   3 = top context only (the historical single pattern)
 *   4 = following ctx only: the whole hunk region is volatile but the line
 *       after it is a stable landmark
 * The deletion-rooted slots (1, 2) outrank the context-rooted ones (3, 4): they
 * land on the text the patch expects to remove (off = 0), while a context match
 * trusts that nothing drifted between the anchor and the hunk.
 * 1 and 4 need deleted lines and return 0 for a pure add; redundant slots (no
 * following context makes 1 == 2, 4 empty) are dropped by the caller's dedup.
 * raw[] takes borrowed pointers; *off = lines from match start to target. */
static int default_pat_lines(group_t *g, int pi, char **raw, int *off)
{
	int n = 0;
	int has_del = g->ndel > 0 && !(g->ndel == 1 && !g->del_texts[0][0]);
	int has_post = g->npost_ctx > 0 || (g->follow_ctx && g->follow_ctx[0]);
	*off = 0;
	if (pi == 2) {
		if (!has_del)
			return 0;
		for (int i = 0; i < g->ndel; i++)
			raw[n++] = g->del_texts[i];
		return n;
	}
	if (pi == 1) {
		/* deleted lines + following ctx; match starts on the first
		 * deleted line, which is the target (off = 0). Only distinct
		 * from strategy 2 when following context exists. */
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
	if (pi == 3) {
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

/* A file-validated relaxed window: pre-escaped regex lines plus the offset and
 * mode needed to emit it like an exact pattern. */
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
 * Up to max fuzzed windows for group g into out[]: the whole-hunk window
 * relaxed at increasing levels, keeping each variant the original file proves
 * still resolves to exactly one place - the right one. Needs orig_lines loaded
 * and the hunk pristine. Caller owns out[i].lines.
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
		/* keep ~20% of runes literal: a window relaxed past four in
		 * five is too thin to trust, however it validates here */
		int too_loose = total > 0 && masked * 5 > total * 4;
		int first, cnt = any && !too_loose
				 ? count_window_by(win, bn, &first, m_fuzzy) : 0;
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
 * context plus the first deleted line on change/delete - becomes "TEXT.*?" line
 * by line, the final line captured "(TEXT)". ":grp 1" lands on that captured
 * line, and the non-greedy ".*?" on the leading ones absorbs text added after
 * an anchor without shifting the target. Unanchored, so no leading ".*".
 *
 * The captured last line IS the target at offset 0: change/delete captures the
 * first deleted line, a pure insert the last anchor ("'Ni" appends after it).
 * Voided when degenerate - under two lines (a bare "(text)" duplicates the
 * exact single-line strategies) or an empty capture (zero-width "()" resolves
 * anywhere) - and file-validated like the fuzzed windows.
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
	cnt = count_window_by(raw, n, &first, m_substr);
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

/* How far above/below a hunk gen_win_window looks for a unique anchor block;
 * bounds its O(scan * file) validation cost. */
#define WIN_SCAN 200

/* Lines per straddle anchor block: a 3-line block is far more discriminating
 * than a single line, so the global search false-matches less. */
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
 * OUTSIDE the diff's shown region - the nearest unique WIN_ANCHOR-line block
 * above the enclosing @@ hunk and below it - so these lines exist only in the
 * original and this needs it readable. Regex "t1\nt2\nt3.*(b1)\nb2\nb3": each
 * block newline-joined (consecutive-line match), one greedy ".*" between them
 * absorbing the whole hunk. Only b1 is captured, so ":grp 1" lands on it and the
 * target is a negative offset back up. Emitted bracketed by mark-0 save / "1;0"
 * reset / "'0" restore, so the search runs from the file top.
 *
 * skip=1 is pattern 9: skip the first qualifying block on each side, advancing a
 * WHOLE block so the two windows stay disjoint - a wider, looser straddle last
 * in the chain.
 *
 * File-validated: top block unique; bottom block unique AND its captured first
 * line carries its text nowhere down to EOF, so the greedy ".*" lands on
 * exactly it.
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
	sbuf_nul4(sb)
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

/* One fallback pattern as the f> argument inside a ? conditional, which
 * consumes one more ex_arg escape layer than a top-level search - so with the
 * default backslash escape every backslash is doubled again, and with a dynamic
 * one nothing extra is needed. */
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
		sb_chr(out, '$');
	/* Ensure trailing newline when last line is empty */
	if (p->nlines > 0 && !p->lines[p->nlines - 1][0])
		sb_chr(out, '\n');
}

/* Phase 1 fallback chain: every pattern nested into one ? conditional, chained
 * with escaped separators, first match wins. Per pattern n (capture tag n):
 *   %f> <pat>\:<n>??\:<n>??[+off]m <id>\\\:${OK1}p OK <loc>:a<n>\\\:1q\:
 * (the ${OK1} report only on fallback blocks, n >= 1). The search's status is
 * captured into tag <n>; on success that branch marks the target and 1q
 * short-circuits out. After the last block a single <0;1;..>??! DNF check over
 * all tags reports the failure.
 * Each attempt carries its mode's setup and the teardown it implies: mode 1
 * restores the register cache ("fr 98") on both paths, modes 2 and 3 reset the
 * search group, mode 3 restores the saved cursor before the conditional 1q. */
static void emit_fallback_chain(sbuf *out, pat_spec_t *ps, int nps,
				int mark_id, int target_line, int first)
{
	int pids[NSEARCH];
	sb_chr(out, '?');
	for (int n = 0; n < nps; n++) {
		int m1 = ps[n].mode == 1;
		int g3 = ps[n].mode == 3;
		int g2 = ps[n].mode == 2 || g3;   /* grp bracketing covers both */
		/* Readability line break, so every attempt starts on its own
		 * source line: separator, no-op clause, newline, then the
		 * separator before the search setup. */
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
		/* The same once the result is captured into tag <n>, splitting
		 * the match from its mark action. After the capture (and before
		 * grp 0 / the action re-test), so it never separates a tag test
		 * from its then-arm. */
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
		/* fallback (non-primary) match: with DBG1=1 the OK chain is
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
				/* the unconditional restore split the then-arg,
				 * so re-test the tag to keep 1q on success */
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

/* ex_re_read halves a run of escapes sitting against the closing delimiter
 * (ceil(n/2), commit d94cd92). Our escapers already doubled each literal
 * backslash, so double the trailing run once more for ex_re_read to halve back.
 * Only the run adjacent to the delimiter is affected - an interior escape is
 * followed by an ordinary char and passes through unchanged. */
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

/* s/// replacement text: only \ (backreferences) and the delimiter are
 * special. The _raw escapers omit double_trailing_esc so segments can be
 * concatenated with raw regex before the fixup is applied once to the whole. */
static char *escape_sub_repl_raw(const char *s, char delim)
{
	char set[3] = { '\\', delim, 0 };
	return escape_chars(s, set);
}

/* escape_regex plus the delimiter, for ex_re_read. */
static char *escape_sub_pat_raw(const char *s, char delim)
{
	char set[sizeof(REGEX_META) + 1];
	snprintf(set, sizeof(set), "%s%c", REGEX_META, delim);
	return escape_chars(s, set);
}

/* Longest common substring of a and b: its byte length, with ai/bi set to the
 * start offsets. Plain O(alen*blen) DP; diff lines are short. */
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
	ARR_PUSH(bv->v, bv->n, bv->cap)
	bv->v[bv->n].oa = oa;
	bv->v[bv->n].na = na;
	bv->v[bv->n++].len = len;
}

/* Decompose om/nm into in-order common blocks, difflib-style: longest common
 * substring, then recurse on the flanks. Blocks are rune-trimmed; the trimmed
 * edges and the gaps fall through as changed text. */
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
 * The MINIMAL head and tail (in runes) of a stable run that each occur exactly
 * once in the whole old line, so "(head.*tail)" matches it deterministically -
 * minimal anchors maximize the wildcarded interior. Needs non-overlapping
 * head/tail leaving at least one rune of middle; 0 = caller falls back to a
 * literal capture.
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
 * Grp-capture absorbing substitute (rung 1 of the progression). The changed
 * line is decomposed into stable common runs and the edits between them, and
 * the pattern covers the EXACT SPAN ONLY - first edit to last edit. The stable
 * runs outside it are dropped: the substitute matches as an unanchored
 * substring, so wrapping the unchanged prefix/suffix in "(.*)" would only
 * duplicate the exact rung.
 *
 * Each in-span run becomes a capture group, each edit is matched literally and
 * re-emitted. A run is wildcarded so it absorbs drift inside itself:
 *   - full "(.*)" when flanked by non-empty edits whose old-texts are each
 *     UNIQUE in the old line, so the literal separators pin the greedy
 *     boundaries: "X bbbb Y" -> "P bbbb Q" gives s/X(.*)Y/P\1Q/.
 *   - else "(head.*tail)" over the minimal unique anchors (fuzz_anchors), for
 *     when a separator is an insertion or repeats and "(.*)" would be ambiguous
 *   - else a literal "(text)" capture.
 * Returns 0 unless at least one run is wildcarded: otherwise the variant
 * reproduces the span verbatim and is a pure dup of the exact rung.
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
		/* Fold small INTERIOR runs into the surrounding edit, but keep the
		 * boundary ones regardless of size: they become "(.*)" absorbers, and
		 * folding one would strand an insertion against a wildcard and force
		 * the whole variant to be rejected. */
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
	sbuf_nul4(pat)
	sbuf_nul4(repl)
	*pat_out = double_trailing_esc(pat->s);
	*repl_out = double_trailing_esc(repl->s);
	return 1;
}


/* One progression rung, from pre-escaped pat/repl. */
static void emit_substitute_grp(sbuf *out, const char *pat, const char *repl)
{
	sb_str(out, "s/");
	sb_exarg(out, pat);
	sb_chr(out, '/');
	sb_exarg(out, repl);
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
 * Phase 2 substitute progression, mirroring emit_fallback_chain: each variant
 * (exact -> grp-absorbing) at the mark, first success wins. The s/// is both
 * test and action - a failed match leaves the line untouched, so the next,
 * looser variant is safe to try, and the first success short-circuits with 1q
 * so no later one re-edits the now changed line. A non-primary success reports
 * via ${OK2}; if every variant fails the trailing DNF check reports FAIL. A
 * single-variant chain degrades to a plain addressed s/// + check.
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

/* The substitute progression for a single-line change: rung 0 the minimal-span
 * exact s/old/new/, rung 1 the grp-absorbing variant over that same span
 * (skipped when it would absorb nothing). Fields are fully escaped, as
 * interactive mode displays them; caller frees. */
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

/* Phase 2: substitute at a mark, through the exact -> grp-absorbing chain. */
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

/* Strip a +N/-N prefix off a custom rel/relc command and return it: a bare
 * "+N" line is removed outright, a "+3a text" one loses just the prefix. */
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

/* Is this SEARCH PATTERN line only a +N/-N offset override? Real pattern lines
 * starting with + are regex-escaped, so a bare signed number is unambiguous. */
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

/* grp_delta_t doubles as the per-group editor-buffer parse result (that path
 * just leaves group_idx/pre_ctx/post_ctx unset). Free every array it owns. */
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
 * parsers: the two formats carry the same fields under different header
 * spellings, and both append through gsect_add. CONTENT stays per-parser. */
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

/* The slot digit after a "=== <tag>" prefix of n bytes, 0-based; a legacy tag
 * with no digit means the top-context slot, SEARCH PATTERN 4. */
static int pat_slot(const char *line, int n)
{
	char c = line[n];
	return (c >= '1' && c <= '0' + NSEARCH) ? c - '1' : 3;
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

/* One line of an open PHASE blob, which is byte-verbatim: only the end tag ends
 * it, every other line is content. which (1/2) picks the blob it lands in, gd
 * may be NULL where the header named no group the reader knows. Returns the
 * capture state to keep: which while it stays open, 0 once the tag closed it
 * and the accumulated bytes - minus the one newline the display pass appended -
 * are stored. */
static int ph_capture(sbuf *ph, const char *line, grp_delta_t *gd, int which)
{
	char **dst;
	if (strcmp(line, end_tag_rd) != 0) {
		sbuf_str(ph, line)
		sbuf_chr(ph, '\n')
		return which;
	}
	if (ph->s_n > 0 && ph->s[ph->s_n - 1] == '\n')
		ph->s_n--;
	sbuf_nul(ph)
	if (gd) {
		dst = which == 1 ? &gd->ph1 : &gd->ph2;
		free(*dst);
		*dst = uc_dup(ph->s);
	}
	sbufn_cut(ph, 0)
	return 0;
}

/*
 * Parse a multi-file interactive editor blob, mutated in place (so parse a blob
 * only once). "=== FILE: <path> ===" routes the groups after it to
 * per_file_results[k]. Content is stored raw, with no parse_ecmd_offset
 * stripping, so the baseline and the edited buffer compare like for like.
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

		if (in_ph) {
			grp_delta_t *pg = file_idx >= 0 && gi >= 0 &&
					  gi < active[file_idx]->ngroups
					  ? &per_file_results[file_idx][gi] : NULL;
			in_ph = ph_capture(ph, line, pg, in_ph);
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
			 * pattern), now SEARCH PATTERN 4. */
			const char *p = line + 18;
			while (*p == ' ')
				p++;
			in_pat = (*p >= '1' && *p <= '0' + NSEARCH) ? *p - '0' : 4;
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

/* One "=== <name> ===" section; an empty array writes nothing at all, so an
 * untouched field leaves no section behind. */
static void emit_delta_sect(sbuf *out, const char *name, char **v, int n)
{
	if (n <= 0)
		return;
	sb_printf(out, "=== %s ===\n", name);
	sb_lines(out, v, n);
	sb_end(out);
}

/* One group's delta, in the structured storage format. */
static void emit_grp_delta(sbuf *out, grp_delta_t *gd)
{
	char name[32];
	sb_printf(out, "=== GROUP %d ===\n", gd->group_idx);
	for (int i = 0; i < gd->ndel_lines; i++)
		sb_printf(out, "-%s\n", gd->del_lines[i]);
	for (int i = 0; i < gd->nadd_lines; i++)
		sb_printf(out, "+%s\n", gd->add_lines[i]);
	sb_end(out);
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
 * verb takes the first added line, the rest follow below. Every call site is
 * an adding shape, so nadd > 0. */
static void wg_content(sbuf *fp, group_t *g)
{
	sb_chr(fp, ' ');
	for (int k = 0; k < g->nadd; k++) {
		sb_str(fp, g->add_texts[k]);
		sb_chr(fp, '\n');
	}
}

/* One file-validated window SEARCH PATTERN (fuzz/grp/straddle slots): a
 * recorded delta wins, else the freshly generated window w. */
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
		sb_lines(fp, gd->pattern[slot], gd->npattern[slot]);
	else
		sb_lines(fp, w->lines, w->nlines);
	sb_end(fp);
}

/* The three EDIT COMMAND sections. A recorded delta wins over the generated
 * default; abs is always offered, relc only for a single-line change with a
 * usable ;c span, rel whenever the group has something to anchor on. */
static void wg_edit_cmds(sbuf *fp, group_t *g, grp_delta_t *gd, int is_new,
			 int has_anchors, int add_a)
{
	int show_relc = has_anchors && g->ndel == 1 && g->nadd == 1
			&& g->has_line_diff;
	sb_str(fp, "=== EDIT COMMAND (abs) ===\n");
	if (gd && gd->nabs > 0) {
		sb_lines(fp, gd->abs_cmd, gd->nabs);
	} else if (g->del_start && g->nadd) {
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
	sb_end(fp);

	if (show_relc || (gd && gd->nrelc > 0)) {
		sb_str(fp, "=== EDIT COMMAND (relc) ===\n");
		if (gd && gd->nrelc > 0)
			sb_lines(fp, gd->relc_cmd, gd->nrelc);
		else if (g->ldc_start == g->ldc_end)
			sb_printf(fp, ".;%dc %s\n", g->ldc_start, g->ldc_new_text);
		else
			sb_printf(fp, ".;%d;%dc %s\n", g->ldc_start, g->ldc_end,
				  g->ldc_new_text);
		sb_end(fp);
	}

	if (!has_anchors && !(gd && gd->nrel > 0))
		return;
	sb_str(fp, "=== EDIT COMMAND (rel) ===\n");
	if (gd && gd->nrel > 0) {
		sb_lines(fp, gd->rel_cmd, gd->nrel);
	} else if (show_relc) {
		/* the substitute progression, one s/// per rung (exact ->
		 * grp-absorbing); >1 rung becomes a first-wins chain at emit */
		subvar_t v[2];
		int nv = build_sub_variants(g, v);
		for (int k = 0; k < nv; k++) {
			sb_printf(fp, "s/%s/%s/\n", v[k].pat, v[k].repl);
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
	sb_end(fp);
}

/*
 * Every group to fp, optionally injecting stored delta from in_fd. with_phase
 * adds the MARK header field and the per-group PHASE 1/PHASE 2 sections holding
 * the verbatim segment bytes (override, else generated), and so requires
 * gen_group_segments to have run.
 */
static void write_groups_to_file(sbuf *fp, group_t *groups, int ngroups,
				 file_delta_t *in_fd, int is_new,
				 const char *orig_path, int with_phase)
{
	/* the pre-patch original, to validate the relaxed windows against */
	if (orig_path && !is_new)
		load_orig_file(orig_path);
	for (int gi = 0; gi < ngroups; gi++) {
		group_t *g = &groups[gi];
		if (!g->del_start && !g->nadd)
			continue;
		int target = g->del_start ? g->del_start : g->add_after;

		grp_delta_t *gd = find_grp_delta(in_fd, gi + 1, g);

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

		/* Group header; MARK names the phase-1 mark id the PHASE
		 * blobs reference (edit it when renumbering marks in them) */
		if (with_phase && g->mark_id >= 0)
			sb_printf(fp, "=== GROUP %d/%d (line %d) MARK %d ===\n",
				  gi + 1, ngroups, target, g->mark_id);
		else
			sb_printf(fp, "=== GROUP %d/%d (line %d) ===\n",
				  gi + 1, ngroups, target);
		/* gd is non-NULL only when in_fd is: find_grp_delta bails on NULL */
		if (gd && gd->ncustom_text > 0 && gd->has_star) {
			sb_lines(fp, gd->custom_text, gd->ncustom_text);
		} else {
			for (int i = 0; i < g->ndel; i++)
				sb_printf(fp, "-%s\n", g->del_texts[i]);
			for (int i = 0; i < g->nadd; i++)
				sb_printf(fp, "+%s\n", g->add_texts[i]);
		}
		sb_end(fp);
		int lvl = (gd && gd->level) ? gd->level : 2;
		sb_printf(fp, "=== LEVEL %d%s ===\n", lvl, gd && gd->has_star ? "*" : "");

		/* COMMAND STRATEGY: inject stored strategy or keep all commented */
		int sel_strat = (gd && gd->strategy != STRAT_DEFAULT)
				? gd->strategy : STRAT_DEFAULT;
		sb_str(fp, "=== COMMAND STRATEGY ===\n");
		sb_printf(fp, "%sabs\n", sel_strat == STRAT_ABS ? "" : "#");
		if (has_anchors && g->ndel == 1 && g->nadd == 1 && g->has_line_diff)
			sb_printf(fp, "%srelc\n", sel_strat == STRAT_RELC ? "" : "#");
		if (has_anchors)
			sb_printf(fp, "%srel\n", sel_strat == STRAT_REL ? "" : "#");

		/* SEARCH PATTERN 1-NPAT, first match wins (see
		 * default_pat_lines). Single-line patterns show their ^...$
		 * anchors so the user can remove them; emit respects that. */
		sb_end(fp);
		/* Pure adds position on the line to append after, so the shown
		 * offsets include the -1 step the append-after "i" implies. */
		int pure_add = !g->del_start && g->nadd;
		int add_a = pure_add && default_offset - 1 >= 0;
		char **praw = emalloc((g->ndel + 7) * sizeof(char *));
		for (int pi = 0; pi < NPAT; pi++) {
			sb_printf(fp, "=== SEARCH PATTERN %d ===\n", pi + 1);
			int doff;
			int n = default_pat_lines(g, pi, praw, &doff);
			/* OFFSET: lines from match start to the edit target;
			 * MODE: the search form (see SEARCH MODES). */
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
			sb_end(fp);
		}
		free(praw);

		/* The relaxed slots, generated fresh from the original; a
		 * recorded delta wins so user tweaks round-trip. */
		winset_t ws;
		gen_extra_windows(g, &ws);
		for (int pi = NPAT; pi < NSEARCH; pi++) {
			int i = pi - NPAT;
			/* recorded-delta mode default: the generator's own */
			int def_mode = pi == GRP_SLOT ? 2 : pi >= WIN_SLOT ? 3
				       : (gd && gd->npattern[pi] == 1);
			emit_win_section(fp, gd, pi, &ws.w[i], ws.has[i], def_mode);
		}
		free_extra_windows(&ws);

		wg_edit_cmds(fp, g, gd, is_new, has_anchors, add_a);

		/* PHASE 1/2: the group's verbatim ex-body bytes (override
		 * wins). Only the end tag terminates them, and editing them
		 * supersedes every structured section above. */
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

/* The editor-side blob of a whole unit: one "=== FILE: <path> ===" section per
 * file, each holding that file's groups. Written three times per session - the
 * injection blob, the no-injection baseline and the displayed text - which
 * differ only in the store they inject from and whether the PHASE blobs are
 * shown, so every one of them is spelled here. */
static void write_files_blob(sbuf *out, file_patch_t **fps, int n,
			     file_delta_t **fd_per, int with_phase)
{
	for (int k = 0; k < n; k++) {
		sb_printf(out, "=== FILE: %s ===\n", fps[k]->path);
		write_groups_to_file(out, fps[k]->groups, fps[k]->ngroups,
				     fd_per ? fd_per[k] : NULL, fps[k]->is_new,
				     fps[k]->orig_path ? fps[k]->orig_path
				     : fps[k]->path, with_phase);
		sb_printf(out, "%s\n\n", end_tag_wr);
	}
	sbuf_nul(out)
}

static void gen_group_segments(file_patch_t *fp);

/* Drop every custom_* override so apply_grp_edits starts clean: an emptied
 * editor section then reverts to defaults instead of keeping the last pass. */
static void free_cmd(char ***v, int *n)
{
	free_lines(*v, *n);
	*v = NULL;
	*n = 0;
}

static void clear_group_customs(group_t *g)
{
	for (int pi = 0; pi < NSEARCH; pi++) {
		free_cmd(&g->custom_pat[pi], &g->ncustom_pat[pi]);
		g->custom_pat_has_off[pi] = g->custom_pat_off[pi] = 0;
		g->custom_pat_has_mode[pi] = g->custom_pat_mode[pi] = 0;
	}
	free_cmd(&g->custom_abs_lines, &g->custom_abs_nlines);
	free_cmd(&g->custom_relc_lines, &g->custom_relc_nlines);
	free_cmd(&g->custom_rel_lines, &g->custom_rel_nlines);
	g->custom_offset = 0;
}

/* Move a parsed section's array into a group's override slot, emptying the
 * source. Nothing moves when the section was left untouched. */
static void steal_lines(char ***dv, int *dn, char ***sv, int *sn, int *scap)
{
	if (*sn <= 0)
		return;
	*dv = *sv;
	*dn = *sn;
	*sv = NULL;
	*sn = *scap = 0;
}

/* Move parsed sections eg into g's custom_* overrides. Runs twice per session -
 * pre-editor to bake stored deltas into the PHASE baselines, post-editor with
 * the user's own edits - so the previous pass is cleared first. */
static void apply_grp_edits(group_t *g, grp_delta_t *eg)
{
	clear_group_customs(g);
	g->strategy = eg->strategy;
	for (int pi = 0; pi < NSEARCH; pi++) {
		/* the OFFSET marker wins over a legacy +N first line */
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
		steal_lines(&g->custom_pat[pi], &g->ncustom_pat[pi],
			    &eg->pattern[pi], &eg->npattern[pi], &eg->pat_cap[pi]);
	}
	steal_lines(&g->custom_abs_lines, &g->custom_abs_nlines,
		    &eg->abs_cmd, &eg->nabs, &eg->abs_cap);
	/* file order (relc before rel), so the last-written
	 * custom_offset comes from the later section */
	if (eg->nrelc > 0)
		g->custom_offset = parse_ecmd_offset(eg->relc_cmd, &eg->nrelc);
	steal_lines(&g->custom_relc_lines, &g->custom_relc_nlines,
		    &eg->relc_cmd, &eg->nrelc, &eg->relc_cap);
	if (eg->nrel > 0)
		g->custom_offset = parse_ecmd_offset(eg->rel_cmd, &eg->nrel);
	steal_lines(&g->custom_rel_lines, &g->custom_rel_nlines,
		    &eg->rel_cmd, &eg->nrel, &eg->rel_cap);
}

/* A structured edit supersedes a stale verbatim override: drop it, keeping the
 * blobs and the group's identity in the .rej file to re-derive a fix from. */
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
		sb_end(sb);
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

/* Editor bring-up, hoisted from nextvi's main()/ex_init(): no argv, no EXINIT,
 * for the sessions that edit buffers patch2vi built rather than files (-E goes
 * through nextvi_main() instead). Split into init/teardown so one process can
 * run several independent editor lifetimes - the interactive session and, one
 * per script block, the -e runner. The config tables and the input buffer are
 * process-wide and built once; the rest is per session, freed by ed_free().
 * With use_tty the editor takes the controlling terminal on fds 0/1, since
 * patch2vi's own stdin/stdout may be the patch and the generated script. */
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

/* Everything that is not a buffer: temp buffers, registers, the "??" tags, the
 * last search and the globals a body may have changed. Dropped between blocks
 * even where the buffers persist (replay), so nothing is inherited. */
static void ed_free_session(void)
{
	int i;
	/* a run that dies before ed_setup() (a script that will not even parse)
	 * still unwinds through here, and so does a second free: both leave the
	 * temp buffers zeroed, which lbuf_free() does not take */
	for (i = 0; i < (int)LEN(tempbufs); i++) {
		free(tempbufs[i].path);
		if (tempbufs[i].lb)
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
	/* ignorecase defaults on in nextvi and every pattern here is literal
	 * source text (see emit_prologue), so a replay/-i session must turn it
	 * off too, whether or not a replayed prologue already did. */
	xic = 0;
}

/* The state above plus the buffers, their marks and their undo history: a block
 * started after this sees exactly what a freshly spawned editor sees. */
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

/* Load one in-memory buffer under a label; nothing here touches the
 * filesystem. */
static void ed_loadbuf(const char *name, char *text)
{
	char msg[512];
	bufs_switch(bufs_open(name, strlen(name)));
	lbuf_edit(xb, text, 0, 0, 0, 0);
	ex_bufpostfix(ex_buf, 1);
	snprintf(msg, sizeof(msg), "\"%s\" %dL [f]", xb_path, lbuf_len(xb));
	/* "-m" silences the load line, as ec_edit's own is */
	if (!(xvis & 4))
		ex_print(msg, bar_ft)
}

/* Hand the loaded buffers to the user and end the session; they outlive it so
 * the caller can read them back, and ed_free() drops them. */
/* The nextvi command line that follows a mode's own arguments: -E's after
 * its script, -C's after its fix slot, -i/-d's after the input patch. Its
 * option letters are vi(1)'s own, applied to the interactive session, and
 * its files are opened on top of the ones the run itself named - so a
 * session can visit a file the script never touched and still have it end
 * up in the emitted diff. */
static int hand_vis = -1;	/* xvis for the session, -1 = plain visual */
static char **hand_files;
static int nhand_files;

/* The one interactive session every editing path ends in. Undo what the
 * replay or the loader left behind (the body's "|sc!" separator, escape
 * and error mode), open the command line's files, park on the first placed
 * failure (fbuf < 0 when there is none), fire the P2VI_EX harness hook and
 * enter the loop nextvi_main() would have - the parsed flags decide, not a
 * hardcoded vi(): "-e" and "-s" mean ex(), "-a" wraps the session in the
 * scroll history. Startup cannot be redone here (term_init already ran
 * without bit 1, and the buffers are loaded, so ex_init gets no argv),
 * which is why this mirrors main()'s tail instead of calling any of its. */
static void ed_serve(int fbuf, int frow)
{
	char *ln;
	int k;

	xvis = hand_vis >= 0 ? hand_vis : 0;
	xsep = ':';
	xesc = '\\';
	xerr = 1;
	for (k = 0; k < nhand_files; k++)
		ec_edit("", "e", hand_files[k]);
	if (fbuf >= 0 && fbuf < xbufcur) {
		bufs_switch(fbuf);
		xrow = frow;
		xoff = 0;
	}
	syn_setft(xb_ft);
	if ((ln = getenv("P2VI_EX")))	/* test harness hook */
		ex_command(ln)
	/* like ex_init(): never enter vi() with xmpt > 1, or it opens with
	 * the "[any key to continue]" pager (each load print bumps xmpt) */
	if (xmpt > 1)
		xmpt = 1;
	if (!xquit) {
		if (xvis & 8)
			term_scrh()
		if (xvis & 2)
			ex();
		else
			vi(1);
		if (xvis & 8)
			term_scrl()
	}
}

static int ed_run(void)
{
	int st;
	ed_serve(-1, -1);
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

/* One derived (or re-read) compatibility block: one whole compat patch, i.e.
 * one unified diff over however many files it touches. One block = one section
 * = one staged body = one storage region = one -i buffer, so a compat patch is
 * authored and shipped as the single diff it is. Always emitted after the host;
 * origin is per-block, since the global only describes the current run. */
typedef struct {
	char *origin;		/* src= label; its basenames are the identity gate */
	int first, count;	/* files[] range this block owns */
	strv_t raw;		/* the block's own === PATCH === lines */
	/* per-block delta customizations, filled either from the editor
	 * (out) or re-read from a stored block (in) */
	dstore_t deltas;
} compat_block_t;
static compat_block_t *compat_blocks;
static int ncompat, compat_cap;

/* basename of a path (see below); forward-declared for the -e applied set */
static const char *base_name(const char *p);
/* the src= fields of a compat block's origin label, as basenames */
static int compat_src_fields(compat_block_t *cb, char ***out);

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
 * One built-in nextvi session over nu named buffers (host unit first, then one
 * per compat block in application order) plus an optional trailing .rej buffer,
 * reading every unit's buffer back into out[i]. Buffer i is unit i by
 * bufs_open()'s append order. -1 on error, out[] then undefined.
 */
static int edit_units(struct edit_ub *u, int nu,
		      const char *rejname, char *rejtext, char **out)
{
	int need = nu + (rejtext ? 1 : 0);
	/* Keep the whole union resident: ed_init() commits xbufsmax from
	 * xbufsalloc and bufs_open() evicts the top slot once full, so an
	 * undersized cap would silently drop a unit's buffer. Sized above nu,
	 * eviction only ever reaches buffers the user opened past the union. */
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
	/* A shrunk count means a unit is gone (:bd, or an eviction reached
	 * into the union) and its edits would be read from the wrong buffer.
	 * The names are NOT re-checked: :w legitimately renames a path. */
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
 * Inject a unit's stored per-file deltas into fps[]->groups, so a later emit (or
 * the interactive PHASE baselines) carries the customizations: build a
 * delta-injected structured blob, parse it right back onto the groups, then
 * attach the stored verbatim PHASE overrides. ds is the unit's store (host
 * in_deltas, or a compat block's own); files match their delta by path.
 */
static void inject_deltas(file_patch_t **fps, int n, dstore_t *ds)
{
	file_delta_t **fd_per = dstore_per_file(ds, fps, n);

	sbuf_smake(tmp_sb, SB_INIT)
	write_files_blob(tmp_sb, fps, n, fd_per, 0);

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
			grp_delta_t *gd = find_grp_delta(fd_per[k], gi + 1, g);
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
 * One unit's structured delta: compare its no-injection baseline (orig_blob)
 * against the buffer the user edited, store the changed groups into out, then
 * apply the edits onto fps[]->groups so the later emit sees them. ins is
 * consulted to preserve a group-locator custom_text a structured-only edit left
 * alone. Per-unit variance is only the stores and the compat window.
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

			/* PHASE edits are detected against the displayed bytes
			 * (override, else generated), so a structured-only edit
			 * leaves them alone and they regenerate from it next
			 * session. Latest edit wins; a tie goes to verbatim. */
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
			grp_delta_t *gout = fd_add_grp(od, gi + 1);
			gout->level = eg->level ? eg->level : 2;
			gout->has_star = eg->has_star;
			/* original del/add always from patch */
			arr_clone(&gout->del_lines, &gout->ndel_lines,
				  &gout->del_cap, g->del_texts, g->ndel);
			arr_clone(&gout->add_lines, &gout->nadd_lines,
				  &gout->add_cap, g->add_texts, g->nadd);
			arr_clone(&gout->pre_ctx, &gout->npre_ctx,
				  &gout->pre_cap, g->all_pre_ctx, g->nall_pre_ctx);
			arr_clone(&gout->post_ctx, &gout->npost_ctx,
				  &gout->post_cap, g->post_ctx, g->npost_ctx);
			/* kept even under a verbatim override: custom_text is
			 * also the group-locator regex for starred LEVEL 2/4,
			 * so dropping it degrades re-entry to index-only */
			if (custom_ch) {
				arr_clone(&gout->custom_text, &gout->ncustom_text, &gout->custom_text_cap,
					  eg->custom_text, eg->ncustom_text);
			} else if (fd_per[k]) {
				/* preserve existing customization from stored delta */
				grp_delta_t *stored = find_grp_delta(fd_per[k], gi + 1, g);
				if (stored && stored->ncustom_text > 0) {
					arr_clone(&gout->custom_text, &gout->ncustom_text, &gout->custom_text_cap,
						  stored->custom_text, stored->ncustom_text);
				}
			}
			/* A verbatim override supersedes the structured
			 * customizations: store both blobs, so the group is
			 * frozen as one unit, with its mark and escape regime. */
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

/* One edit unit: its file range, its inject/preserve store and its
 * derived-delta store. Unit 0 is the host, 1..N the compat blocks in order. */
typedef struct {
	file_patch_t **fps;
	int n;
	dstore_t *ins;		/* inject/preserve store (in_deltas or cb) */
	dstore_t *out;		/* derived-delta store (out_deltas or cb) */
	int compat;		/* 1 = compat block: hold the compat window */
	char *name;		/* buffer label (unique across units), owned */
	char *orig;		/* no-injection baseline blob (owned) */
	char *phased;		/* editor text: injected + MARK/PHASE (owned) */
} unit_t;

/* Enter/leave the compat emission window: relative anchoring and the
 * file-validated generators off (they would read the pre-origin file). Held
 * around a block's blob build, its derivation and its body emission alike. */
static void compat_win_enter(int *sv)
{
	*sv = relative_mode;
	relative_mode = 1;
	compat_building = 1;
}

static void compat_win_leave(int sv)
{
	compat_building = 0;
	relative_mode = sv;
}

/* A unit's two blobs: the no-injection baseline (for the later diff) and the
 * injected + MARK/PHASE text shown in the editor. */
static void build_unit_blobs(unit_t *u)
{
	int sv = 0;
	if (u->compat)
		compat_win_enter(&sv);

	file_delta_t **fd_per = dstore_per_file(u->ins, u->fps, u->n);

	sbuf_smake(orig, SB_INIT)
	write_files_blob(orig, u->fps, u->n, NULL, 0);
	u->orig = orig->s;

	inject_deltas(u->fps, u->n, u->ins);
	for (int k = 0; k < u->n; k++)
		gen_group_segments(u->fps[k]);

	sbuf_smake(ph, SB_INIT)
	write_files_blob(ph, u->fps, u->n, fd_per, 1);
	u->phased = ph->s;

	free(fd_per);
	if (u->compat)
		compat_win_leave(sv);
}


/* Will this stored delta find a group to re-apply to? One that finds none is
 * rejected: dumped to the .rej buffer and stripped of its star. */
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
 * Interactive editing of the host groups and every compat block, one in-RAM
 * nextvi buffer per unit. Pattern lines are shown regex-escaped, as the regex
 * engine will see them, and an untouched buffer parses back to what was written
 * into it, so an unedited unit reproduces its input. Host edits land in
 * out_deltas, compat edits in each block's own cb->deltas.
 */
static void interactive_edit_all_files(file_patch_t **active, int nactive)
{
	unit_t *units = ecalloc(1 + ncompat, sizeof(*units));
	int nu = 0;
	sbuf *rej = NULL;
	/* Buffer labels, not files: reference the original input (the patch,
	 * or the previously generated script under -d) when it has a name;
	 * .diff/.rej pick up nextvi's diff highlighting. */
	const char *base = input_file ? input_file : "patch2vi";
	char *rejname = str_fmt("%s.p2v.rej", base);

	/* --- Host unit (buffer 0): its reject pass mutates in_deltas before
	 * injection, so it runs here, not in build_unit_blobs. --- */
	file_delta_t **in_fd_per = dstore_per_file(delta_mode ? &in_deltas : NULL,
						   active, nactive);
	if (nactive > 0) {
		/* before the editor buffer is built, so a rejected delta can
		 * lose its star and not inject its custom_text */
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

		/* -i without -d rejects every stored delta wholesale: nothing
		 * is injected or preserved, and they are dumped to the .rej
		 * buffer to re-apply by hand, mirroring -d's reject flow. */
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
			sbuf_nul(rej)

		unit_t *hu = &units[nu++];
		hu->fps = active;
		hu->n = nactive;
		hu->ins = delta_mode ? &in_deltas : NULL;
		hu->out = &out_deltas;
		hu->compat = 0;
		hu->name = str_fmt("%s.p2v.diff", base);
		build_unit_blobs(hu);
	}

	/* Compat units: one buffer per block, in application order. Their
	 * baseline is their own stored deltas - there is no fresh diff to fall
	 * back to - so -i keeps them and an untouched buffer re-derives them. */
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
		/* Index prefix keeps two blocks over one file from colliding on
		 * bufs_find (ex.c matches on the name). The origin label's
		 * repeated " src=" fields join on '+' here, so the whitespace
		 * the storage header separates them with stays out of a name
		 * the user types at :b; one origin is unchanged. */
		const char *osrc = cb->origin ? cb->origin : "", *oend;
		sbuf_smake(nb, SB_INIT)
		while ((oend = strstr(osrc, " src="))) {
			sbuf_mem(nb, osrc, oend - osrc)
			sbuf_chr(nb, '+')
			osrc = oend + 5;
		}
		sbuf_str(nb, osrc)
		sbuf_nul(nb)
		cu->name = str_fmt("%d.%s.compat-post.p2v.diff", c, nb->s);
		free(nb->s);
		build_unit_blobs(cu);
		nu++;
	}

	if (nu == 0) {
		free(units);
		free(ca_store);
		free(in_fd_per);
		free(rejname);
		return;
	}

	/* The index prefix guarantees unique names, but a mis-map would route
	 * one unit's edits into another, so verify rather than trust. */
	for (int i = 0; i < nu; i++)
		for (int j = i + 1; j < nu; j++)
			if (strcmp(units[i].name, units[j].name) == 0) {
				fprintf(stderr, "patch2vi: duplicate editor "
					"buffer name \"%s\"\n", units[i].name);
				out_cleanup();
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

	/* Each unit's delta from its own buffer. A compat unit clears its store
	 * first - a snapshot keeps the pre-edit deltas for the custom_text
	 * lookup - and holds the compat window, since apply_grp_edits
	 * regenerates segments. */
	for (int i = 0; i < nu; i++) {
		unit_t *u = &units[i];
		dstore_t *ins = u->ins;
		dstore_t snap = {0};
		int sv = 0;
		if (u->compat) {
			compat_win_enter(&sv);
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
	for (int i = 0; i < nu; i++)
		free(units[i].name);
	free(rejname);
	free(units);
	free(ubs);
	free(outs);
}

/* A custom EDIT COMMAND: lines[0] is "cmd [first-content]", the rest are
 * further content lines. An s/// goes out through the exarg layer only, a
 * bare command (d, ,#+Nd) verbatim. */
static void emit_custom_edit_lines(sbuf *out, char **lines, int nlines)
{
	if (nlines == 0)
		return;
	const char *first = lines[0];
	if (is_substitute(first)) {
		sb_exarg(out, first);
		return;
	}
	/* Find first space: split command prefix from inline content */
	const char *sp = strchr(first, ' ');
	if (sp) {
		int horiz = memchr(first, ';', sp - first) != NULL;
		sb_mem(out, first, sp - first);  /* command prefix verbatim */
		sb_chr(out, ' ');
		sb_exarg(out, sp + 1);     /* first content line escaped */
		if (!horiz)
			sb_chr(out, '\n');
		for (int k = 1; k < nlines; k++) {
			sb_exarg(out, lines[k]);
			sb_chr(out, '\n');
		}
	} else {
		/* No content (d, ,#+Nd, etc.) */
		sb_str(out, first);
	}
}

/*
 * groups[] for a file, from its ops[]: a group is a contiguous run of
 * deletes/adds with optional context anchors.
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

		/* Up to 3 following context lines, for the patterns that
		 * key on them (default_pat_lines 0/1/4). Without them
		 * relative mode would fall back to the single follow_ctx
		 * line and drift from the interactive output. */
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
		/* interactive mode also shows all leading context */
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
				/* ;c positions are rune indexes, and a split rune
				 * would shift them and splice invalid UTF-8 */
				int prefix, suffix;
				common_affix(old, new, &prefix, &suffix);
				g->ldc_start = rune_count_n(old, prefix);
				g->ldc_end = rune_count_n(old, olen - suffix);
				g->ldc_new_text = dup_n(new + prefix,
							nlen - suffix - prefix);
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

/* A custom EDIT COMMAND at the group's mark. A custom_offset (the +N/-N pulled
 * off the verb) rides the mark address as "'N+off", so an insert-above-line-1
 * survives the round-trip instead of being lost against the patterns'
 * explicit OFFSETs. */
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

/* Every group's verbatim phase-1/phase-2 segment bytes into ph1_gen/ph2_gen
 * (forward layout only), regenerated from the current structured state on every
 * call. A group carrying an override still generates normally and its bytes are
 * discarded, so the cross-group state - mark allocation, first-search flag -
 * evolves exactly as it did when the override was captured. */
static void gen_group_segments(file_patch_t *fp)
{
	group_t *groups = fp->groups;
	int ngroups = fp->ngroups;

	cur_file_path = fp->path;

	/* the pre-patch original, for anchor validation: the "---" path
	 * names it, and the edit target holds the same content until the
	 * script runs */
	if (!fp->is_new && !compat_building)
		load_orig_file(fp->orig_path ? fp->orig_path : fp->path);

	/* drop stale segments and reserve the override marks first */
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

	/* Phase 1 (resolve): every group's search against the register cache,
	 * recording its target line in a mark. Edit marks start at 1, mark 0
	 * being the global searches' cursor scratch. */
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

		/* non-interactively the flags decide, interactively the user */
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

		/* The fallback list: edited SEARCH PATTERN sections if any,
		 * else the defaults. Duplicates dropped. */
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
			/* mirrors default_pat_lines: the deletion-rooted
			 * slots start on the target line, post-only sits
			 * ndel below it, the rest anchor on leading context */
			ps[nps].offset = g->custom_pat_has_off[pi]
					 ? g->custom_pat_off[pi]
					 : (pi == 1 || pi == 2) ? 0
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
			/* the relaxed windows, loosest last; off_final on the
			 * last three keeps their offsets through the pure-add
			 * shift. Interactive mode surfaces them as custom_pat. */
			gen_extra_windows(g, &ws);
			for (int pi = NPAT; pi < NSEARCH && nps < NSEARCH; pi++)
				if (ws.has[pi - NPAT])
					nps = push_win_pat(ps, nps, &ws.w[pi - NPAT],
							   pi + 1, pi >= GRP_SLOT);
			/* No re-sort: the slots are already strict to loose, and
			 * -i emits in this same order, so sorting only -r would
			 * diverge the two modes. */
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

		/* Pure insert: the mark lands on the line to append after.
		 * Custom edit lines carry their own verb-relative offset, so
		 * they take no adjustment. */
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
		sbuf_nul(out)
		g->ph1_gen = out->s;
	}

	/* Phase 2 (commit): the edits at their marks, forward order; the marks
	 * auto-adjust as edits above them shift lines. */

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
			 * editable progression: rebuild it as a first-wins
			 * chain. Anything else emits verbatim at the mark. */
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
		sbuf_nul(out)
		g->ph2_gen = out->s;
	}
	free_orig_file();
}

/* One file's groups as ex commands: absolute mode bottom-to-top (line numbers
 * stay valid, no searches, no marks), the forward modes every phase-1 segment
 * and then every phase-2 one. The groups are freed here. */
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
			sb_seg(out, seg);
	}
	for (int gi = 0; gi < ngroups; gi++) {
		group_t *g = &groups[gi];
		const char *seg = g->ph2_ovr ? g->ph2_ovr : g->ph2_gen;
		if (seg)
			sb_seg(out, seg);
	}
	for (int gi = 0; gi < ngroups; gi++)
		free_group(&groups[gi]);
}

/* The state registers, defined at the top of every $VI body: the body writes
 * the default state and the shell then contributes whole commands that flip
 * individual switches (any non-empty value counts as set).
 * REG_HDLR prints the FAIL line with the print redirected into REG_FLOG, so the
 * line reaches the terminal and the log both, and then calls INTR; a phase's
 * FAIL chain calls REG_HDLR and then its quit chain, so QF only fires where the
 * report does. The redirect is switched off again right after ("pr" with no
 * argument toggles), leaving the log the only state the site keeps. */
static void emit_qf2_assert(sbuf *out);

static void emit_reg_defaults(sbuf *out)
{
	sb_printf(out, "%dreg pr%c", REG_HDLR, REG_FLOG);
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
	sb_printf(out, "%dreg vis 2", REG_QF2A);
	EMIT_ESCSEP(out);
	sb_str(out, "q!1");
	EMIT_SEP(out);
	/* the default assert, the same one a -C block restores */
	emit_qf2_assert(out);
}

/* The switches, as whole commands the shell either contributes or not - the
 * only part of a body sh writes, and a double-quoted word, so its raw separator
 * bytes are escaped for it. The backslash-newline breaks are continuations
 * inside the quotes, so they only split the word for readability.
 *
 * applied adds the applied set to that word: ex can read no environment but
 * EXINIT, so $P2VI_PATCH has to be written in from outside, and this word is
 * already the place the shell writes. The value lands in REG_APPLIED with a
 * space at each end - two spaces after "reg", since ex_cmd eats one - so the
 * identity gates emit_compat_flags builds delimit a name with plain spaces. It
 * goes out ahead of the body, which is the third printf argument, so the
 * register is set before anything reads it. */
static void emit_reg_switches(sbuf *out, int applied)
{
	if (applied) {
		sb_printf(out, "%dreg  $P2VI_PATCH ", REG_APPLIED);
		sb_dq_esc_sep(out, 0);
		sb_str(out, "\\\n");
	}
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
	sb_printf(out, "${QF2:+ya!%d", REG_QF2A);
	sb_dq_esc_sep(out, 0);
	sb_str(out, "}\\\n");
	/* the failing site is where the script wrote the location
	 * register, so INTR searches itself for that argument */
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
 * real files (b0..bN-1, the order the call opened them in) and quit. skip, when
 * given, marks buffers a section body writes itself (a compat-only file, which
 * must not be created when its identity gate misses) - the tail skips them. */
static void emit_write_tail(sbuf *out, int nbufs, const char *skip)
{
	sb_str(out, "vis 2");
	EMIT_SEP(out);
	for (int i = 0; i < nbufs; i++) {
		if (skip && skip[i])
			continue;
		sb_printf(out, "b%d", i);
		EMIT_SEP(out);
		sb_str(out, "w");
		EMIT_SEP(out);
	}
	sb_str(out, "2q");
}

/* The specials prologue every body opens with: "|sc! <esc><sep>|" declares the
 * escape and separator bytes to ex (with the default backslash escape the loc
 * halves a doubled one), "vis 3" enters raw ex mode, "ic 0" makes every pattern
 * case-sensitive.
 *
 * That last one is a correctness requirement, not a preference: nextvi's
 * ignorecase defaults ON and feeds every rset this script relies on, while the
 * patterns here are literal source text. A case-folded match is simply the
 * wrong line or the wrong byte - "s/x/w/" meant for "xrows" lands on the "X" of
 * an earlier "MAX(" - and the shorter the pattern, the likelier the impostor. */
static void emit_prologue(sbuf *out)
{
	sb_str(out, "|sc! ");
	sb_chr(out, dyn_esc ? dyn_esc : '\\');
	if (!dyn_esc)
		sb_chr(out, '\\');
	sb_chr(out, sep);
	sb_str(out, "|:vis 3");
	EMIT_SEP(out);
	sb_str(out, "ic 0");
	EMIT_SEP(out);
}

/* Open the "printf '%s%s%s\n' ..." that stages a body, writing its first two
 * arguments: the prologue plus the default register state (single quoted, so
 * every byte goes out verbatim), and the switches the shell contributes (a
 * double-quoted word). The printf is left open on its third argument, the body
 * proper. regs = 0 omits both halves, as a plain absolute script has no state
 * to switch; applied adds the $P2VI_PATCH register write, which only a driver
 * with compat blocks reads. osb is scratch, left empty. */
static void emit_body_head(sbuf *osb, int regs, int applied)
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
		emit_reg_switches(osb, applied);
	sbuf_nul(osb)
	fputs(osb->s, stdout);
	fputs("\"\\\n'", stdout);
	sbuf_cut(osb, 0)
}

/* One "$VI -e" invocation: the printf body (|sc! prologue, per-file
 * b<k>/%ya 98/groups, vis 2, the writes, the 2q) staged into $P2VIF, then the
 * EXINIT $VI line naming the files in b<k> order. Buffer indices are the
 * position in active[], which is what vi opens. */
static void emit_vi_block(file_patch_t **active, int nactive)
{
	int forward = relative_mode || interactive_mode;
	int regs = relative_mode || interactive_mode || compat_mode;
	sbuf_smake(osb, SB_INIT)
	/* the three printf arguments sit on their own source lines, spliced by
	 * backslash-newline continuations, so the output is unchanged */
	emit_body_head(osb, regs, 0);
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
	emit_write_tail(osb, nactive, NULL);
	sq_write(osb->s, osb->s_n);
	fputs("' > \"$P2VIF\"\n" P2VI_VICALL " $VI -e", stdout);
	for (int k = 0; k < nactive; k++) {
		fputc(' ', stdout);
		sq_path(active[k]->path);
	}
	fputs(" \"$P2VIF\"\n", stdout);
	free(osb->s);
}

/* Global buffer index in uf[], the order the single $VI call opens the real
 * files in. Matched by path: the host's and a compat block's entries for one
 * file are distinct file_patch_t but one physical file, so one buffer. */
static int uf_index(file_patch_t **uf, int nuf, file_patch_t *fp)
{
	for (int i = 0; i < nuf; i++)
		if (!strcmp(uf[i]->path, fp->path))
			return i;
	return -1;
}

/* One section's edit body - no prologue, no register defaults, no identity
 * gate, no vis 2/w/2q tail: the per-file buffer select, its register cache and
 * its groups. Buffer indices are global (uf_index), since one $VI call opens
 * every file. The body is staged as its own buffer and run verbatim through %@,
 * which inserts the yanked bytes without rescanning them, so its top-level
 * separators stay raw however deep the call sits. */
static void emit_section_body(sbuf *out, file_patch_t **files, int nf,
			      file_patch_t **uf, int nuf)
{
	/* The driver expands this body through "2sc % : ?%@<reg>", so xexp is
	 * still '%' here - but the body's own %ya/%f> mean the all-lines range,
	 * not expansion. Reset it up front; the error sites re-enable it
	 * locally through emit_reg_call. */
	sb_str(out, "2sc!");
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
	/* A dangling separator from the last error check would be an empty
	 * command when %@ runs the body, which ex reports as unknown. */
	if (out->s_n > 0 && out->s[out->s_n - 1] == sep)
		out->s_n--;
}

/* The writes a compat block owns: the files no other section touches, which the
 * driver's write tail therefore leaves alone. A block's body only runs when
 * its identity gate fired, so a file the block's origin creates (lsp.c for
 * lsp.sh) is written when the origin is applied and never conjured - an empty
 * one - out of a buffer the block did not edit. Emitted inside the body, ahead
 * of the announce, so reaching the print still means everything landed. */
static void emit_section_writes(sbuf *out, file_patch_t **files, int nf,
				file_patch_t **uf, int nuf, const char *own)
{
	int k, gi, n = 0;
	for (k = 0; k < nf; k++) {
		gi = uf_index(uf, nuf, files[k]);
		if (gi < 0 || !own[gi])
			continue;
		/* Leave raw ex mode for the writes, the way the driver's write
		 * tail does: the '"file" 100L [w]' message is only syntax
		 * highlighted outside vis 3. Re-entered right after. */
		if (!n++) {
			EMIT_SEP(out);
			sb_str(out, "vis 2");
		}
		EMIT_SEP(out);
		sb_printf(out, "b%d", gi);
		EMIT_SEP(out);
		sb_str(out, "w");
	}
	if (n) {
		EMIT_SEP(out);
		sb_str(out, "vis 3");
	}
}

/* A compat block announces itself as the LAST command of its own body: the body
 * only runs when its identity gate fired, and reaching its end means every
 * edit applied - so the print is proof of application, not of intent. Silent on
 * a clean tree. No DBG switch hides it: it is the only outside evidence that a
 * compat block ran. */
static void emit_compat_announce(sbuf *out, int reg, char *origin)
{
	EMIT_SEP(out);
	sb_printf(out, "p compat %d applied: src=%s", reg,
		  origin ? origin : "");
}

/* Stage one section body as a shell here-string into "$P2VIF".<suf>, the file
 * the single $VI call opens as a buffer. The suffix is the block's own flag
 * register - what "# Compat <reg>" above it says and what -E takes as its
 * selector - so the staged files name themselves; the host section, which has
 * no flag, keeps 0. It is a label, not an index: the sections are staged, and
 * opened, in run order, and that order is what the driver's b<N> counts. */
static void stage_section(sbuf *body, int suf)
{
	printf("printf '%%s\\n' '");
	sq_write(body->s, body->s_n);
	printf("' > \"$P2VIF\".%d\n", suf);
}

/* A section to run in the single call: its files, its register, and (for a
 * compat block) its flag and the block it customizes from. */
typedef struct {
	file_patch_t **files;
	int nf;
	int reg;		/* register the driver yanks/executes the body from */
	int secbuf;		/* global buffer index of the staged body */
	int suf;		/* "$P2VIF".<suf>: the flag register, 0 = host */
	int flagk;		/* per-compat-block flag slot (base+flagk); -1 host */
	compat_block_t *cb;	/* NULL for the host section */
} section_t;

/* A compat section's identity gate, asked of the flag the driver set (one per
 * block, the block's register REG_FLAG_BASE+flagk, "1" when every src= of that
 * block's label is in the applied set and "0" otherwise): "fr <reg>; f> 1"
 * reads the flag and the trailing "??" fires the %@ call that follows only
 * when it is set. The host section is unconditional ("? "). The -e path runs
 * this very code, over the set it publishes as $P2VI_PATCH itself.
 *
 * The read leaves xfr pointing at the flag register, but the section body
 * the call runs begins with its own "fr 98" (see emit_section_body), which
 * restores the file cache before anything else searches. */
static void emit_identity_gate(sbuf *out, section_t *s)
{
	if (!s->cb) {
		sb_str(out, "? ");
		return;
	}
	sb_printf(out, "fr %d", REG_FLAG_BASE + s->flagk);
	EMIT_SEP(out);
	sb_str(out, "f> 1");
	EMIT_SEP(out);
	sb_str(out, "?? ");
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

/* Redefine REG_QF2 to the assert form, so an error site calling it quits 1 -
 * unless QF2=1 emptied REG_QF2A, which is the whole point of routing through
 * it: a run told to fall through must keep falling through however many times
 * a block rewrites the policy. The body is one command with no separator of
 * its own, so it needs no escape level and reads the same inside a ??-arm as
 * at the driver's top level.
 * The trailing separator stays raw either way: it ends this command in the
 * stream the driver's top level parses, and escaping it would fold every
 * following driver command - the block's buffer select and %ya included - into
 * the arm, to run only when the arm fires. */
static void emit_qf2_assert(sbuf *out)
{
	sb_printf(out, "%dreg ? %%@%d", REG_QF2, REG_QF2A);
	EMIT_SEP(out);
}

/* Redefine REG_QF2 to empty: error sites in the following body still report,
 * through REG_ERR2 -> REG_HDLR, but no longer abort. */
static void emit_qf2_clear(sbuf *out)
{
	sb_printf(out, "%dreg", REG_QF2);
	EMIT_SEP(out);
}

/* Host quit override, emitted once before the host body when any origin is
 * present: "211reg fr <ANY>:f> 1:??!? %@221:fr 98". A miss on the shared
 * any-origin flag (a clean tree, no origin in the applied set) is an error ??!
 * catches and asserts on,
 * exactly as a non-compat script does; a hit leaves it silent, so the host is
 * best-effort and falls through its own mismatches. It lives inside 211 and
 * re-runs per error site, so the flag is read with a register f>, never an
 * anchor. The assert is the shared 221 body, so QF2=1 relaxes this arm too.
 *
 * The trailing "fr 98" is load-bearing: ex_find's register redirection is the
 * global xfr, so reading the flag leaves every later search pointed at it, and
 * the next group would search the flag's "1" instead of the file cache. It sits
 * outside the arm, so the restore happens whether the arm asserts or not. */
static void emit_host_override(sbuf *out)
{
	sb_printf(out, "%dreg fr %d", REG_QF2, REG_FLAG_ANY);
	EMIT_ESCSEP(out);
	sb_str(out, "f> 1");
	EMIT_ESCSEP(out);
	sb_printf(out, "?" "?!? %%@%d", REG_QF2A);
	EMIT_ESCSEP(out);
	sb_str(out, "fr 98");
	EMIT_SEP(out);
}

/* The anchor slots base..base+n-1 as one expression, joined by op (';' is OR,
 * ',' is AND): the DNF prefix a "??" branches on instead of the last command's
 * status. */
static void sb_slots(sbuf *out, int base, int n, int op)
{
	for (int k = 0; k < n; k++) {
		if (k)
			sb_chr(out, op);
		sb_printf(out, "%d", base + k);
	}
}

/* Block-head quit policy: a block asserts iff no later block over the same file
 * has a fired origin. Each later same-file origin's presence is recorded as an
 * anchor slot and the slots ORed to redefine 211 - any present suppresses, none
 * asserts. A statically-last block emits no test at all and so asserts
 * unconditionally, restoring the assert the host override relaxed. */
static void emit_block_qf2(sbuf *out, section_t *secs, int nsec, int i)
{
	int nlater = 0;
	for (int j = i + 1; j < nsec; j++) {
		if (!secs[j].cb || secs[j].flagk < 0 ||
		    !sections_share_file(&secs[i], &secs[j]))
			continue;
		sb_printf(out, "fr %d", REG_FLAG_BASE + secs[j].flagk);
		EMIT_SEP(out);
		sb_str(out, "f> 1");
		EMIT_SEP(out);
		sb_printf(out, "%d?" "?", FLAG_SLOT_BASE + nlater++);
		EMIT_SEP(out);
	}
	if (!nlater) {
		emit_qf2_assert(out);
		return;
	}
	sb_slots(out, FLAG_SLOT_BASE, nlater, ';');
	sb_str(out, "?" "?");
	emit_qf2_clear(out);
	sb_slots(out, FLAG_SLOT_BASE, nlater, ';');
	sb_str(out, "?" "?!");
	emit_qf2_assert(out);
}

/* The basename of a path: the text after the last '/'. */
static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

/* A compat block's origin label, as its src= fields' basenames: *out[i] is
 * one field, the count is returned. The label is "a.sh src=b.sh" (no
 * "src=" on the first), the shape compat_origin_label builds. */
static int compat_src_fields(compat_block_t *cb, char ***out)
{
	const char *s = cb->origin ? cb->origin : "";
	char **v = NULL;
	int n = 0, cap = 0;
	*out = NULL;
	while (s && *s) {
		const char *e = strstr(s, " src=");
		const char *end = e ? e : s + strlen(s);
		const char *b = s;
		for (const char *p = s; p < end; p++)
			if (*p == '/')
				b = p + 1;
		ARR_PUSH(v, n, cap)
		v[n++] = dup_n(b, end - b);
		s = e ? e + 5 : NULL;
	}
	*out = v;
	return n;
}

/* One src= member as a pattern for REG_APPLIED: "[ /]<base> ". The register is
 * space padded at both ends, so the delimiters always exist and a name matches
 * only whole - never as the tail of a longer one ("p.sh" inside "lsp.sh") -
 * while the '/' alternative accepts the "./x.sh" and "/abs/x.sh" spellings a
 * caller may have put in $P2VI_PATCH for the basename the field stores.
 * Every byte outside [A-Za-z0-9_-] goes in a one-character class, which is
 * literal to the regex and inert in the ex_arg escape layer both, so a '.' in
 * a script name needs no escape byte and cannot degrade into "any character".
 * A ']' or '^' in a script name is not representable this way and not
 * supported. */
static void sb_src_pat(sbuf *out, const char *base)
{
	sb_str(out, "[ /]");
	for (const char *p = base; *p; p++) {
		if (isalnum((unsigned char)*p) || *p == '_' || *p == '-')
			sb_chr(out, *p);
		else
			sb_printf(out, "[%c]", *p);
	}
	sb_chr(out, ' ');
}

/* The compat flags, decided in ex from the applied set alone.
 *
 * REG_APPLIED already holds "<space> patch1.sh patch2.sh ... <space>", written
 * by the body head. Everything here is the editor's: "fr REG_APPLIED" points
 * searching at that register, each block writes its own flag 0, searches the
 * register once per src= member recording the outcome in an anchor slot, and
 * the ANDed slots ("20,21??") rewrite the flag to 1 - and REG_FLAG_ANY with it,
 * so the any-origin flag needs no second pass. A block with no src= field at
 * all keeps its 0 and never fires.
 *
 * The arm holds two commands, joined by an escaped separator: ex_arg unescapes
 * it, so the ex_exec the arm runs sees a chain. A gate that misses leaves xpret
 * set and the "??" returns xuerr, which with the default xerr is neither
 * printed nor fatal - the way any unfired arm reads.
 *
 * The closing "fr 98" hands searching back to the file cache: every section
 * body sets it again itself, but nothing should have to rely on that. */
static void emit_compat_flags(sbuf *out, section_t *secs, int nsec)
{
	sb_printf(out, "%dreg 0", REG_FLAG_ANY);
	EMIT_SEP(out);
	sb_printf(out, "fr %d", REG_APPLIED);
	EMIT_SEP(out);
	for (int i = 0; i < nsec; i++) {
		char **fields;
		int nf, reg;
		if (!secs[i].cb)
			continue;
		reg = REG_FLAG_BASE + secs[i].flagk;
		nf = compat_src_fields(secs[i].cb, &fields);
		/* one source line per block: its default, its scans, its arm */
		EMIT_LB(out);
		EMIT_SEP(out);
		sb_printf(out, "%dreg 0", reg);
		EMIT_SEP(out);
		for (int k = 0; k < nf; k++) {
			sb_str(out, "f> ");
			sb_src_pat(out, fields[k]);
			EMIT_SEP(out);
			sb_printf(out, "%d?" "?", SRC_SLOT_BASE + k);
			EMIT_SEP(out);
			free(fields[k]);
		}
		free(fields);
		if (!nf)
			continue;
		sb_slots(out, SRC_SLOT_BASE, nf, ',');
		sb_printf(out, "?" "? %dreg 1", reg);
		EMIT_ESCSEP(out);
		sb_printf(out, "%dreg 1", REG_FLAG_ANY);
		EMIT_SEP(out);
	}
	EMIT_LB(out);
	EMIT_SEP(out);
	sb_str(out, "fr 98");
	EMIT_SEP(out);
}

/* Call half: yank the section body into its register and %@-call it -
 * unconditionally for the host, behind the identity gate on the section's flag
 * register for a compat block. Bracketed with the "2sc %" / "2sc!" expansion
 * window, since the driver prologue's |sc! leaves xexp inert. */
static void emit_driver_call(sbuf *out, section_t *secs, int nsec, int i,
			     file_patch_t **uf, int nuf, const char *own)
{
	section_t *s = &secs[i];
	/* Rewind every real file this section touches: an earlier block
	 * leaves the cursor deep in the buffer, and the body's
	 * relative searches key off the current line. The rewind is
	 * unconditional, so a block whose origin is absent still rewinds the
	 * buffer of the file that origin creates - empty, so line 1 of it is
	 * "invalid range". Only a section holding such a file pays for
	 * "err 0": a rewind over a file the host patches cannot fail. */
	int risky = 0;
	for (int k = 0; k < s->nf; k++) {
		int gi = uf_index(uf, nuf, s->files[k]);
		risky |= gi >= 0 && own[gi];
	}
	if (risky) {
		sb_str(out, "err 0");
		EMIT_SEP(out);
	}
	for (int k = 0; k < s->nf; k++) {
		sb_printf(out, "b%d", uf_index(uf, nuf, s->files[k]));
		EMIT_SEP(out);
		sb_str(out, "1");
		EMIT_SEP(out);
	}
	if (risky) {
		sb_str(out, "err 1");
		EMIT_SEP(out);
	}
	/* Set this block's quit policy before its body runs: assert if it is the
	 * last firing block over its file, suppress otherwise. */
	if (s->cb)
		emit_block_qf2(out, secs, nsec, i);
	/* Readability line break where the setup ends and the dispatch begins:
	 * everything above is this section's rewinds and quit policy, everything
	 * below its staged buffer, register and gated call. */
	EMIT_LB(out);
	EMIT_SEP(out);
	sb_printf(out, "b%d", s->secbuf);
	EMIT_SEP(out);
	sb_printf(out, "%%ya %d", s->reg);
	EMIT_SEP(out);
	sb_str(out, "2sc %");
	EMIT_SEP(out);
	emit_identity_gate(out, s);
	sb_printf(out, "%%@%d", s->reg);
	EMIT_SEP(out);
	sb_str(out, "2sc!");
	EMIT_SEP(out);
}

/* The whole patch as a single $VI call: the real files as b0..bN-1, one staged
 * buffer per section (host, then each compat block), and a driver buffer EXINIT
 * yanks into register 97 and runs. The driver defines the state registers once,
 * %@-calls each section in application order, then writes every real file and
 * quits. An identity gate that misses only skips its block's call - nothing
 * quits the shared process, so later sections still run. */
static void emit_one_call(file_patch_t **active, int nactive)
{
	section_t *secs = emalloc((ncompat + 1) * sizeof(*secs));
	int nsec = 0, compat_reg = 50, nwrite;
	file_patch_t **uf;
	char *own;		/* uf slots a compat body writes itself */
	int nuf = 0;

	/* Sections in run order: host, then every compat block (all post). */
	if (nactive > 0) {
		secs[nsec].files = active;
		secs[nsec].nf = nactive;
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
		secs[nsec].reg = compat_reg++;
		/* the flag slot is the block's own index, so a skipped block
		 * does not shift the registers the rest read */
		secs[nsec].flagk = c;
		secs[nsec].cb = cb;
		nsec++;
		nflag++;
	}

	/* Buffer order follows the sections, not files[]: a script's stored
	 * compat regions sit before its host patch, so -d parses them in the
	 * other order than the run that derived them did, and a files[]-ordered
	 * b<N> would renumber every buffer across a regeneration. */
	uf = emalloc((nfiles + ncompat + 1) * sizeof(*uf));
	for (int i = 0; i < nsec; i++)
		for (int j = 0; j < secs[i].nf; j++)
			if (uf_index(uf, nuf, secs[i].files[j]) < 0)
				uf[nuf++] = secs[i].files[j];
	nwrite = nuf;

	/* Files no host section edits: their only writer is the compat block
	 * that names them, and it writes them from inside its own gated body -
	 * so an origin's new file (its whole point is that it does not exist
	 * yet) is not created empty by a run the origin is missing from. One
	 * pass collects each slot's writers (1 = a block, 2 = the host section),
	 * and a slot no host wrote is the one its block owns. */
	own = ecalloc(nuf + 1, 1);
	for (int i = 0; i < nsec; i++)
		for (int j = 0; j < secs[i].nf; j++) {
			int gi = uf_index(uf, nuf, secs[i].files[j]);
			if (gi >= 0)
				own[gi] |= secs[i].cb ? 1 : 2;
		}
	for (int i = 0; i < nuf; i++)
		own[i] = own[i] == 1;

	fputs("# Body too large for EXINIT/argv: stage it in a file\n"
	      "( : > /tmp/p2vi.$$.d ) 2>/dev/null && P2VIF=/tmp/p2vi.$$ || P2VIF=./p2vi.$$\n"
	      "trap 'rm -f \"$P2VIF\".*' EXIT\n\n", stdout);

	/* the driver references these before the bodies are staged */
	for (int i = 0; i < nsec; i++) {
		secs[i].secbuf = nuf + i;
		secs[i].suf = secs[i].cb ? REG_FLAG_BASE + secs[i].flagk : 0;
	}

	/* The driver (".d") is staged first: prologue + register defaults,
	 * shell switches, then orchestration and the final writes. */
	sbuf_smake(osb, SB_INIT)
	emit_body_head(osb, 1, nflag > 0);
	if (nflag > 0) {
		/* the driver decides every identity gate itself, out of the
		 * applied set the head just put in REG_APPLIED */
		emit_compat_flags(osb, secs, nsec);
		/* then, before any body: with an origin present (the any flag
		 * set) the host override relaxes its own quit chain, so the
		 * compat blocks can repair its misses; on a clean tree 211
		 * keeps its default assert */
		emit_host_override(osb);
	}
	for (int i = 0; i < nsec; i++)
		emit_driver_call(osb, secs, nsec, i, uf, nuf, own);
	emit_write_tail(osb, nwrite, own);
	sq_write(osb->s, osb->s_n);
	putchar('\'');
	printf(" > \"$P2VIF\".d\n");
	free(osb->s);

	/* Stage each section body after the driver. */
	for (int i = 0; i < nsec; i++) {
		section_t *s = &secs[i];
		int sv_rel = 0;
		if (s->cb) {
			/* the block's identity gate, spelled out for a reader:
			 * the flag register emit_compat_flags writes, and every
			 * src= that has to be in the applied set for it to reach
			 * 1. Each origin carries the "src=" the label leaves off
			 * its first, so the fields read alike and grep alike.
			 * Every block is post, so nothing says so. */
			char **fields;
			int nf = compat_src_fields(s->cb, &fields);
			printf("# Compat %d", REG_FLAG_BASE + s->flagk);
			for (int k = 0; k < nf; k++) {
				printf(" src=%s", fields[k]);
				free(fields[k]);
			}
			free(fields);
			printf("\n");
			compat_win_enter(&sv_rel);
			if (!interactive_mode)
				inject_deltas(s->files, s->nf, &s->cb->deltas);
		}
		sbuf_smake(bsb, SB_INIT)
		emit_section_body(bsb, s->files, s->nf, uf, nuf);
		if (s->cb) {
			emit_section_writes(bsb, s->files, s->nf, uf, nuf, own);
			emit_compat_announce(bsb, s->suf, s->cb->origin);
		}
		sbuf_nul(bsb)
		stage_section(bsb, s->suf);
		free(bsb->s);
		if (s->cb)
			compat_win_leave(sv_rel);
	}

	/* real files, section bodies, driver last - so it is current at EXINIT
	 * and "%ya 97" yanks it */
	fputs(P2VI_VICALL " $VI -e", stdout);
	for (int i = 0; i < nuf; i++) {
		fputc(' ', stdout);
		sq_path(uf[i]->path);
	}
	for (int i = 0; i < nsec; i++)
		printf(" \"$P2VIF\".%d", secs[i].suf);
	printf(" \"$P2VIF\".d\n");

	for (int i = 0; i < nsec; i++)
		if (secs[i].cb)
			free(secs[i].files);
	free(secs);
	free(uf);
	free(own);
}

/* A delta store as === DELTA <path> === sections; a file whose groups were all
 * left alone contributes nothing. */
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
		sbuf_nul(sb)
		fputs(sb->s, stdout);
		printf("%s\n", end_tag_wr);
	}
	free(sb->s);
}

/* Every compat block as a terminator-fenced tail region after exit 0 and before
 * the host === PATCH2VI PATCH === (which stays last, to EOF). One region per
 * compat patch, self-contained - its delta customizations and its whole unified
 * diff - so -d regenerates it and -i edits it without re-running the origin.
 * === COMPAT PATCH === is that diff and nothing else, stored verbatim, so a -C
 * second positional comes back out as the patch its author handed in. The inner
 * sub-sections close with === END === like the host's, so the reader reaches
 * === END COMPAT === with no section open.
 *
 * A block's identity gate is the applied set, so nothing of it is stored here:
 * it is derived from $P2VI_PATCH at run time. */
static void emit_compat_storage(void)
{
	for (int c = 0; c < ncompat; c++) {
		compat_block_t *cb = &compat_blocks[c];
		printf("=== PATCH2VI COMPAT %d src=%s ===\n",
		       REG_FLAG_BASE + c, cb->origin ? cb->origin : "");
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
 * -e: run a generated script through the embedded editor, no shell involved.
 * The grammar is closed and self-generated - header assignments, one printf'd
 * ex body per editor invocation, the "$VI -e <files> $P2VIF" line that runs it -
 * so it is parsed exactly and anything outside it is an error, not a
 * best-effort guess. Each block gets its own editor lifetime, mirroring the
 * separate $VI process the shell would spawn.
 */

static int exec_mode;		/* -e: execute the input script */
static const char *exec_script;	/* its path, i.e. the script's $0 */

/* Shell variables assigned by the script header, looked up before the
 * environment - so an assignment shadows an inherited value while the header's
 * own conditionals still test the inherited one. */
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

/* Header assignments belong to one script: replaying two (-C) must not let the
 * first's shadow the environment while the second's conditionals are read. */
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

/* The [A-Za-z0-9_] run at *s as a fresh string, advancing *s past it. */
static char *sh_name(const char **s)
{
	sbuf_smake(sb, 32)
	while (isalnum((unsigned char)**s) || **s == '_')
		sbuf_chr(sb, *(*s)++)
	sbufn_ret(sb, sb->s)
}

/* One double-quoted shell word: ${VAR}, ${VAR:-default} and ${VAR:+alternate}
 * (both nestable), $VAR, $0, $(printf '\NNN') and the escapes that survive
 * double quotes. Everything else is literal. */
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
				sbuf_nul(def)
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
		sbuf_nul(val)
		s = val->s;
	}
	sbuf_smake(out, SB_INIT)
	if (!(ret = sh_expand(s, out))) {
		sbuf_nul(out)
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

/* One editor lifetime: the real files as b0..bN-1, then one in-RAM buffer per
 * staged section body, then the driver body. EXINIT only exists to lift the
 * body out of the buffer the shell had to pass it in; -e holds it already, so
 * it fills the register the body may recurse through and runs the chain itself.
 * The driver does its own "b<k>:%ya <reg>:...%@<reg>" per section, so the
 * section bodies must be resident before it runs - at exactly the emitter's
 * uf-count + section index. */
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

/* The script's separator byte, read where ex learns it - the body's own
 * "|sc! <esc><sep>|" prologue, which holds the bytes as they are with a dynamic
 * escape and a doubled one with the default backslash. Per block, since a
 * replay may span two scripts and each body keeps its own. */
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

/* EXINIT='<init>' $VI -e 'file' ... "$P2VIF": the init is the fixed one the
 * emitter writes and -e supplies its effect itself, so it is only checked */
static int parse_vi_call(const char *s, p2vi_block_t *blk)
{
	int closed;
	sbuf_smake(w, SB_INIT)
	if (strncmp(s, P2VI_VICALL, strlen(P2VI_VICALL))) {
		free(w->s);
		return sh_err("vi call", s);
	}
	s += strlen(P2VI_VICALL);
	if (strncmp(s, " $VI -e", 7)) {
		free(w->s);
		return sh_err("vi call", s);
	}
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
		/* one quoted word, as sq_path wrote it: a quote closes it and a
		 * '\'' reopens it around an embedded quote */
		if (*s != '\'') {
			free(w->s);
			return sh_err("vi call", s);
		}
		sbuf_cut(w, 0)
		closed = 0;
		for (s++; *s && !closed; ) {
			if (*s == '\'' && s[1] == '\\' && s[2] == '\'' &&
			    s[3] == '\'') {
				sbuf_chr(w, '\'')
				s += 4;
			} else if (*s == '\'') {
				s++;
				closed = 1;
			} else
				sbuf_chr(w, *s++)
		}
		if (!closed) {
			free(w->s);
			return sh_err("vi call", s);
		}
		sbuf_nul(w)
		blk->paths = erealloc(blk->paths,
				      (blk->npaths + 1) * sizeof(char *));
		blk->paths[blk->npaths++] = uc_dup(w->s);
	}
	free(w->s);
	return 0;
}

/* One shell word: adjacent single-quoted, double-quoted and unquoted runs
 * concatenate into it. A single-quoted run is verbatim, a double-quoted one
 * goes through sh_expand. */
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
		sbuf_nul(dq)
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
 * concatenates its arguments and ends the line. sh writes at most one word, and
 * only ever whole commands, so nothing here changes how the rest parses. */
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
		sbuf_nul(fmt)
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
	sbuf_nul(exp)
	if (ret >= 0)
		*out = uc_dup(exp->s);
	free(exp->s);
	return ret;
}

/* Staged bodies gathered between two "$VI" calls, keyed by the "$P2VIF" suffix
 * they redirect into: "" single-body, "d" driver, "0","1",... sections. */
typedef struct {
	char **raw;	/* the raw printf command, one per staged body */
	char **suf;	/* its "$P2VIF" suffix */
	int n, cap;
} pend_t;

static void pend_push(pend_t *p, const char *raw, const char *suf)
{
	/* one capacity for two arrays: the saved copy gives the second ARR_PUSH
	 * the same pre-growth value, so both grow in step */
	int cap = p->cap;
	ARR_PUSH(p->raw, p->n, p->cap)
	ARR_PUSH(p->suf, p->n, cap)
	p->raw[p->n] = uc_dup(raw);
	p->suf[p->n++] = uc_dup(suf);
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

/* The gathered bodies as one block: either the single "" body, or the "d"
 * driver plus the section bodies. The sections keep the order they were staged
 * in, which is the order the $VI call opens them in and so the order the
 * driver's b<N> counts; their "$P2VIF" suffix is a label (the block's flag
 * register, or 0 for the host) and is not read as an index. Scripts emitted
 * when it was one - ".0", ".1", ".2" - parse the same, being in that order. */
static int pend_finish(pend_t *p, p2vi_block_t *blk)
{
	int drv = -1, old = -1, nsec = 0, ret = 0;
	for (int i = 0; i < p->n; i++) {
		if (!p->suf[i][0])
			old = i;
		else if (!strcmp(p->suf[i], "d"))
			drv = i;
		else
			nsec++;
	}
	if (old >= 0) {
		ret = expand_body(p->raw[old], &blk->body);
	} else if (drv >= 0) {
		blk->sects = ecalloc(nsec + 1, sizeof(char *));
		blk->nsects = nsec;
		nsec = 0;
		for (int i = 0; ret >= 0 && i < p->n; i++) {
			if (!p->suf[i][0] || !strcmp(p->suf[i], "d"))
				continue;
			ret = expand_body(p->raw[i], &blk->sects[nsec++]);
		}
		if (ret >= 0)
			ret = expand_body(p->raw[drv], &blk->body);
	} else
		ret = sh_err("body", "no staged body before $VI call");
	if (ret >= 0)
		blk->sep = body_sepbyte(blk->body);
	return ret;
}

/* The applied set of the script currently being parsed (-e from the earlier
 * scripts on the command line, replay from the earlier scripts in the chain):
 * basenames, so a stored src=a/b.sh field matches the basename a/b.sh puts in
 * $P2VI_PATCH. This is what -e publishes as $P2VI_PATCH for the driver. */
static char **cur_applied;
static int ncur_applied, cur_applied_cap;

static void cur_applied_clear(void)
{
	for (int i = 0; i < ncur_applied; i++)
		free(cur_applied[i]);
	ncur_applied = 0;
}

/* Replace the applied set with the basenames of paths[0..n-1], on top of
 * whatever $P2VI_PATCH the environment carries (the shell chain's own channel
 * - so -E and -C replays over a tree that already carries origins can assert
 * them the way the shell would have inherited them). Both are basenames. */
static void cur_applied_set(const char **paths, int n)
{
	cur_applied_clear();
	const char *env = getenv("P2VI_PATCH");
	if (env && *env) {
		char *cpy = uc_dup(env), *save = NULL;
		for (char *tok = strtok_r(cpy, " ", &save); tok;
		     tok = strtok_r(NULL, " ", &save)) {
			if (!*tok)
				continue;
			ARR_PUSH(cur_applied, ncur_applied, cur_applied_cap)
			cur_applied[ncur_applied++] = uc_dup(base_name(tok));
		}
		free(cpy);
	}
	for (int i = 0; i < n; i++) {
		const char *b = base_name(paths[i]);
		ARR_PUSH(cur_applied, ncur_applied, cur_applied_cap)
		cur_applied[ncur_applied++] = uc_dup(b);
	}
}

/* Free the applied-set paths once, at the end of a run. */
static void cur_applied_free(void)
{
	cur_applied_clear();
	free(cur_applied);
	cur_applied = NULL;
	cur_applied_cap = 0;
}

/* The applied set as the one space separated word $P2VI_PATCH holds in the
 * shell chain. The staged body head writes that variable into REG_APPLIED and
 * every compat block is decided from it, so handing -e the same string is the
 * whole of what -e has to do differently. */
static char *cur_applied_word(void)
{
	sbuf_smake(sb, SB_INIT)
	for (int i = 0; i < ncur_applied; i++) {
		if (i)
			sbuf_chr(sb, ' ')
		sbuf_str(sb, cur_applied[i])
	}
	sbufn_ret(sb, sb->s)
}

/* The script's executable region (everything before "exit 0") as one block per
 * editor invocation. Parsing is separate from running because -e gives each
 * block its own lifetime while the compat session replays them all in one, and
 * that needs the whole list up front. */
static int parse_p2vi_script(FILE *in, p2vi_block_t **blks, int *nblks)
{
	const char *body_end = " > \"$P2VIF\"";
	p2vi_block_t blk = {0};
	pend_t pend = {0};
	int skip = 0, in_body = 0, ret = 0, j;
	char *line;
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
			 * span lines, so they are gathered raw and read once
			 * the whole command is in hand. The redirect's suffix
			 * ("" / ".d" / ".0"...) routes it in pend_finish. */
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
			sbuf_nul(body)
			pend_push(&pend, body->s, suf);
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
			/* the applied set the shell chain would have inherited,
			 * published before the bodies expand so the head's
			 * "229reg  $P2VI_PATCH " resolves to it and the identity
			 * gates decide this call's blocks as a chain would */
			char *ap = cur_applied_word();
			sh_set("P2VI_PATCH", ap);
			free(ap);
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
 * Replay: the same blocks, but as one editor session. Deriving a compat patch
 * means seeing the tree an origin script leaves behind, so the buffers persist
 * across blocks (a later block naming an already edited file switches to that
 * buffer, no disk round-trip), nothing is ever written, and the last block
 * hands the session to the user. Everything that is not a buffer is still reset
 * per block, as under -e: no register cache, "??" tag or separator carries.
 */

#define BODY_DELIM(c) ((c) == sep || (c) == '\n')

/* Drop the body's trailing writes ("b<N> SEP w" per file, then "2q"), parsed
 * from the end: "vis 2", which stays, also occurs inside the quit and interrupt
 * chains and so anchors nothing. */
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

/* b<N> indexes the block's own file list, but a replay session's indices are
 * session-global, so the tokens are rewritten with the session's numbers. Only
 * a whole command counts as one, leaving the literal ":b0:" inside INTR (whose
 * commands are colon-separated) alone. */
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

/* The session's buffer index for a path, opened if this is the first block to
 * name it; mirrors bufs_open()'s append order. */
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
 * (and, for -C, the target) has been replayed but before the user edits it.
 * The compat diff is measured from here to the buffer's final state. */
typedef struct { char *path, *text; } snap_t;
typedef struct { snap_t *v; int n, cap; } snaps_t;
static snaps_t compat_base;

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

/* A baseline entry holding path's on-disk text, unless it has one. A path no
 * baseline block named is untouched, so disk is its post-replay state; without
 * the entry compat_derive() would take it for "opened after the baseline" and
 * drop the edits made to it. */
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
	sbuf_nul(sb)
	free_lines(v, n);
	ARR_PUSH(sn->v, sn->n, sn->cap)
	sn->v[sn->n].path = uc_dup(path);
	sn->v[sn->n++].text = sb->s;
}

/* -C second positional, unified-diff form, applied to the live buffers
 * of the handover session (its script form replays as another block). */
static int compat_apply_diff(const char *path);
static int compat_pre_script;	/* the second positional is a generated script */

/*
 * FAILURE PLACEMENT
 *
 * A QF2=1 run reports every failing site and keeps going, so it ends with the
 * hunks that missed simply not applied - the state is in the buffers, but what
 * was supposed to happen is only in the terminal scrollback. Placement puts it
 * back, using the two things the run leaves behind in registers: REG_FLOG, a
 * line per failure, and the command stream itself (P2VI_REG), which the block
 * ran and which still holds every edit verbatim.
 *
 * The two are joined by the mark. A phase-2 FAIL line reads
 * "<path>:<line>:m<id>", and in the stream a mark address only ever occurs at
 * the head of a whole separator-delimited command, so "<sep>'<id>" names the
 * failed edit and nothing else. What is lost is where it should go: its anchor
 * did not resolve, so the only placement left is the line number the FAIL line
 * carries - the site's line in the original file, which is approximate once the
 * hunks above it have shifted things. So the edit is re-aimed at that line and
 * run; if it fails there too, it goes into the buffer verbatim between marker
 * lines, which loses nothing and is a local edit to fix up. Either way the
 * session is handed over parked on the first such spot.
 */

/* One line of REG_FLOG, cut up where it lies: "FAIL <path>:<line>:m<id>", read
 * off the end so a path holding a colon still resolves. Returns the mark, or -1
 * for a line with none - a phase-1 report, which names an anchor that did not
 * resolve and so no edit to recover; the phase-2 site it was to steer is logged
 * right after it and is the actionable half of the same failure. */
static int fail_parse(char *s, char **path, int *line)
{
	char *c;
	int mark;
	if (strncmp(s, "FAIL ", 5) || !(c = strrchr(s, ':')) || c[1] != 'm'
			|| !isdigit((unsigned char)c[2]))
		return -1;
	mark = atoi(c + 2);
	*c = '\0';
	if (!(c = strrchr(s, ':')) || !isdigit((unsigned char)c[1]))
		return -1;
	*c = '\0';
	*line = atoi(c + 1);
	*path = s + 5;
	return mark;
}

/* The whole command addressing mark <mark>, at whatever depth it sits: a
 * command always starts right after a separator byte, so a quote there is an
 * address and never payload (payload lines are newline-delimited). It ends at
 * the next separator, minus the escapes a nested site carries before it. */
static char *stream_site(const char *body, int mark)
{
	const char *p = body, *e;
	while ((p = strchr(p, xsep))) {
		if (*++p != '\'' || atoi(p + 1) != mark)
			continue;
		for (e = p + 1; isdigit((unsigned char)*e); e++);
		if (e == p + 1 || !(e = strchr(e, xsep)))
			continue;
		while (e[-1] == xesc)
			e--;
		return dup_n(p, e - p);
	}
	return NULL;
}

static int buf_by_path(const char *path)
{
	for (int i = 0; i < xbufcur; i++)
		if (bufs[i].path && !strcmp(bufs[i].path, path))
			return i;
	return -1;
}

/* Put one failure back, in the buffer it belongs to. Returns the row the
 * session should park on, or -1 if that file is not even open. */
static int fail_place(const char *body, const char *path, int line, int mark,
		      int *bi, int *shift)
{
	char *site = stream_site(body, mark);
	const char *addr, *verb;
	int row, len, ok = 0;
	sbuf_smake(sb, SB_INIT)
	if ((*bi = buf_by_path(path)) < 0) {
		free(site);
		free(sb->s);
		return -1;
	}
	bufs_switch(*bi);
	/* every block placed above this one pushed the rest of the file down,
	 * and the failures come in patch order, so the shift simply adds up */
	line += shift[*bi];
	len = lbuf_len(xb);
	row = MAX(0, MIN(len, line) - 1);
	xrow = row;
	xoff = 0;
	if (site) {
		for (addr = site + 1; isdigit((unsigned char)*addr); addr++);
		for (verb = addr; *verb && !isalpha((unsigned char)*verb); verb++);
		/* The mark is what did not resolve, so the same command aimed
		 * at the reported line is the best guess left - taken only
		 * where it cannot lose anything: an insert adds, a substitute
		 * has to match first. A "c" or "d" one line off would quietly
		 * eat a line the patch never mentioned. */
		if (*verb == 'i' || *verb == 's') {
			sb_printf(sb, "%d%s", line, addr);
			sbuf_nul(sb)
			ok = ex_exec(sb->s) == NULL;
			sbuf_cut(sb, 0)
		}
	}
	if (!ok) {
		sb_printf(sb, ">>> p2v FAIL %s:%d:m%d\n", path, line, mark);
		if (site)
			sb_str(sb, site);
		if (sb->s[sb->s_n - 1] != '\n')
			sb_chr(sb, '\n');
		sb_str(sb, "<<< p2v END\n");
		sbuf_nul(sb)
		lbuf_edit(xb, sb->s, row, row, -1, -1);
	}
	shift[*bi] += lbuf_len(xb) - len;
	free(site);
	free(sb->s);
	return row;
}

/* Every failure the block logged, oldest first. Returns the buffer to park the
 * handover on and sets *prow to the row in it, or -1 when the run logged
 * nothing - which is every run that did not fail, so the common path is one
 * register lookup.
 *
 * bodyreg holds the body the logged marks belong to: the driver's own, or the
 * staged section a -E selector run singles out. skip is how many bytes the log
 * already held before that body ran, so a run that reports one section's
 * misses does not place the whole call's. */
static int fail_report(int sepb, int *prow, int bodyreg, int skip)
{
	sbuf *log = ex_regget(REG_FLOG), *body = ex_regget(bodyreg);
	char *s, *nl, *txt, *path;
	int *shift, bi, row, line, mark, first = -1, n = 0;
	if (!log || log->s_n <= skip || !body || !xbufcur)
		return -1;
	shift = ecalloc(xbufcur, sizeof(int));
	/* cut up a copy: the register is the run's own record, which the
	 * handover may well want to read */
	txt = uc_dup(log->s + skip);
	/* the stream is read and re-run under the body's own specials: xesc is
	 * still what its "|sc!" prologue set (the stripped tail never put it
	 * back) and the separator is restated from the block */
	preserve(int, xsep, xsep = sepb;)
	/* every entry is newline-terminated, that being the point of logging
	 * into an upper-case register */
	for (s = txt; (nl = strchr(s, '\n')); s = nl) {
		*nl++ = '\0';
		if ((mark = fail_parse(s, &path, &line)) < 0)
			continue;
		row = fail_place(body->s, path, line, mark, &bi, shift);
		if (row >= 0 && first < 0) {
			first = bi;
			*prow = row;
		}
		n += row >= 0;
	}
	restore(xsep)
	free(txt);
	free(shift);
	if (n)
		fprintf(stderr, "replay: %d failed hunk%s put back at the "
			"reported line; look for \">>> p2v FAIL\"\n",
			n, n > 1 ? "s" : "");
	return first;
}

static int cmd_is(const char *body, int b, int e, const char *s)
{
	int n = strlen(s);
	return e - b == n && !strncmp(body + b, s, n);
}

/* -E <reg>: lift one section's dispatch out of the driver body, so the caller
 * can run the rest of the body, take the compat baseline, and only then run
 * this one block.
 *
 * emit_driver_call writes a gated section as seven commands - "b<sec>",
 * "%ya <reg>", "2sc %", the identity gate's "fr <flag>", "f> 1" and
 * "?? %@<reg>", then "2sc!" - so the flag register names the whole span
 * unambiguously, and the shape is checked rather than trusted. What the lift
 * costs is only the order: every other block still runs where it was stored,
 * above the baseline, so its edits cancel out of the derived diff, and the
 * commands the emitter puts before this dispatch (buffer rewinds, quit policy)
 * are state and not text. Returns the register the span yanks the section body
 * into (fail_report reads the marks out of it), or -1. */
static int body_cut_dispatch(char *body, int sep, int flagreg, char **span)
{
	struct { int b, e; } *c = NULL;
	int n = strlen(body), nc = 0, cap = 0, i, t = -1, reg, p = 0, b, e;
	char pat[32];
	for (;;) {
		b = p;
		while (p < n && !BODY_DELIM(body[p]))
			p++;
		ARR_PUSH(c, nc, cap)
		c[nc].b = b;
		c[nc++].e = p;
		if (p >= n)
			break;
		p++;
	}
	/* "fr <flag>" and "f> 1" alone are not enough: emit_block_qf2 tests a
	 * later block's flag the same way, ahead of the dispatch, and only
	 * records the answer in an anchor slot. The call is the one that runs
	 * the section from its register. */
	snprintf(pat, sizeof(pat), "fr %d", flagreg);
	for (i = 3; i + 3 < nc; i++)
		if (cmd_is(body, c[i].b, c[i].e, pat)
		    && cmd_is(body, c[i + 1].b, c[i + 1].e, "f> 1")
		    && !strncmp(body + c[i + 2].b, "?? %@", 5)) {
			t = i;
			break;
		}
	if (t < 0) {
		free(c);
		fprintf(stderr, "replay: no gated call on register %d\n",
			flagreg);
		return -1;
	}
	if (body[c[t - 3].b] != 'b' || c[t - 3].e - c[t - 3].b < 2
	    || strncmp(body + c[t - 2].b, "%ya ", 4)
	    || !cmd_is(body, c[t - 1].b, c[t - 1].e, "2sc %")
	    || !cmd_is(body, c[t + 3].b, c[t + 3].e, "2sc!")) {
		free(c);
		fprintf(stderr, "replay: register %d is not a section call\n",
			flagreg);
		return -1;
	}
	reg = atoi(body + c[t - 2].b + 4);
	b = c[t - 3].b;
	e = c[t + 3].e;
	if (e < n)		/* the delimiter goes with the span */
		e++;
	/* The span is lifted past the body's write tail, whose "vis 2" has
	 * left the editor a session by then; the sections are ex scripts and
	 * every one of them was emitted under the prologue's "vis 3". */
	*span = emalloc(e - b + 7);
	i = sprintf(*span, "vis 3%c", sep);
	memcpy(*span + i, body + b, e - b);
	(*span)[i + e - b] = '\0';
	memmove(body + b, body + e, n - e + 1);
	free(c);
	return reg;
}

/* Every block in one session, leaving its buffers alive for the caller to read
 * back. With handover the last block leaves the editor to the user instead of
 * returning at the end of its body. snap_blk names the block the compat
 * baseline is taken after (-1 = the last): the blocks past it are a pre-applied
 * resolution, which belongs to the derived patch and so must land above the
 * baseline. split_reg is -E's block selector: that block's dispatch is lifted
 * out of snap_blk's driver body and run after the baseline instead of in
 * place, so the baseline is the tree as it stands before the block runs. */
static int replay_blocks(p2vi_block_t *blks, int nblks, int handover,
			 int snap_blk, int split_reg)
{
	char **paths = NULL, *body, *ln;
	int npaths = 0, *bmap = NULL, nmap = 0, i, k, st = 0, sep, bad = 0;
	int fbuf = -1, frow = 0, failreg = P2VI_REG, logskip = 0;
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
		 * fd 1 would leak its status line into the script on stdout. */
		if (ed_init(handover) < 0) {
			st = -1;
			break;
		}
		/* the tail's flags govern every block's loads, not just the
		 * first: the body run clears bit 4 below (its own chatter is
		 * not the session's to silence) and it must be back before
		 * the next block loads its files and scaffolds */
		xvis = (hand_vis >= 0 ? hand_vis : xvis) | 2;
		/* the staged section bodies load as scaffolding buffers above
		 * the real files and the driver references both by index, so
		 * the map must cover the whole span */
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
			/* remapped like the driver body below: earlier blocks may
			 * have opened files that shift the real-file numbers */
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
		if (split_reg >= 0 && i == snap_blk) {
			char *span = NULL;
			int breg = body_cut_dispatch(body, sep, split_reg,
						     &span);
			if (breg < 0) {
				free(body);
				ed_done();
				st = -1;
				break;
			}
			ex_exec(body);
			if (!xquit) {
				sbuf *lg;
				snap_bufs(&compat_base);
				lg = ex_regget(REG_FLOG);
				logskip = lg ? lg->s_n : 0;
				failreg = breg;
				ex_exec(span);
			}
			free(span);
		} else
			ex_exec(body);
		free(body);
		/* Drop the section scaffolding: its %@ calls have run and applied
		 * their edits, so the handover and the read-back must see only the
		 * real files. The section buffers are the topmost slots; switch to
		 * a real one first so ex_buf/ex_pbuf never dangle. */
		if (blks[i].nsects) {
			bufs_switch(bmap[0]);
			ex_pbuf = ex_tpbuf = ex_buf;
			for (k = 0; k < blks[i].nsects; k++)
				bufs_free(--xbufcur);
		}
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
			/* Present the baseline as a clean, saved file: the "w" that
			 * would have marked it saved went with the stripped tail, so
			 * without this every buffer shows as modified and undo still
			 * reaches back into the replay's own edits. */
			for (k = 0; k < xbufcur; k++)
				lbuf_saved(bufs[k].lb, 1);
			/* a QF2=1 body reaches here having reported its
			 * failures instead of quitting at the first: put what
			 * they were meant to do back into the buffers, after
			 * the save above so the user can undo any of it. Not
			 * while deriving a compat patch - there a miss is the
			 * input to the derivation, not a hunk to fix, and the
			 * blocks would land in the derived diff. */
			if (!compat_capturing)
				fbuf = fail_report(sep, &frow, failreg,
						   logskip);
			ed_serve(fbuf, frow);
		}
		if (!xquit)	/* no counted quit: the block simply ended */
			xquit = -1;
		st = ed_done();
		bad = i + 1;
		if (i + 1 < nblks || !handover) {
			/* the next block starts as a fresh editor over the same
			 * buffers: saved, so :e and :q see no modification and
			 * undo cannot cross the boundary, and with no state */
			if (xbufcur)
				bufs_switch(0);
			for (k = 0; k < xbufcur; k++) {
				struct lbuf *lb = bufs[k].lb;
				lbuf_saved(lb, 1);
				/* exbuf_save() persists the cursor and the marks
				 * live on the lbuf, so the next block would :e
				 * each file with the previous one's position and
				 * marks - and a non-fatal phase-1 miss would fall
				 * through to a phase-2 edit steered by them.
				 * Rewind and drop the marks, as a freshly opened
				 * file has none. */
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

/* Append one script's blocks to *blks. Header assignments are per script and
 * each block carries its own separator, so two scripts' headers never mix. */
/* tol: replay this script with QF2=1, whatever the environment says. A -C
 * derivation replays a target that is expected to collide with the origin -
 * that collision is the whole input to the derivation - and a target quitting
 * at its first missed hunk would leave a tree the fix was never written
 * against. The shipped script does the same thing on its own: a block's host
 * override empties the quit chain, so a target that already carries one keeps
 * going where the same target, stripped back to its base (rebuild), would die.
 * The misses are still reported. */
static int parse_script(const char *path, p2vi_block_t **blks, int *nblks,
			int tol)
{
	FILE *f = fopen(path, "r");
	int st;
	if (!f) {
		perror(path);
		return -1;
	}
	sh_reset();
	if (tol)
		sh_set("QF2", "1");
	exec_script = path;
	st = parse_p2vi_script(f, blks, nblks);
	fclose(f);
	return st;
}

/* Replay the scripts over the tree as it is on disk, in one session the caller
 * reads back: -C replays the origin and then the target, so the user is handed
 * the state both applied to. The scripts keep their own phase policy - the
 * script itself is the single source of truth. snap_sc is the script index of
 * the baseline snapshot, translated into the block index replay_blocks() wants
 * (a script contributes one block per $VI call). split_reg is passed straight
 * through to replay_blocks. */
static int replay_scripts(const char **paths, int nscripts, int handover,
			  int snap_sc, int split_reg)
{
	p2vi_block_t *blks = NULL;
	int nblks = 0, st = 0, i, snap_blk = -1;
	for (i = 0; i < nscripts && st >= 0; i++) {
		/* the applied set is the origins already replayed: the -e path's
		 * counterpart of the $P2VI_PATCH the shell chain would carry */
		cur_applied_set(paths, i);
		/* the origins are replayed as they ship; only the target is
		 * held to the collision it is being measured through */
		st = parse_script(paths[i], &blks, &nblks,
				  (compat_mode || split_reg >= 0)
				  && i == snap_sc);
		if (i == snap_sc)
			snap_blk = nblks - 1;
	}
	cur_applied_free();
	if (st >= 0)
		st = replay_blocks(blks, nblks, handover, snap_blk, split_reg);
	free_blocks(blks, nblks);
	return st;
}

/*
 * -I: edit in the built-in nextvi and convert what changed into a script.
 * Nothing is written back: every buffer the editor leaves behind is diffed
 * against the file as it was on disk, and that diff feeds the same pipeline a
 * diff read from stdin would - so a session that visits several files with :e
 * yields one script covering all of them. Hence the built-in differ below.
 *
 * -E is the same emit stage over a different session: instead of opening bare
 * files, it replays a generated script and hands over the tree that replay
 * leaves behind. The diff base is still the file on disk, so the new script
 * carries the old one's effect plus the user's changes - it replaces the input
 * script rather than extending it (extending is -C).
 */
static int edit_mode;		/* -I: edit, then emit the diff as a script */
static int amend_mode;		/* -E: replay a script, edit, re-emit it */
static int amend_inplace;	/* -oE: -o's file is -E's own script */

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
	ARR_PUSH(d->v, d->n, d->cap)
	d->v[d->n].t = t;
	d->v[d->n++].s = s;
}

/* Line census over one span, for patience anchoring: how often a line occurs on
 * each side, and where last. Open addressing, strcmp on collision. */
typedef struct {
	const char *s;
	int co, cn;		/* occurrences in old[] / new[] */
	int in;			/* the last index seen in new[] */
} lmap_t;

static lmap_t *lmap_slot(lmap_t *t, unsigned mask, const char *s)
{
	unsigned i = hash_str(s, 5381) & mask;
	while (t[i].s && strcmp(t[i].s, s))
		i = (i + 1) & mask;
	return &t[i];
}

/* Patience anchors for a span too large to diff outright: lines occurring
 * exactly once on each side pair up unambiguously, and the longest increasing
 * subsequence of those pairs is a set of matches no sane diff would cross.
 * Splitting there leaves sub-spans small enough for the exact LCS below, so a
 * 2000-line function whose body moved by two lines diffs as the handful of
 * lines that really changed, not as 4000 lines of delete-all/insert-all. */
static int diff_anchors(char **old, int os, int oe, char **new, int ns, int ne,
			int **aop, int **anp)
{
	int n = oe - os, m = ne - ns, nc = 0, len = 0, i, k;
	int *co, *cn, *pile, *prev, *ao, *an;
	unsigned cap = 8, mask;
	lmap_t *t;
	while (cap < (unsigned)(n + m) * 2)
		cap <<= 1;
	mask = cap - 1;
	t = ecalloc(cap, sizeof(*t));
	for (k = os; k < oe; k++) {
		lmap_t *e = lmap_slot(t, mask, old[k]);
		e->s = old[k];
		e->co++;
	}
	for (k = ns; k < ne; k++) {
		lmap_t *e = lmap_slot(t, mask, new[k]);
		e->s = new[k];
		e->cn++;
		e->in = k;
	}
	/* candidates in old order, so the LIS below only has to sort by new */
	co = emalloc(n * sizeof(int));
	cn = emalloc(n * sizeof(int));
	for (k = os; k < oe; k++) {
		lmap_t *e = lmap_slot(t, mask, old[k]);
		if (e->co == 1 && e->cn == 1) {
			co[nc] = k;
			cn[nc++] = e->in;
		}
	}
	free(t);
	if (!nc) {
		free(co);
		free(cn);
		return 0;
	}
	/* longest strictly increasing subsequence of cn[], patience piles:
	 * pile[l] = candidate ending the smallest length-l run so far */
	pile = emalloc((nc + 1) * sizeof(int));
	prev = emalloc(nc * sizeof(int));
	for (k = 0; k < nc; k++) {
		int lo = 1, hi = len, pos;
		while (lo <= hi) {
			int mid = (lo + hi) / 2;
			if (cn[pile[mid]] < cn[k])
				lo = mid + 1;
			else
				hi = mid - 1;
		}
		pos = lo;
		prev[k] = pos > 1 ? pile[pos - 1] : -1;
		pile[pos] = k;
		if (pos > len)
			len = pos;
	}
	ao = emalloc(len * sizeof(int));
	an = emalloc(len * sizeof(int));
	for (k = len - 1, i = pile[len]; k >= 0; k--, i = prev[i]) {
		ao[k] = co[i];
		an[k] = cn[i];
	}
	free(co);
	free(cn);
	free(pile);
	free(prev);
	*aop = ao;
	*anp = an;
	return len;
}

/* Ops turning old[os..oe) into new[ns..ne): common head and tail lines first,
 * so the table only sees what is left, then either the classic LCS table or -
 * where that table would be too large - a patience split into sub-spans around
 * unique common lines, each diffed by the same routine. Only a span both too
 * large and anchorless degrades to delete-all/insert-all. */
static void diff_region(dops_t *d, char **old, int os, int oe,
			char **new, int ns, int ne)
{
	int n, m, nsuf = 0, na = 0, i, j, k;
	int *c, *ao = NULL, *an = NULL;
	while (os < oe && ns < ne && !strcmp(old[os], new[ns])) {
		dop_add(d, ' ', old[os]);
		os++;
		ns++;
	}
	while (oe > os && ne > ns && !strcmp(old[oe - 1], new[ne - 1])) {
		oe--;
		ne--;
		nsuf++;
	}
	n = oe - os;
	m = ne - ns;
	if (n > 0 && m > 0 && (double)(n + 1) * (m + 1) > DIFF_MAX_CELLS)
		na = diff_anchors(old, os, oe, new, ns, ne, &ao, &an);
	if (na > 0) {
		int po = os, pn = ns;
		for (k = 0; k < na; k++) {
			diff_region(d, old, po, ao[k], new, pn, an[k]);
			dop_add(d, ' ', old[ao[k]]);
			po = ao[k] + 1;
			pn = an[k] + 1;
		}
		diff_region(d, old, po, oe, new, pn, ne);
		free(ao);
		free(an);
		goto tail;
	}
	if (n == 0 || m == 0 || (double)(n + 1) * (m + 1) > DIFF_MAX_CELLS) {
		for (i = os; i < oe; i++)
			dop_add(d, '-', old[i]);
		for (j = ns; j < ne; j++)
			dop_add(d, '+', new[j]);
		goto tail;
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
tail:
	/* the tail trimmed above closes the span, after whatever filled it */
	for (k = 0; k < nsuf; k++)
		dop_add(d, ' ', old[oe + k]);
}

/* An op list as unified diff text for path: header, then one hunk per run of
 * changes with DIFF_CTX context lines around it. Nothing is written when the
 * list holds no change; the op list is the only input, so every way of deriving
 * one serializes through here. */
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
		/* to the last change whose context still touches this one's */
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

/* The -C second positional's form, sniffed from its first line as the ordinary
 * input is: 1 = a generated script (replayed as another block), 0 = a unified
 * diff (spliced into the live buffers), -1 = unreadable. */
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

/* Where img[] sits in lines[], at or after from, preferring the occurrence
 * nearest hint - the diff's own coordinate, stale by construction on a tree the
 * origin and the target already changed. An empty image is a pure insertion:
 * hint itself, clamped. */
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

/* The live session buffer holding path, opened if no replayed block named it -
 * in which case disk is its baseline text, so it is snapshotted too. */
static struct lbuf *compat_openbuf(char *path)
{
	int i = buf_by_path(path);
	if (i < 0) {
		snap_seed(&compat_base, path);
		xmpt = 0;
		ec_edit("", "e", path);
		i = buf_by_path(path);
	}
	return i < 0 ? NULL : bufs[i].lb;
}

/* One parsed file of the pre-applied diff onto its session buffer: each hunk's
 * pre-image is searched for rather than trusted at its line number, and the
 * buffer rebuilt around the post-image. A hunk whose pre-image is gone is a
 * hard error - dropping it would ship a compat patch nobody wrote. */
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
		sbuf_nul(sb)
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

/* Drop the tail of files[] from `first` on: the entries a scoped parse added
 * once whatever needed them is done with them. */
static void drop_files_from(int first)
{
	for (int i = first; i < nfiles; i++) {
		for (int j = 0; j < files[i].nops; j++)
			free(files[i].ops[j].text);
		free(files[i].ops);
		free(files[i].path);
		free(files[i].orig_path);
	}
	nfiles = first;
}

/* Keep the slots, drop what they patch. A range in the middle of files[] cannot
 * be removed - every later block's range is indexed by position - and a file
 * with no ops builds no groups, which is what every reader downstream tests. */
static void blank_files_range(int first, int count)
{
	for (int i = first; i < first + count && i < nfiles; i++) {
		for (int j = 0; j < files[i].nops; j++)
			free(files[i].ops[j].text);
		free(files[i].ops);
		files[i].ops = NULL;
		files[i].nops = files[i].ops_cap = 0;
	}
}

/* -C second positional, unified-diff form: parsed into its own files[] range and
 * raw sink (so the host === PATCH === stays byte-identical) and spliced into the
 * live buffers. The range is dropped right after - the diff is applied, not
 * shipped, and what the user is left holding is re-diffed from the baseline. */
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
	drop_files_from(first);
	free_lines(sink.v, sink.n);
	free(text);
	return st;
}

/* Forget every compat block that was read from a script, along with the
 * files[] range they own - they are parsed before the host patch is, so the
 * range is the head of files[] and dropping it leaves the array empty for the
 * host diff that follows. The blocks' edits are not undone: whatever their
 * identity gates let through during the replay stays in the buffers, so it is
 * re-derived as part of the host patch instead of as its own gated block. */
static void drop_compat_blocks(void)
{
	if (!ncompat)
		return;
	drop_files_from(compat_blocks[0].first);
	for (int c = 0; c < ncompat; c++) {
		compat_block_t *cb = &compat_blocks[c];
		free_lines(cb->raw.v, cb->raw.n);
		free(cb->origin);
	}
	ncompat = 0;
}

/* The src= label of this derivation: its origins in replay order, one src=
 * field each, so the label of a two-origin block reads "a.sh src=b.sh" and the
 * header carries "src=a.sh src=b.sh". The field is repeated rather than one
 * field with a separator in it because a separator would be a byte no origin
 * path may hold, and the only such byte here is the space the header already
 * parses on: a reader splits on whitespace it could not have kept anyway.
 * Annotation only - the reader keeps the label whole and nothing matches on it
 * - so a single origin still writes exactly its own path. */
static char *compat_origin_label(void)
{
	sbuf_smake(sb, SB_INIT)
	for (int i = 0; i < ncompat_origin; i++) {
		if (i)
			sbuf_str(sb, " src=")
		sbuf_str(sb, compat_origins[i])
	}
	sbufn_ret(sb, sb->s)
}

/* One diff over every buffer the session reshaped, in buffer order: one block,
 * one section, one storage region. A buffer with no baseline was opened after
 * the snapshot (the handover's own command line) and is not part of what is
 * being measured. Returns how many buffers moved. */
static int derive_diff(sbuf *diff)
{
	int nchanged = 0;
	for (int i = 0; i < xbufcur; i++) {
		char **pre, **base, **fin;
		char *basetext = NULL, *fintext, *bdup, *fdup;
		int npre, nbase, nfin, is_new;
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
		free(fintext); free(bdup); free(fdup);
		free(base); free(fin);
		free_lines(pre, npre);
	}
	return nchanged;
}

/* The one compat block this run produces: replay the origin and the target into
 * one session, hand it to the user, then measure every changed buffer from its
 * post-origin baseline to its final state and concatenate the results into a
 * single unified diff. That diff, over however many files it spans, IS the
 * compatibility patch. The block is stored, not emitted, and its bytes are
 * marked used so the script-global SEP/ESC chosen after this cover them. -1 on
 * any hard error (a nonzero handover status, a session that changed nothing),
 * and main() then writes nothing. */
static int compat_derive(void)
{
	int i, nsc = 0, nchanged = 0;
	int nor = ncompat_origin;
	const char **sc = emalloc((nor + 2) * sizeof(*sc));
	sbuf_smake(diff, SB_INIT)
	for (i = 0; i < nor; i++)
		sc[nsc++] = compat_origins[i];
	sc[nsc++] = input_file;
	/* A pre-applied resolution in script form is one more replay block, run
	 * after the baseline is taken; its diff form is spliced into the buffers
	 * at that same point. Either way the user edits on top of it. */
	if (compat_pre) {
		if ((compat_pre_script = compat_pre_isscript(compat_pre)) < 0) {
			free(sc);
			free(diff->s);
			return -1;
		}
		if (compat_pre_script)
			sc[nsc++] = compat_pre;
	}
	compat_capturing = 1;
	/* Replay the origins in order and then the target into one session so
	 * the new block derives on top of every block the target already
	 * carries; existing compat blocks stack in stored order (post-only, one
	 * group). The baseline is snapshotted after the target. */
	if (replay_scripts(sc, nsc, 1, nor, -1) != 0) {
		ed_free();
		free(sc);
		free(diff->s);
		return -1;
	}
	compat_capturing = 0;
	nchanged = derive_diff(diff);
	ed_free();
	sbuf_nul(diff)
	if (!nchanged) {
		fprintf(stderr, "no compat patch derived\n");
		free(diff->s);
		free(sc);
		return -1;
	}
	ARR_PUSH(compat_blocks, ncompat, compat_cap)
	compat_block_t *cb = &compat_blocks[ncompat++];
	cb->origin = compat_origin_label();
	/* the block's diff parses into a fresh files[] range and its own raw
	 * sink, so the host === PATCH === stays byte-identical */
	raw_sink = &cb->raw;
	parse_diff_reset();
	cb->first = nfiles;
	parse_diff_text(diff->s);
	cb->count = nfiles - cb->first;
	raw_sink = NULL;
	mark_bytes_used(diff->s);
	free(diff->s);
	free(sc);
	return 0;
}

/* -E <reg>: rebuild one stored compat block, in place.
 *
 * The block's own src= label names the stack it was derived in, so the origins
 * are looked for beside the target and replayed ahead of it: the identity
 * gates then fire exactly as the shell chain would make them, and the tree the
 * user is handed is the one the block is meant to repair. The target replays
 * with QF2=1 forced - a block being amended is a block expected to miss - and
 * with its own dispatch for this block lifted to the end, the baseline taken
 * in between. So the block's own edits, its misses as fail_report puts them
 * back, and whatever the user does on top are together the new diff.
 *
 * Nothing else in the script moves: the host patch, the sibling blocks and
 * this block's label and register are all stored, and re-emitted from storage.
 */
static int amend_derive(void)
{
	compat_block_t *cb;
	char **src = NULL, **own = NULL;
	const char **sc = NULL;
	int nsrc = 0, nsc = 0, dlen, i, blk, st = -1;
	sbuf_smake(diff, SB_INIT)
	blk = amend_sel - REG_FLAG_BASE;
	if (blk < 0 || blk >= ncompat) {
		fprintf(stderr, "%s: no compat block on register %d\n",
			input_file, amend_sel);
		free(diff->s);
		return -1;
	}
	cb = &compat_blocks[blk];
	nsrc = compat_src_fields(cb, &src);
	sc = emalloc((nsrc + 1) * sizeof(*sc));
	own = emalloc((nsrc + 1) * sizeof(*own));
	/* the label stores basenames, and a chain is one directory of scripts */
	dlen = base_name(input_file) - input_file;
	for (i = 0; i < nsrc; i++) {
		char *q = emalloc(dlen + strlen(src[i]) + 1);
		memcpy(q, input_file, dlen);
		strcpy(q + dlen, src[i]);
		own[nsc] = q;
		sc[nsc++] = q;
		if (access(q, R_OK) < 0) {
			fprintf(stderr, "%s: origin %s of block %d is not "
				"beside the script\n", input_file, q, amend_sel);
			goto out;
		}
	}
	sc[nsc++] = input_file;
	fprintf(stderr, "amend: replaying");
	for (i = 0; i < nsc; i++)
		fprintf(stderr, " %s", sc[i]);
	fprintf(stderr, "\n");
	if (replay_scripts(sc, nsc, 1, nsc - 1, amend_sel) != 0) {
		ed_free();
		goto out;
	}
	if (!derive_diff(diff)) {
		ed_free();
		fprintf(stderr, "no compat patch derived\n");
		goto out;
	}
	ed_free();
	sbuf_nul(diff)
	/* The rebuilt block takes the old one's place: same register, same
	 * label, same position in the run order. Its old files[] range stays
	 * where it is, emptied - the new one parses in at the end of the array,
	 * as a freshly derived block's does. */
	blank_files_range(cb->first, cb->count);
	free_lines(cb->raw.v, cb->raw.n);
	memset(&cb->raw, 0, sizeof(cb->raw));
	if (cb->deltas.n) {
		fprintf(stderr, "amend: block %d had stored customizations, "
			"which its new diff cannot be matched to: dropping "
			"them\n", amend_sel);
		memset(&cb->deltas, 0, sizeof(cb->deltas));
	}
	raw_sink = &cb->raw;
	parse_diff_reset();
	cb->first = nfiles;
	parse_diff_text(diff->s);
	cb->count = nfiles - cb->first;
	raw_sink = NULL;
	mark_bytes_used(diff->s);
	st = 0;
out:
	for (i = 0; i < nsrc; i++)
		free(src[i]);
	for (i = 0; i < nsc && i < nsrc; i++)
		free(own[i]);
	free(src);
	free(own);
	free(sc);
	free(diff->s);
	return st;
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

/* An editing session over args, its resulting unified diff written to out.
 * Every buffer the session leaves behind is diffed, not just the ones named
 * here: files reached with :e join the same diff, in the order they were
 * opened, and a path that does not exist yet is diffed as a creation.
 *
 * The session is nextvi's own main(), renamed nextvi_main() for this build, so
 * args is a nextvi command line - flags, files and EXINIT behave exactly as in
 * vi(1). Only the framing is patch2vi's: the terminal is claimed first because
 * stdout is the script, and the buffers are read back and freed after, which is
 * the whole point - nextvi_main() returns without touching them and nothing is
 * ever written to disk. Its process-wide bring-up doubles as ed_init()'s, so a
 * later session must not repeat it. */
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

/* -E: everything after the script name is a nextvi command line, kept for the
 * handed-over session - option letters as in vi(1), then files. */
static int parse_hand_args(char **args, int n)
{
	int i, j, vis = 0;
	for (i = 0; i < n && args[i][0] == '-'; i++) {
		if (args[i][1] == '-' && !args[i][2]) {
			i++;
			break;
		}
		for (j = 1; args[i][j]; j++) {
			if (args[i][j] == 's')
				vis |= 1|2;
			else if (args[i][j] == 'e')
				vis |= 2;
			else if (args[i][j] == 'm')
				vis |= 4;
			else if (args[i][j] == 'a')
				vis |= 8;
			else if (args[i][j] == 'v')
				vis = 0;
			else {
				fprintf(stderr, "Unknown editor option: -%c\n",
					args[i][j]);
				return -1;
			}
		}
		hand_vis = vis;
	}
	hand_files = args + i;
	nhand_files = n - i;
	return 0;
}

/* -o: the script goes to a named file instead of stdout. It is built in a temp
 * file beside it and renamed over it once the whole run succeeded, so a failure
 * anywhere - a refused replay, an unusable separator, a signal - leaves that
 * file untouched and no half-built script is ever visible under its name. That
 * is what makes "-o" naming the very script -E is updating safe: by the time
 * anything is written the input has been replayed, read and closed, whereas a
 * shell redirection onto it would have truncated it before patch2vi started. */
static const char *out_file;
static char *out_tmp;

/* The temporary twin's name is derived deterministically from the target's,
 * so nothing has to keep it from piling up: a run that dies without notice
 * (a signal, the editor's out-of-memory exits under -I) may leave
 * <path>.p2v.tmp behind, and the next -o path run reopens it with "w" and
 * truncates it away. What no run ever leaves is a half-built script under
 * the target's own name - only out_commit() puts one there, atomically. */
static void out_cleanup(void)
{
	if (out_tmp) {
		unlink(out_tmp);
		free(out_tmp);
		out_tmp = NULL;
	}
}

static int out_redirect(const char *path)
{
	struct stat st;
	out_tmp = str_fmt("%s.p2v.tmp", path);
	fflush(stdout);
	if (!freopen(out_tmp, "w", stdout)) {
		perror(out_tmp);
		return -1;
	}
	/* What is emitted is a script meant to be run: a file being replaced
	 * keeps its own mode, a file created here gets the executable bits the
	 * umask allows, so no caller has to chmod what patch2vi wrote. */
	if (!stat(path, &st)) {
		chmod(out_tmp, st.st_mode);
	} else {
		mode_t um = umask(0);
		umask(um);
		chmod(out_tmp, 0777 & ~um);
	}
	return 0;
}

static int out_commit(const char *path)
{
	if (fflush(stdout) || ferror(stdout)) {
		perror(out_tmp);
		out_cleanup();
		return -1;
	}
	if (rename(out_tmp, path) < 0) {
		perror(path);
		out_cleanup();
		return -1;
	}
	free(out_tmp);
	out_tmp = NULL;
	return 0;
}

/* -E: replay one generated script into a single session, hand it to the user,
 * and diff every buffer it leaves behind against disk. The replay is the same
 * one -C drives, so the blocks keep their own phase policy and nothing is
 * written; a block that fails aborts the whole thing, since a partial replay
 * would silently drop the hunks it never reached from the emitted script. */
static int amend_to_diff(const char *path, sbuf *out)
{
	const char *sc[1];
	int i;
	sc[0] = path;
	/* every buffer of the session ends up in the diff */
	xbufsalloc = MAX(64, xbufsalloc);
	if (replay_scripts(sc, 1, 1, -1, -1) != 0) {
		fprintf(stderr, "%s: replay failed, script left alone\n", path);
		ed_free();
		return -1;
	}
	for (i = 0; i < xbufcur; i++)
		if (bufs[i].path && bufs[i].path[0])
			buf_to_diff(out, bufs[i].path, bufs[i].lb);
	ed_free();
	return 0;
}

/* One line of unified diff, from a file, stdin or the built-in differ under -E;
 * consumed in place (chomped, and paths cut out of it). */
static int diff_in_hunk;	/* inside an @@ hunk */
static int diff_old_line;	/* the original line the next op sits at */

/* A parse always begins at a file header, so switching destinations (a compat
 * diff into its own files[] range) only has to clear what a mid-file header
 * would otherwise carry over. */
static void parse_diff_reset(void)
{
	diff_in_hunk = 0;
	diff_old_line = 0;
	pending_is_new = 0;
	free(pending_orig_path);
	pending_orig_path = NULL;
}

/* A "--- "/"+++ " header's path, in place: the a//b/ prefix dropped and the
 * trailing tab/space timestamp cut off. */
static char *diff_path(char *p)
{
	char *t;
	if (p[0] && p[1] == '/')
		p += 2;
	if ((t = strpbrk(p, "\t ")))
		*t = '\0';
	return p;
}

static void parse_diff_line(char *line)
{
	if (strncmp(line, "+++ ", 4) == 0) {
		new_file(diff_path(line + 4));
		diff_in_hunk = 0;
		return;
	}
	/* "--- /dev/null" means the next +++ creates the file; otherwise the
	 * path names the pre-patch content, which on disk is what the script
	 * runs against - the input to file-aware anchor validation. */
	if (strncmp(line, "--- ", 4) == 0) {
		char *p = line + 4;
		pending_is_new = strncmp(p, "/dev/null", 9) == 0
				 && (!p[9] || p[9] == '\t' || p[9] == ' ');
		free(pending_orig_path);
		pending_orig_path = pending_is_new ? NULL
				    : uc_dup(diff_path(p));
		return;
	}
	if (strncmp(line, "diff ", 5) == 0 || strncmp(line, "index ", 6) == 0)
		return;

	int os, oc;
	if (parse_hunk_header(line, &os, &oc)) {
		diff_in_hunk = 1;
		diff_old_line = os;
		cur_hunk_lo = os;
		cur_hunk_hi = oc > 0 ? os + oc - 1 : os;
		/* GNU diff -N marks a created file with the nonexistent
		 * path and an epoch timestamp instead of /dev/null, so
		 * detect it by its sole "@@ -0,0" hunk too; a later hunk
		 * addressing real lines means it existed after all. */
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
	if (line[0] == ' ' || line[0] == '-') {
		add_op(line[0] == ' ' ? 'c' : 'd', diff_old_line, line + 1);
		diff_old_line++;
	} else if (line[0] == '+')
		add_op('a', diff_old_line, line + 1);
	else if (line[0] != '\\')   /* not "\ No newline at end of file" */
		diff_in_hunk = 0;
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
 * A generated script's tail metadata in one left-to-right pass: the host
 * === DELTA === sections and every === PATCH2VI COMPAT === region (its own
 * DELTA sub-sections and its === COMPAT PATCH === diff). Regions nest one deep
 * and are fenced by === END COMPAT ===, never by a line count, so a hand-edit
 * that adds or drops a line still parses. Stops at === PATCH2VI PATCH ===,
 * leaving the host diff to the caller.
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
	int j, exit_found = 0;
	sbuf_smake(lb, SB_INIT)
	/* Skip until "exit 0" line; EOF first means the script was cut short
	 * and nothing past the cut can be trusted - refuse rather than
	 * regenerate an empty script over it */
	while ((line = read_line(in, lb))) {
		chomp(line);
		if (strcmp(line, "exit 0") == 0) {
			exit_found = 1;
			break;
		}
	}
	if (!exit_found) {
		free(lb->s);
		fprintf(stderr, "%s: not a patch2vi script (no exit 0)\n",
			input_file ? input_file : "<stdin>");
		return -1;
	}
	/* Read structured delta section */
	file_delta_t *cur_fd = NULL;
	grp_delta_t *cur_gd = NULL;
	int in_sect = GS_NONE;
	int pat_idx = 1; /* pattern[] slot for GS_PAT */
	int in_ph = 0;   /* 1/2 = inside a verbatim phase blob */
	/* Compat tail-region state, depth 1: cur_cb redirects DELTA
	 * sub-sections into the block's own array, in_compat_patch routes the
	 * block's diff into its own files[] range and raw sink. All closed by
	 * === END ===, the region by === END COMPAT ===. */
	compat_block_t *cur_cb = NULL;
	int in_compat_patch = 0;
	sbuf_smake(ph, SB_INIT)
	while (read_line(in, lb)) {
		line = chomp_sb(lb);
		/* A stored blob's bytes are marked used as it closes, so a
		 * changed patch cannot pick a SEP/ESC that collides with them. */
		if (in_ph) {
			int was = in_ph;
			in_ph = ph_capture(ph, line, cur_gd, in_ph);
			if (!in_ph && cur_gd)
				mark_verbatim_bytes(was == 1 ? cur_gd->ph1
						    : cur_gd->ph2,
						    cur_gd->ovr_esc,
						    cur_gd->ovr_sep);
			continue;
		}
		/* === COMPAT PATCH === body: raw diff lines, so a source
		 * line that looks like a section tag is harmless and only
		 * a column-0 === END === closes it. */
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
		if (strncmp(line, "=== PATCH2VI COMPAT ", 20) == 0) {
			if (cur_cb) {
				fprintf(stderr, "nested COMPAT region\n");
				return -1;
			}
			ARR_PUSH(compat_blocks, ncompat, compat_cap)
			cur_cb = &compat_blocks[ncompat++];
			/* "=== PATCH2VI COMPAT <reg> [src=<origin>...] ==="
			 * One src= field per origin, so the label of a
			 * multi-origin block is everything from the first one
			 * to the terminator, inner "src=" included: it is kept
			 * whole and only ever printed back. */
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
			/* a compat block's DELTA is always read, so the region
			 * round-trips; the host's only when re-applying */
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
				cur_gd = fd_add_grp(cur_fd, atoi(line + 10));
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

/*
 * The applied-set tail.
 *
 * The applied set is the chain of scripts already run, carried in $P2VI_PATCH
 * as basenames. A script inherits it from its caller, hands it to the editor
 * whole (REG_APPLIED, where emit_compat_flags decides every gate from it) and
 * appends itself before invoking the next script with the rest of the queue.
 * The set and the queue are disjoint: the environment grows in run order, the
 * arguments shrink. That is the whole shell side of compat - no header, no
 * flags, nothing to keep in step with the editor's own reading of the set.
 *
 * Only the argument test is a shell conditional; the body lines are ordinary
 * top-level commands, so the -e parser skips the whole block.
 */
static void emit_compat_tail(void)
{
	printf("\nif [ $# -gt 0 ]; then\n");
	printf("    export P2VI_PATCH=\"$P2VI_PATCH ${0##*/}\"\n");
	printf("    next=$1\n");
	printf("    shift\n");
	/* an argument that already carries a path is named as it stands,
	 * wherever ./ cannot reach it; only a bare name, which the shell would
	 * hunt down $PATH, is the one that has to be pinned to this directory */
	printf("    case $next in /*|./*|../*) \"$next\" \"$@\" ;;"
	       " *) \"./$next\" \"$@\" ;; esac\n");
	printf("fi\n");
}

/* The whole help, on stdout for -h (a request, answered with success) and on
 * stderr for a misused option (a diagnostic); err picks both the stream and
 * the exit status. */
static void usage(const char *prog, int err)
{
	FILE *f = err ? stderr : stdout;
	fprintf(f, "Usage: %s [-arih] [-d[N]] [-o FILE] [-er TAG] [-ew TAG]"
		" [input.patch] [nextvi-opts...]\n"
		"       %s -e script.sh [script2.sh...]\n"
		"       %s [-ari]I [nextvi-opts...]\n"
		"       %s [-ario]E script.sh [<reg>|''] [nextvi-opts...]\n"
		"       %s [-o]C origin.sh [-C origin2.sh...] target.sh"
		" [fix.[patch|sh]|''] [nextvi-opts...]\n",
		prog, prog, prog, prog, prog);
	fputs("Converts unified diff to shell script using nextvi ex commands\n"
	      "Input can be a unified diff or a previously generated patch2vi script\n"
	      "  -h    Show this help\n"
	      "  -a    Absolute line numbers\n"
	      "  -r    Relative regex patterns instead of line numbers\n"
	      "  -o    Write the script to FILE, atomically; may be a file this\n"
	      "        run reads. Clustered with another option it takes no FILE\n"
	      "        and updates that option's own script in place\n"
	      "  -e    Execute a script with the built-in nextvi, no shell involved\n"
	      "        Several scripts run in order, stopping at the first failure\n"
	      "  -i    Interactive: edit patterns and ex bodies in the built-in nextvi\n"
	      "        Rest of the line after the input patch is a nextvi command\n"
	      "        line for the session (none follows a stdin input)\n"
	      "  -d    Delta: re-apply previous customizations (implies -i)\n"
	      "  -d1   Delta: match by group index\n"
	      "  -d2   Delta: match by group index + deleted/inserted text or regex\n"
	      "  -d3   Delta: match by group index + entire hunk\n"
	      "  -d4   Delta: match by deleted/inserted text or regex\n"
	      "  -d5   Delta: match by entire hunk\n", f);
	fprintf(f, "  -er   Read section end tag (default: \"%s\")\n"
		"  -ew   Write section end tag (default: \"%s\")\n",
		end_tag_rd, end_tag_wr);
	fputs("  -E    Update a script: replay it, edit, re-emit it\n"
	      "        A compat block's flag register after the script rebuilds\n"
	      "        that one block instead, replaying its src= origins ahead of\n"
	      "        the target; '' skips the slot. Rest of the line is a nextvi\n"
	      "        command line; -d[N] keeps deltas\n"
	      "        With QF2=1 the hunks that missed are put back into the\n"
	      "        buffers at the line they reported, cursor parked on the first\n"
	      "  -I    Edit files in the built-in nextvi, emit the edits as a script\n"
	      "        Rest of the line is a nextvi command line, EXINIT included\n"
	      "  -C    Compat patch: resolve a collision with origin.sh, ship the\n"
	      "        fix as a block after the target's, behind an identity gate\n"
	      "        on origin being in $P2VI_PATCH; a second positional\n"
	      "        pre-applies a written fix to start from, '' skips it; the\n"
	      "        rest of the line is a nextvi command line for the handover\n"
	      "        Repeat -C to gate one block on several origins at once\n", f);
	exit(err ? 1 : 0);
}

/* A multi-letter option's argument, attached (-erTAG) or separate (-er TAG);
 * n is where the attached form starts, i.e. one past the option's last
 * letter (3 for -er/-ew, 2 for -C and 3 for the clustered -oC). */
static const char *opt_arg(int argc, char **argv, int *i, int n)
{
	if (argv[*i][n])
		return argv[*i] + n;
	if (*i + 1 < argc)
		return argv[++*i];
	fprintf(stderr, "Option -%.*s requires an argument\n",
		n - 1, argv[*i] + 1);
	usage(argv[0], 1);
	return NULL;
}

/* Is what follows a leading "-o" an option cluster naming -E or -d rather than
 * a file name? Only when it holds one of those and nothing but cluster letters,
 * so that "-oE" and "-od2" (and "-oEd2", "-od3E") mean "update the script in
 * place" while any ordinary -oFILE, even -oEDITED or -odelta.sh, still names a
 * file. Both modes read a script and emit one, so in place is what an author
 * means; the file literally named "d" is still reachable as "-o d". */
static int amend_cluster(const char *s)
{
	int k;
	if (!strchr(s, 'E') && !strchr(s, 'd'))
		return 0;
	for (k = 0; s[k]; k++)
		if (!strchr("ariIEod12345", s[k]))
			return 0;
	return 1;
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
			end_tag_rd = opt_arg(argc, argv, &i, 3);
			continue;
		}
		if (argv[i][1] == 'e' && argv[i][2] == 'w') {
			end_tag_wr = opt_arg(argc, argv, &i, 3);
			continue;
		}
		/* -C origin.sh: derive a compatibility patch against that
		 * script, applied AFTER the target (the ordinary positional
		 * input). Post-only.
		 *
		 * Repeatable: every -C adds one origin, and the block's
		 * identity gate tests all of them together, for a fix that only
		 * a particular stack of patches needs. The origins are their
		 * own arguments rather than positionals so the target and the
		 * optional pre-applied fix stay where they are, unambiguously.
		 *
		 * "-oC" clusters the top-level -o into it, as "-oE" does: no
		 * FILE of its own, the result lands back on the target script
		 * the block extends. A file literally named "C" is still
		 * reachable as "-o C". */
		j = argv[i][1] == 'o' && argv[i][2] == 'C';
		if (argv[i][1 + j] == 'C') {
			compat_mode = 1;
			read_deltas = 1;
			amend_inplace |= j;
			ARR_PUSH(compat_origins, ncompat_origin, compat_origin_cap)
			compat_origins[ncompat_origin++] =
				opt_arg(argc, argv, &i, 2 + j);
			continue;
		}
		/* -o FILE (or -oFILE): the script, wherever it comes from,
		 * lands in that file rather than on stdout; tested after -C
		 * so it cannot shadow it */
		if (argv[i][1] == 'o' && !amend_cluster(argv[i] + 2)) {
			if (argv[i][2])
				out_file = argv[i] + 2;
			else if (i + 1 < argc)
				out_file = argv[++i];
			else {
				fprintf(stderr, "Option -o requires an argument\n");
				usage(argv[0], 1);
			}
			continue;
		}
		/* bare -e: execute the script; tested after -er/-ew so it
		 * cannot shadow them, and kept out of the cluster loop
		 * whose letters are a r i h d E I */
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
			/* -I and -E both end patch2vi's own option parsing:
			 * whatever follows the cluster is a nextvi command
			 * line, options and files alike - for -E all but its
			 * first word, which names the script to update. Either
			 * way the script goes to stdout, as in every mode */
			else if (argv[i][j] == 'I')
				edit_mode = 1;
			else if (argv[i][j] == 'E')
				amend_mode = 1;
			/* -o inside an -E cluster takes no argument of its
			 * own: the script -E names is also the output, so
			 * -oE updates it in place */
			else if (argv[i][j] == 'o')
				amend_inplace = 1;
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
				usage(argv[0], 0);	/* asked for: stdout, ok */
			else {
				fprintf(stderr, "Unknown option: -%c\n", argv[i][j]);
				usage(argv[0], 1);
			}
		}
		if (edit_mode || amend_mode) {	/* the rest belongs to nextvi */
			i++;
			break;
		}
	}
	if (amend_inplace && !amend_mode && !compat_mode && !delta_mode) {
		fprintf(stderr, "Clustered -o is only for -E, -d and -C\n");
		usage(argv[0], 1);
	}
	if (i < argc && !edit_mode)
		input_file = argv[i];
	/* -i/-d take the editor's command line after the input positional,
	 * exactly as -E does after its script and -C after its fix slot: the
	 * positional anchors where patch2vi's own options end, so a stdin
	 * input has no anchor and no tail */
	if (interactive_mode && !exec_mode && !edit_mode && !amend_mode &&
			!compat_mode && i < argc &&
			parse_hand_args(argv + i + 1, argc - i - 1) < 0)
		usage(argv[0], 1);
	/* -od[N]: the delta run regenerates the script it read, so that is what
	 * it writes back (atomically, so reading it first is safe) */
	if (delta_mode && amend_inplace && !amend_mode && !compat_mode) {
		if (!input_file) {
			fprintf(stderr, "-od requires a script argument\n");
			return 1;
		}
		out_file = input_file;
	}
	/* -oC: the block extends the target script, so that is what the run
	 * writes back; the write is atomic, so reading it first is safe */
	if (compat_mode && amend_inplace) {
		if (!input_file) {
			fprintf(stderr, "-oC requires a target script\n");
			return 1;
		}
		out_file = input_file;
	}
	/* plain -C replays the target by name (compat_derive feeds its path
	 * to the same parser -E uses), so stdin cannot supply it */
	if (compat_mode && !amend_inplace && !input_file) {
		fprintf(stderr, "-C requires a target script\n");
		return 1;
	}
	/* -C takes a second positional: an already written compat fix, applied
	 * before the editor is handed over. An empty string skips the slot,
	 * which is how the handover's nextvi command line stays reachable
	 * without naming a fix. The rest parses as -E's does; its files are
	 * opened only at handover, after the baseline snapshot, so they never
	 * leak into the derived diff (the differ skips buffers it has no
	 * baseline for). */
	if (compat_mode && !edit_mode && !amend_mode) {
		char **ha = argv + i + 1;
		int nh = argc - i - 1;
		if (nh > 0) {
			if (ha[0][0])
				compat_pre = ha[0];
			ha++;
			nh--;
		}
		if (parse_hand_args(ha, nh) < 0)
			usage(argv[0], 1);
	}
	/* -E: the first word after the cluster is the script to update, read
	 * like any other input; then an optional block selector, then the
	 * editor's command line.
	 *
	 * The selector is a stored block's flag register, the number the
	 * "=== PATCH2VI COMPAT <reg>" header and the "# Compat <reg>" comment
	 * both carry. With it, that one block is what the run rebuilds and
	 * everything else in the script stands; without it -E updates the base
	 * patch as it always did. An empty word skips the slot, which is how
	 * the handover's nextvi command line stays reachable, exactly as -C's
	 * fix slot does. */
	if (amend_mode) {
		char **ha = argv + i + 1;
		int nh = argc - i - 1;
		if (i >= argc) {
			fprintf(stderr, "-E requires a script argument\n");
			return 1;
		}
		if (amend_inplace)
			out_file = argv[i];
		if (nh > 0 && !ha[0][strspn(ha[0], "0123456789")]) {
			if (ha[0][0]) {
				amend_sel = atoi(ha[0]);
				/* a script carrying blocks is search anchored
				 * throughout: an absolute edit next to a gated
				 * section would clobber a line by number in a
				 * tree the gate just reshaped */
				relative_mode = 1;
			}
			ha++;
			nh--;
		}
		if (parse_hand_args(ha, nh) < 0)
			usage(argv[0], 1);
	}
	/* the tail's mode flags govern the loads too, not just the session:
	 * "-m" must already silence the line every ed_loadbuf prints, so they
	 * cannot wait for ed_serve to apply them at entry. The replay's own
	 * per-block "xvis |= 2" stays on top of whatever lands here; ed_serve
	 * resets to the flags verbatim once the bodies are done. */
	if (hand_vis >= 0)
		xvis = hand_vis;

	/* Mark chars that cannot be ex separators. */
	static const char *forbidden =
		" \t0123456789+-.,<>/$';%*#|" /* ex range syntax */
		"@&!?bpaefidgmqrwusxycjtohlv=" /* ex commands */
		":\"\\`\n\r";                  /* default sep, shell quote/escape/backtick, newline */
	for (const char *p = forbidden; *p; p++)
		byte_used[(unsigned char)*p] = 1;

	if (relative_mode || interactive_mode || compat_mode)
		mark_bytes_used("FAIL OK");

	/* -I: the diff is not read, it is made. Everything patch2vi's own
	 * option loop did not consume is a nextvi command line - flags after
	 * "--", then files (a missing one counts as a creation) - and the
	 * buffers that session leaves behind are diffed against their disk
	 * copies, that diff going through the parser in place of an input
	 * stream. The script itself goes to stdout, like every other mode. */
	sbuf_smake(dsb, SB_INIT)
	if (edit_mode) {
		if (i >= argc) {
			fprintf(stderr, "-I requires a file argument\n");
			return 1;
		}
		if (edit_to_diff(argv + i, argc - i, dsb) < 0)
			return 1;
		sbuf_nul(dsb)
		parse_diff_text(dsb->s);
	}

	/* -e: no conversion, just run the script through the embedded editor
	 * and report the status the shell would have reported. Every remaining
	 * positional is a script of its own, run in the order given and each in
	 * its own editor lifetime, the first failure stopping the run: what
	 * "./a.sh && ./b.sh" does, without a shell or a process per script. */
	if (exec_mode) {
		if (!input_file) {
			fprintf(stderr, "-e requires a script argument\n");
			return 1;
		}
		int first = i;	/* the first script argument */
		for (; i < argc; i++) {
			/* the scripts already run are this one's applied set, the
			 * chain order -e shares with the shell */
			cur_applied_set((const char **)argv + first, i - first);
			FILE *f = fopen(argv[i], "r");
			if (!f) {
				perror(argv[i]);
				return 1;
			}
			exec_script = argv[i];
			j = exec_p2vi_script(f);
			fclose(f);
			if (j)
				return j;
		}
		cur_applied_free();
		return 0;
	}

	FILE *in = edit_mode ? NULL : stdin;
	if (input_file && !edit_mode) {
		in = fopen(input_file, "r");
		if (!in) {
			perror(input_file);
			return 1;
		}
	}

	/* Detect if input is a previously generated patch2vi script */
	sbuf_smake(lb, SB_INIT)
	if (in && read_line(in, lb)) {
		if (!strncmp(lb->s, "#!/bin/sh", 9)) {
			if (read_delta_sections(in) < 0)
				return 1;
		} else if (amend_mode) {
			fprintf(stderr, "%s: not a patch2vi script\n", input_file);
			return 1;
		} else {
			/* Not a script; store and process this first line */
			add_raw(lb->s);
			chomp(lb->s);
			parse_diff_line(lb->s);
		}
	}
	/* -E: the delta sections are read as under -d, but the old patch
	 * section is not - the new one is what the session produces, over the
	 * files as they are on disk. Close before the loop below reads it. */
	if (amend_mode && amend_sel < 0) {
		if (in)
			fclose(in);
		in = NULL;
		/* Compat blocks cannot round-trip: they are derived against
		 * an origin script this run knows nothing about. Discarding
		 * them beats refusing the update - the replay still runs them,
		 * so their effect survives folded into the host patch, only
		 * their gating is lost. Naming one (-E script.sh <reg>) is the
		 * other way round: that block is rebuilt and the base patch is
		 * the part that stands, read below like any stored region. */
		if (ncompat) {
			fprintf(stderr, "%s: script carries %d compat block%s, "
				"which -E cannot round-trip: discarding them, "
				"their edits fold into the emitted patch\n",
				input_file, ncompat, ncompat > 1 ? "s" : "");
			drop_compat_blocks();
		}
		if (amend_to_diff(input_file, dsb) < 0)
			return 1;
		sbuf_nul(dsb)
		parse_diff_text(dsb->s);
	}
	while (in && read_line(in, lb)) {
		add_raw(lb->s);
		chomp(lb->s);
		parse_diff_line(lb->s);
	}
	free(lb->s);

	if (in && in != stdin)
		fclose(in);

	/* -E <reg>: the host patch has just been read out of the script and
	 * every other stored region is in hand, so all this replaces is the
	 * named block's own diff. Same window as -C below, one block down. */
	if (amend_mode && amend_sel >= 0) {
		if (amend_derive() < 0)
			return 1;
		relative_mode = 1;
	}

	/* -C: replay the origin script in one session and hand the
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

	/* -o: from here on stdout is the output file's temp twin. Every mode
	 * that emits a script passes through this point, and everything any of
	 * them reads - the patch, the script's delta sections, the files a
	 * replay or an -I session opened - has been read by now, so -o may name
	 * a file the same run consumed (-E updating its own script). */
	if (out_file && out_redirect(out_file) < 0)
		return 1;

	/* Emit shell script header; the emit layer targets sbufs, so build
	 * stdout pieces in one scratch sbuf and flush it after each use */
	sbuf_smake(osb, SB_INIT)
	fputs("#!/bin/sh -e\n# Generated by patch2vi from unified diff\n", stdout);
	list_unused_bytes(osb);
	sbuf_nul(osb)
	fputs(osb->s, stdout);
	fputs("\nVI=${VI:-vi}\n"
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
	 * every compat block share one process so the flags cross the host body
	 * through registers. Without them the common case stays a
	 * single host block, emitted byte-identically as before. */
	if (ncompat) {
		emit_one_call(active, nactive);
	} else if (nactive > 0) {
		/* A large body overflows EXINIT/argv, so the $VI invocation stages
		 * its ex command body in a temp file the shell expands. */
		fputs("# Body too large for EXINIT/argv: stage it in a file\n"
		      "( : > /tmp/p2vi.$$ ) 2>/dev/null && P2VIF=/tmp/p2vi.$$ || P2VIF=./p2vi.$$\n"
		      "trap 'rm -f \"$P2VIF\"' EXIT\n\n", stdout);
		fputs("# Patch:", stdout);
		for (int k = 0; k < nactive; k++)
			fprintf(stdout, " %s", active[k]->path);
		fputc('\n', stdout);
		emit_vi_block(active, nactive);
	}

	/* The chaining tail: if arguments remain, append this script to the
	 * inherited applied set and invoke the next script with the rest. */
	emit_compat_tail();

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
	/* the script is whole: put it under the name -o asked for */
	if (out_tmp && out_commit(out_file) < 0)
		return 1;
	return 0;
}
