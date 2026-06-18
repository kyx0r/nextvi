/*
 * patch2vi - Convert unified diff patches to shell scripts using Nextvi ex commands
 *
 * Usage: patch2vi [input.patch]
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include "vi.h"
#include "common.c"
#include "uc.c"
#include "regex.c"

#define MAX_LINE 8192
#define MAX_OPS 65536

typedef struct {
	int type;       /* 'd'=delete, 'a'=add, 'c'=context */
	int oline;      /* line number in original file */
	char *text;     /* line content (for add operations) */
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
static const char *end_tag_rd = "=== END ===";
static const char *end_tag_wr = "=== END ===";

/* Number of f> anchor search strategies (SEARCH PATTERN slots), tried
 * strict-to-loose with first match wins. See default_pat_lines() for the
 * per-slot pattern composition. NFUZZ extra slots hold file-validated
 * relaxed (fuzzed) variants generated after the exact strategies; NSEARCH
 * is the total SEARCH PATTERN capacity per group. */
#define NPAT 5
#define NFUZZ 4   /* max file-validated fuzzed candidates per group */
#define NSEARCH (NPAT + NFUZZ)   /* must stay <= 9: section numbers are 1 digit */

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
	char **pattern[NSEARCH];  /* SEARCH PATTERN 1-NPAT fallbacks */
	int npattern[NSEARCH], pat_cap[NSEARCH];
	int pat_off[NSEARCH];      /* per-pattern OFFSET marker value */
	int pat_has_off[NSEARCH];
	int pat_mode[NSEARCH];     /* per-pattern MODE: 0 = %;f>, 1 = .,$f> */
	int pat_has_mode[NSEARCH];
	char **abs_cmd;
	int nabs, abs_cap;
	char **rel_cmd;
	int nrel, rel_cap;
	char **relc_cmd;
	int nrelc, relc_cap;
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

static char *xstrdup(const char *s)
{
	char *p = strdup(s);
	if (!p) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return p;
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
	int extra = 0;
	for (const char *p = s; *p; p++)
		if (strchr(set, *p))
			extra++;
	char *result = emalloc(strlen(s) + extra + 1);
	char *dst = result;
	for (const char *p = s; *p; p++) {
		if (strchr(set, *p))
			*dst++ = '\\';
		*dst++ = *p;
	}
	*dst = '\0';
	return result;
}

#define REGEX_META "\\^$.*+?[](){|"
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
	(*arr)[(*n)++] = xstrdup(s);
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
 * File-aware anchor validation.
 *
 * patch2vi normally compiles blind: it sees only the diff's context, so it
 * emits a strict-to-loose fallback chain and lets nextvi resolve the anchor
 * at apply time. When the pre-patch original is readable on disk (it usually
 * is, since the generated script applies in the same working tree), we can do
 * better: count how many times each candidate anchor window actually occurs
 * in the file and which occurrence is the right one, then sort the proven
 * unique anchor to the front of the chain. We still emit anchor searches (not
 * absolute line numbers) plus the full fallback chain, so the script stays
 * portable and drift-tolerant; file access only improves the ordering.
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

/* Uniqueness bonuses dominate any specificity score so a proven anchor always
 * sorts ahead of an unproven one of the same shape. */
#define UNIQUE_BONUS (1 << 28)   /* exactly one match, at the right place */
#define CORRECT_BONUS (1 << 27)  /* first match is right, but not unique */

/*
 * File-validated fuzzed (relaxed) anchors.
 *
 * When the original file is readable we can safely *relax* an exact anchor into
 * a regex that tolerates small drift, because we can verify the relaxed form
 * still resolves to exactly one place - the right one - in the file. A fuzzed
 * window replaces selected runes of each line with '.', the nextvi wildcard
 * (one rune; it never has to cross a line here because the literal newlines
 * between lines pin the structure). The relaxation is length-preserving: each
 * '.' stands for exactly one rune, so a fuzzed line still matches only lines of
 * the same rune length - it tolerates in-place character drift (a renamed,
 * equal-length token, a changed digit) but nothing else. We keep a fuzzed
 * candidate only when it still matches uniquely at the expected location, so an
 * over-relaxed, ambiguous pattern is dropped rather than emitted. Without the
 * file we emit no fuzzed anchors at all (they rely on full-file inspection).
 */

/* Byte length of the UTF-8 rune starting at s (>= 1). */
static int rune_len(const char *s)
{
	unsigned char c = (unsigned char)*s;
	if (c < 0x80) return 1;
	if ((c >> 5) == 0x6) return 2;
	if ((c >> 4) == 0xe) return 3;
	if ((c >> 3) == 0x1e) return 4;
	return 1;
}

static int rune_count(const char *s)
{
	int n = 0;
	while (*s) { s += rune_len(s); n++; }
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
		int ol = rune_len(o), bl = rune_len(b);
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
			int bl = rune_len(b);
			if (!win[j].mask[i]) {
				run += bl;
			} else {
				if (run > SPEC_LINE_CAP) run = SPEC_LINE_CAP;
				s += (long)run * run;
				run = 0;
			}
			b += bl;
		}
		if (run > SPEC_LINE_CAP) run = SPEC_LINE_CAP;
		s += (long)run * run;
		if (s > SPEC_MAX) { s = SPEC_MAX; break; }
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
		int bl = rune_len(b);
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

/* Fill mask[0..nrune) for fuzz level lvl over a window-global rune index that
 * starts at *gi (advanced by nrune). Level 0/1 drop odd/even runes; levels >= 2
 * drop by an increasing content-seeded threshold (looser each step). At least
 * one rune is always kept literal. */
static void fuzz_mask(unsigned char *mask, const char *base, int nrune,
		      int lvl, unsigned seed, int *gi)
{
	int thr = lvl >= 2 ? 400 + (lvl - 2) * 100 : 0;
	int kept = 0;
	for (int i = 0; i < nrune; i++) {
		int g = (*gi)++;
		int drop;
		if (lvl == 0)
			drop = (g & 1) == 1;
		else if (lvl == 1)
			drop = (g & 1) == 0;
		else
			drop = (int)(hash_pos(seed, g) % 1000) < thr;
		mask[i] = drop ? 1 : 0;
		if (!drop)
			kept++;
	}
	if (!kept && nrune > 0)
		mask[0] = 0;  /* never wildcard an entire line away */
	(void)base;
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
	if (!lines || nlines == 0)
		return xstrdup("");
	size_t len = 0;
	for (int i = 0; i < nlines; i++)
		len += strlen(lines[i]) + 1;
	char *result = emalloc(len ? len : 1);
	char *p = result;
	for (int i = 0; i < nlines; i++) {
		int slen = strlen(lines[i]);
		memcpy(p, lines[i], slen);
		p += slen;
		*p++ = i < nlines - 1 ? '\n' : '\0';
	}
	if (nlines == 0)
		*result = '\0';
	return result;
}

/* Build the default display text (as it appears in the temp file) from patch del/add lines.
 * Returns e.g. "-line1\n-line2\n+line3\n+line4\n" */
static char *build_default_text(char **del, int ndel, char **add, int nadd)
{
	size_t len = 0;
	for (int i = 0; i < ndel; i++)
		len += strlen(del[i]) + 2;
	for (int i = 0; i < nadd; i++)
		len += strlen(add[i]) + 2;
	char *result = emalloc(len + 1);
	char *p = result;
	for (int i = 0; i < ndel; i++) {
		*p++ = '-';
		int slen = strlen(del[i]);
		memcpy(p, del[i], slen);
		p += slen;
		*p++ = '\n';
	}
	for (int i = 0; i < nadd; i++) {
		*p++ = '+';
		int slen = strlen(add[i]);
		memcpy(p, add[i], slen);
		p += slen;
		*p++ = '\n';
	}
	*p = '\0';
	return result;
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

/* Get UTF-8 character position for byte offset */
static int utf8_char_offset(const char *s, int bytepos)
{
	int chars = 0;
	for (int i = 0; i < bytepos && s[i]; i++)
		if ((s[i] & 0xC0) != 0x80)
			chars++;
	return chars;
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
 * Sets *start to first differing UTF-8 char position,
 * *old_end to end position in old string,
 * *new_text to the replacement text (allocated).
 */
static int find_line_diff(const char *old, const char *new,
			  char **old_text, char **new_text)
{
	int old_len = strlen(old);
	int new_len = strlen(new);

	/* Find common prefix (in bytes) */
	int prefix = 0;
	while (old[prefix] && new[prefix] && old[prefix] == new[prefix])
		prefix++;

	/* Find common suffix (in bytes), but don't overlap with prefix */
	int suffix = 0;
	while (suffix < old_len - prefix && suffix < new_len - prefix &&
	       old[old_len - 1 - suffix] == new[new_len - 1 - suffix])
		suffix++;

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
static void list_unused_bytes(FILE *out)
{
	int n = 0;
	fprintf(out, "# Available separators:");
	int range_start = -1;
	for (int c = 1; c <= 256; c++) {
		int unused = (c < 256) && !byte_used[c];
		if (unused && range_start < 0) {
			range_start = c;
		} else if (!unused && range_start >= 0) {
			int range_end = c - 1;
			if (range_end == range_start)
				fprintf(out, " 0%03o", range_start);
			else
				fprintf(out, " 0%03o-0%03o", range_start, range_end);
			n++;
			range_start = -1;
		}
	}
	if (!n)
		fprintf(out, " (none)");
	fputc('\n', out);
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
static void emit_escaped_line(FILE *out, const char *s)
{
	for (; *s; s++) {
		unsigned char c = *s;
		/* the dynamic escape byte never occurs in content; emit it
		 * as the readable ${ESC} expansion */
		if (dyn_esc && c == (unsigned char)dyn_esc) {
			fputs("${ESC}", out);
			continue;
		}
		/* Shell double-quote escapes: $, `, ", \ */
		if (c == '\\' || c == '$' || c == '`' || c == '"') {
			fputc('\\', out);
		}
		fputc(c, out);
	}
}

static void emit_escaped_text(FILE *out, const char *s);

/* separator: shell expands ${SEP} in double-quoted EXINIT */
#define EMIT_SEP(out) fputs("${SEP}", out)
/* escaped separator inside ??! block: <esc><sep> for ex_arg */
#define EMIT_ESCSEP(out) \
	fputs(dyn_esc ? "${ESC}${SEP}" : "\\\\${SEP}", out)
/* triply-escaped separator inside a ?? then-arg nested in a ? cond:
 * <esc><esc><esc><sep> */
#define EMIT_ESC3SEP(out) \
	fputs(dyn_esc ? "${ESC}${ESC}${ESC}${SEP}" : "\\\\\\\\\\\\${SEP}", out)

/*
 * Ex commands emitted by patch2vi and their default range (no address given):
 *
 * All commands default to the current line (xrow) when no range is given,
 * per ex_region(): beg = xrow, end = xrow+1 when vaddr == 0.
 *
 * Commands that advance xrow (and thus affect subsequent relative addresses):
 *   a (append)     - ec_insert: xrow = beg + inserted_lines - 1
 *   c (change)     - ec_insert: xrow = end + inserted_lines - deleted - 1
 *   d (delete)     - ec_delete: xrow = beg (or last line if past end)
 *   f>/f+/f- (find)- ec_find: xrow = matched line, xoff = match position
 *   (bare address) - ec_print (!*cmd && *loc): xrow = end - 1
 *                    This is how +N / -N move xrow without a command.
 *
 * Commands that do NOT advance xrow:
 *   s (substitute) - ec_substitute: does not modify xrow/xoff
 *   p (print)      - ec_print: xrow = end - 1 (but only used for debug)
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
static void emit_content(FILE *out, char **texts, int ntexts)
{
	for (int i = 0; i < ntexts; i++) {
		emit_escaped_text(out, texts[i]);
		fputc('\n', out);
	}
}

/* Emit ex commands for inserting text after line N.
 * New files have an empty buffer with no addressable line, so the
 * insert is emitted bare ("i"); otherwise always numbered ("1i"). */
static void emit_insert_after(FILE *out, int line, char **texts, int ntexts,
			      int is_new)
{
	if (ntexts == 0)
		return;

	if (is_new)
		fprintf(out, "i ");
	else if (line <= 0)
		fprintf(out, "1i ");
	else
		fprintf(out, "%da ", line);
	emit_content(out, texts, ntexts);
	EMIT_SEP(out);
}

/* Emit ex commands for deleting lines from N to M inclusive */
static void emit_delete(FILE *out, int from, int to)
{
	if (from == to)
		fprintf(out, "%dd", from);
	else
		fprintf(out, "%d,%dd", from, to);
	EMIT_SEP(out);
}

/* Emit ex command for horizontal (character-level) edit */
static void emit_horizontal_change(FILE *out, int line, int char_start,
				   int char_end,
				   const char *new_text)
{
	if (!*new_text) {
		if (char_start == char_end)
			return;
		fprintf(out, "%d;%d;%dd", line, char_start, char_end);
	} else if (char_start == char_end) {
		fprintf(out, "%d;%dc ", line, char_start);
		emit_escaped_text(out, new_text);
	} else {
		fprintf(out, "%d;%d;%dc ", line, char_start, char_end);
		emit_escaped_text(out, new_text);
	}
	EMIT_SEP(out);
}

/* Emit ex commands for changing lines (delete and insert) */
static void emit_change(FILE *out, int from, int to, char **texts, int ntexts)
{
	if (ntexts == 0) {
		emit_delete(out, from, to);
		return;
	}

	if (from == to)
		fprintf(out, "%dc ", from);
	else
		fprintf(out, "%d,%dc ", from, to);
	emit_content(out, texts, ntexts);
	EMIT_SEP(out);
}

/*
 * Relative mode emit functions - use regex patterns instead of line numbers.
 *
 * Two-phase emission per file:
 *
 * Phase 1 (resolve): the whole buffer is yanked once into register <b>
 * (the default register, set by 98reg) right after the file is opened.
 * All groups' searches then run top-to-bottom against this cache with
 * no edits in between, so the register stays byte-identical to the
 * buffer for the entire phase. Each group's target line is recorded
 * with a line mark ("+<off>m <id>", ids count up from 0 skipping
 * nextvi's special mark ids). Each group gets up to NPAT fallback
 * patterns tried strict-to-loose, first match wins (see
 * emit_fallback_chain and default_pat_lines): 1 = whole hunk (pre ctx +
 * deleted lines + post ctx), 2 = deleted lines + post ctx, 3 = top
 * context anchors only, 4 = deleted lines only, 5 = post ctx only.
 * Searches use %;f> (first search of a file) / %;f+ (subsequent)
 * against the register cache (a bare ";" resolves to the current
 * xoff, so each search continues one char past the previous match
 * start). When only one pattern survives dedup, single-line patterns
 * search the buffer directly with ";0 0reg .,$f> ^pattern$ .. 98reg"
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
static void emit_err_check_loc(FILE *out, const char *loc, int phase,
			       const char *tags)
{
	if (tags)
		fputs(tags, out);
	fprintf(out, "?" "?!${DBG%d:-", phase);
	fprintf(out, "ya!112");
	EMIT_ESCSEP(out);
	fprintf(out, "prp");
	EMIT_ESCSEP(out);
	fprintf(out, "p FAIL %s", loc);
	EMIT_ESCSEP(out);
	fprintf(out, "pr");
	fputs("${INTR}", out);
	fprintf(out, "${QF%d}}", phase);
	EMIT_SEP(out);
}

/* Phase-1 error check: FAIL <path>:<line> */
static void emit_err_check(FILE *out, int line)
{
	char loc[MAX_LINE];
	snprintf(loc, sizeof(loc), "%s:%d",
		 cur_file_path ? cur_file_path : "?", line);
	emit_err_check_loc(out, loc, 1, NULL);
}

/* Phase-1 fallback chain check: one <0;1;..>??! over all capture tags
 * (DNF OR); the inverted branch fires only when every pattern's
 * capture recorded a failure. */
static void emit_err_check_pats(FILE *out, int ntags, int line)
{
	char loc[MAX_LINE];
	char tags[(NPAT + NFUZZ) * 8];
	int p = 0;
	for (int t = 0; t < ntags && p < (int)sizeof(tags); t++)
		p += snprintf(tags + p, sizeof(tags) - p,
			      t ? ";%d" : "%d", t);
	snprintf(loc, sizeof(loc), "%s:%d",
		 cur_file_path ? cur_file_path : "?", line);
	emit_err_check_loc(out, loc, 1, tags);
}

/* Phase-2 error check: FAIL <path>:<line>:m<id> (mark id of the edited
 * group). mark_id < 0 means no mark (new-file insert, custom abs command). */
static void emit_err_check_mark(FILE *out, int line, int mark_id)
{
	char loc[MAX_LINE];
	char mark[16] = "m";
	if (mark_id >= 0)
		snprintf(mark, sizeof(mark), "m%d", mark_id);
	snprintf(loc, sizeof(loc), "%s:%d:%s",
		 cur_file_path ? cur_file_path : "", line, mark);
	emit_err_check_loc(out, loc, 2, NULL);
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
static void emit_escaped_text(FILE *out, const char *s)
{
	char *exarg_esc = escape_exarg(s);
	emit_escaped_line(out, exarg_esc);
	free(exarg_esc);
}

/* Emit f> search with error check, then mark the target line.
 * Single-line patterns search the buffer directly (0reg .. 98reg)
 * from the cursor's line: ";0" first resets xoff to the line start,
 * then ".,$f> ^pattern$" - the ^...$ anchors plus the .,$ range
 * disambiguate repeated text. The first search of a file uses
 * .,$f>, subsequent ones .,$f+.
 * Multi-line patterns run against the cached default register via
 * %;f> (first search of a file) or %;f+ (subsequent: a bare ";"
 * picks up the current xoff and skips one char from the previous
 * match start so identical anchors find the next occurrence).
 * pre_escaped: 0 = anchors are raw text (apply regex+exarg escape),
 *              1 = anchors are pre-escaped regex (apply exarg only).
 * mode: 1 = direct buffer search (.,$f>, 0reg/98reg, ^...$ anchors);
 *       0 = register-cache search (%;f>). Defaults to 1 for single-line
 *       patterns, 0 otherwise; the OFFSET MODE marker can override.
 * After the search, "+<offset>m <mark_id>" marks the target line
 * without moving the cursor. */
static void emit_search(FILE *out, char **anchors, int nanchors,
			int offset, int mark_id,
			int target_line, int pre_escaped, int first, int mode)
{
	int single = mode == 1;
	if (single) {
		/* reset xoff to 0 so the .,$ region starts at the
		 * current line's first column */
		fputs(";0", out);
		EMIT_SEP(out);
		fputs("0reg", out);
		EMIT_SEP(out);
		fputs(first ? ".,\\$f> " : ".,\\$f+ ", out);
		/* pre-escaped (interactive) patterns carry their own ^...$
		 * from the displayed default; the user may have removed them */
		if (!pre_escaped)
			fputc('^', out);
	} else
		fputs(first ? "%;f> " : "%;f+ ", out);
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
			fputc('\n', out);
	}
	if (single && !pre_escaped)
		fputs("\\$", out);  /* $ anchor, shell-escaped */
	/* Ensure trailing newline when last anchor is empty */
	if (nanchors > 0 && !anchors[nanchors - 1][0])
		fputc('\n', out);
	EMIT_SEP(out);
	emit_err_check(out, target_line);
	if (single) {
		fputs("98reg", out);
		EMIT_SEP(out);
	}
	fputs("${LB}\n", out);
	EMIT_SEP(out);
	if (offset)
		fprintf(out, "%+d", offset);
	fprintf(out, "m %d", mark_id);
	EMIT_SEP(out);
}

/* Next mark id, skipping nextvi's internal special mark ids:
 * <'> 39 <*> 42 <[> 91 <]> 93 <`> 96 are rewritten by the editor
 * itself (<*> on every ex command, <[>/<]> on every change). */
static int next_mark_id(int *n)
{
	while (*n == '\'' || *n == '*' || *n == '[' || *n == ']' || *n == '`')
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
	/* Edited SEARCH PATTERN 1-NPAT sections (pre-escaped regex) */
	char **custom_pat[NSEARCH];
	int ncustom_pat[NSEARCH];
	int custom_pat_off[NSEARCH];     /* per-section +N first-line override */
	int custom_pat_has_off[NSEARCH];
	int custom_pat_mode[NSEARCH];    /* per-section MODE override (0/1) */
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
	/* Two-phase emission state, set in phase 1, read in phase 2 */
	int res_strat;           /* resolved strategy */
	int mark_id;             /* line mark id, -1 = no mark */
	int insert_i;            /* pure add: use i (insert) instead of a */
} group_t;

/* Emit a line with exarg + shell escaping only (no regex escaping).
 * Used for user-edited regex lines in interactive mode. */
static void emit_escaped_exarg_only(FILE *out, const char *s)
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
	int mode;         /* search mode: 0 = %;f> (register), 1 = .,$f> (buffer) */
	int score;        /* ordering key: specificity (+ uniqueness bonus) */
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
	int mode;       /* 0 = %;f>, 1 = .,$f> */
	int score;      /* ordering key (specificity + UNIQUE_BONUS) */
} fuzzwin_t;

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
	int nf = 0;
	for (int lvl = 0; nf < max && lvl <= NFUZZ + 4; lvl++) {
		int any = 0, gi = 0;
		for (int j = 0; j < bn; j++) {
			int nr = rune_count(base[j]);
			unsigned char *m = emalloc(nr ? nr : 1);
			fuzz_mask(m, base[j], nr, lvl, seed, &gi);
			for (int k = 0; k < nr; k++)
				if (m[k]) any = 1;
			win[j].base = base[j];
			win[j].mask = m;
			win[j].nrune = nr;
		}
		int first, cnt = any ? count_window_fuzzy(win, bn, &first) : 0;
		if (any && cnt == 1 && first == expected) {
			char **lines = emalloc(bn * sizeof(char *));
			for (int j = 0; j < bn; j++)
				lines[j] = fuzzy_regex(&win[j]);
			int dup = 0;
			for (int p = 0; p < nf; p++)
				if (lines_equal(lines, bn, out[p].lines,
						out[p].nlines)) {
					dup = 1;
					break;
				}
			if (dup) {
				for (int j = 0; j < bn; j++)
					free(lines[j]);
				free(lines);
			} else {
				out[nf].lines = lines;
				out[nf].nlines = bn;
				out[nf].offset = doff0;
				out[nf].mode = bn == 1 ? 1 : 0;
				out[nf].score = fuzzy_spec(win, bn) + UNIQUE_BONUS;
				nf++;
			}
		}
		for (int j = 0; j < bn; j++)
			free(win[j].mask);
	}
	free(win);
	free(base);
	return nf;
}

static void free_fuzz_windows(fuzzwin_t *w, int n)
{
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < w[i].nlines; j++)
			free(w[i].lines[j]);
		free(w[i].lines);
	}
}

/* Emit one fallback pattern as the f> argument inside a ? conditional.
 * The conditional nesting consumes one more escape layer than a
 * top-level search: with the default backslash escape, every backslash
 * is doubled again for the extra ex_arg layer. The ? conditional no
 * longer delimiter-scans its argument, so literal/quantifier ? pass
 * through untouched. With a dynamic escape, backslash is not special to
 * ex_arg, so the regex needs no extra escaping at all. */
static void emit_chain_pattern(FILE *out, pat_spec_t *p)
{
	int wrap = p->nlines == 1 && !p->pre_escaped;
	if (wrap)
		fputc('^', out);
	for (int i = 0; i < p->nlines; i++) {
		char *r = p->pre_escaped ? NULL : escape_regex(p->lines[i]);
		char *x;
		if (dyn_esc) {
			x = xstrdup(r ? r : p->lines[i]);
		} else {
			char *e = escape_exarg(r ? r : p->lines[i]);
			x = escape_chars(e, "\\");
			free(e);
		}
		emit_escaped_line(out, x);
		free(x);
		free(r);
		if (i < p->nlines - 1)
			fputc('\n', out);
	}
	if (wrap)
		fputs("\\$", out);  /* $ anchor, shell-escaped */
	/* Ensure trailing newline when last line is empty */
	if (p->nlines > 0 && !p->lines[p->nlines - 1][0])
		fputc('\n', out);
}

/* Phase 1 fallback chain: try each pattern in order, first match wins.
 * All attempts are nested into a single ? conditional, chained with
 * escaped separators; per pattern n (capture tag n):
 *   %;f> <pat>\:<n>??\:<n>??[+off]m <id>\\\:${OK1}p OK <loc>:a<n>\\\:1q\:
 * (the ${OK1} success report only on fallback blocks, n >= 1)
 * The search's error status is captured into tag <n>; on success the
 * <n>?? branch marks the target and 1q short-circuits out of the
 * block, skipping the remaining attempts and the check. After the
 * last block (no 1q) a single <0;1;..>??! DNF check over all tags
 * reports the failure.
 * A mode-1 pattern (single-line by default) searches the live buffer
 * with ";0\:0reg\:.,$f> ^pat$" instead, restoring the register cache
 * with 98reg on both the success (before 1q) and no-match paths. */
static void emit_fallback_chain(FILE *out, pat_spec_t *ps, int nps,
				int mark_id, int target_line, int first)
{
	fputc('?', out);
	for (int n = 0; n < nps; n++) {
		int m1 = ps[n].mode == 1;
		if (n)
			EMIT_ESCSEP(out);
		if (m1) {
			/* Mode 1: search the live buffer directly. ";0"
			 * resets xoff, "0reg" clears the default register so
			 * f> reads the buffer (not the cache); the matching
			 * 98reg below restores the cache for later blocks. */
			fputs(";0", out);
			EMIT_ESCSEP(out);
			fputs("0reg", out);
			EMIT_ESCSEP(out);
			fputs(first ? ".,\\$f> " : ".,\\$f+ ", out);
		} else
			fputs(first ? "%;f> " : "%;f+ ", out);
		emit_chain_pattern(out, &ps[n]);
		EMIT_ESCSEP(out);
		fprintf(out, "%d??", n);
		EMIT_ESCSEP(out);
		fprintf(out, "%d??", n);
		if (ps[n].offset)
			fprintf(out, "%+d", ps[n].offset);
		fprintf(out, "m %d", mark_id);
		/* fallback (non-primary) match: with DBG1=1, OK1 expands
		 * empty and reports which anchor resolved the group */
		if (n) {
			EMIT_ESC3SEP(out);
			fprintf(out, "${OK1}p OK %s:%d:a%d",
				cur_file_path ? cur_file_path : "?",
				target_line, n);
		}
		if (m1) {
			/* restore the register cache on the success path,
			 * before 1q quits out of the chain */
			EMIT_ESC3SEP(out);
			fputs("98reg", out);
		}
		if (n < nps - 1) {
			/* 1q sits inside the <n>?? then-arg, one level
			 * deeper, so its separator needs three escapes */
			EMIT_ESC3SEP(out);
			fputs("1q", out);
		}
		if (m1) {
			/* restore the cache on the no-match fall-through */
			EMIT_ESCSEP(out);
			fputs("98reg", out);
		}
	}
	EMIT_SEP(out);
	fputs("${LB}\n", out);
	EMIT_SEP(out);
	emit_err_check_pats(out, nps, target_line);
	fputs("${LB}\n", out);
	EMIT_SEP(out);
}

/* Phase 2: delete at a mark */
static void emit_mark_delete(FILE *out, int line, int mark_id, int count)
{
	if (count == 1)
		fprintf(out, "'%dd", mark_id);
	else
		fprintf(out, "'%d,#+%dd", mark_id, count - 1);
	EMIT_SEP(out);
	emit_err_check_mark(out, line, mark_id);
}

/* Phase 2: insert at a mark (a after the mark, i before it).
 * mark_id < 0 means a new file's empty buffer: no line to mark,
 * so the insert is emitted bare. */
static void emit_mark_insert(FILE *out, int line, int mark_id, int use_i,
			     char **texts, int ntexts)
{
	if (ntexts == 0)
		return;
	if (mark_id < 0)
		fputs("i ", out);
	else
		fprintf(out, "'%d%c ", mark_id, use_i ? 'i' : 'a');
	emit_content(out, texts, ntexts);
	EMIT_SEP(out);
	emit_err_check_mark(out, line, mark_id);
}

/* Phase 2: change lines at a mark */
static void emit_mark_change(FILE *out, int line, int mark_id,
			     int del_count, char **texts, int ntexts)
{
	if (ntexts == 0) {
		emit_mark_delete(out, line, mark_id, del_count);
		return;
	}
	if (del_count == 1)
		fprintf(out, "'%dc ", mark_id);
	else
		fprintf(out, "'%d,#+%dc ", mark_id, del_count - 1);
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
	char *r = emalloc(len + t + 1);
	memcpy(r, s, len);
	memset(r + len, '\\', t);
	r[len + t] = '\0';
	free(s);
	return r;
}

/* Escape replacement text for substitute command.
 * In nextvi :s replacement, only \ is special (for backreferences \0-\9).
 * Delimiter must also be escaped. delim is always '/' in current callers. */
static char *escape_sub_repl(const char *s, char delim)
{
	char set[3] = { '\\', delim, 0 };
	return double_trailing_esc(escape_chars(s, set));
}

/* Escape regex pattern for substitute command.
 * Like escape_regex() but also escapes the delimiter for ex_re_read. */
static char *escape_sub_pat(const char *s, char delim)
{
	char set[sizeof(REGEX_META) + 1];
	snprintf(set, sizeof(set), "%s%c", REGEX_META, delim);
	return double_trailing_esc(escape_chars(s, set));
}

/* Emit the s/old/new/ substitute command (no positioning).
 * Escapes old_text as regex pattern and new_text as replacement,
 * both through ex_re_read delimiter + ex_arg + shell layers. */
static void emit_substitute_cmd(FILE *out, const char *old_text,
				const char *new_text)
{
	char *pat = escape_sub_pat(old_text, '/');
	char *repl = escape_sub_repl(new_text, '/');
	fputs("s/", out);
	char *pat_ea = escape_exarg(pat);
	emit_escaped_line(out, pat_ea);
	fputc('/', out);
	char *repl_ea = escape_exarg(repl);
	emit_escaped_line(out, repl_ea);
	fputc('/', out);
	free(repl_ea);
	free(pat_ea);
	free(repl);
	free(pat);
}

/* Phase 2: substitute at a mark. The pattern can fail to match
 * within the line, so keep the error check. */
static void emit_mark_substitute(FILE *out, int line, int mark_id,
				 const char *old_text, const char *new_text)
{
	fprintf(out, "'%d", mark_id);
	emit_substitute_cmd(out, old_text, new_text);
	EMIT_SEP(out);
	emit_err_check_mark(out, line, mark_id);
}

/* Phase 2: horizontal ;c / ;d edit tail, emitted after an address
 * prefix ('N for marks). Uses precomputed minimal diff positions. */
static void emit_horiz_tail(FILE *out, group_t *g)
{
	if (!*g->ldc_new_text && g->ldc_start != g->ldc_end)
		fprintf(out, ";%d;%dd", g->ldc_start, g->ldc_end);
	else if (g->ldc_start == g->ldc_end) {
		fprintf(out, ";%dc ", g->ldc_start);
		emit_escaped_text(out, g->ldc_new_text);
	} else {
		fprintf(out, ";%d;%dc ", g->ldc_start, g->ldc_end);
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

/* Per-group parsed data from an interactive temp file (raw, no offset stripping) */
typedef struct {
	int strategy;
	int level;         /* 1-5 comparison strictness (0 = default) */
	int has_star;
	char **del_lines;
	int ndel_lines, del_cap;
	char **add_lines;
	int nadd_lines, add_cap;
	char **custom_text;   /* raw lines from the section (captures everything) */
	int ncustom_text, custom_text_cap;
	char **pattern[NSEARCH];
	int npattern[NSEARCH], pat_cap[NSEARCH];
	int pat_off[NSEARCH];      /* per-pattern OFFSET marker value */
	int pat_has_off[NSEARCH];
	int pat_mode[NSEARCH];     /* per-pattern MODE: 0 = %;f>, 1 = .,$f> */
	int pat_has_mode[NSEARCH];
	char **abs_cmd;
	int nabs, abs_cap;
	char **rel_cmd;
	int nrel, rel_cap;
	char **relc_cmd;
	int nrelc, relc_cap;
} parsed_grp_t;

static void free_parsed_grp(parsed_grp_t *p)
{
	for (int i = 0; i < p->ndel_lines; i++)
		free(p->del_lines[i]);
	free(p->del_lines);
	for (int i = 0; i < p->nadd_lines; i++)
		free(p->add_lines[i]);
	free(p->add_lines);
	for (int i = 0; i < p->ncustom_text; i++)
		free(p->custom_text[i]);
	free(p->custom_text);
	for (int k = 0; k < NSEARCH; k++) {
		for (int i = 0; i < p->npattern[k]; i++)
			free(p->pattern[k][i]);
		free(p->pattern[k]);
	}
	for (int i = 0; i < p->nabs; i++)
		free(p->abs_cmd[i]);
	free(p->abs_cmd);
	for (int i = 0; i < p->nrel; i++)
		free(p->rel_cmd[i]);
	free(p->rel_cmd);
	for (int i = 0; i < p->nrelc; i++)
		free(p->relc_cmd[i]);
	free(p->relc_cmd);
}

/*
 * Parse a multi-file interactive temp file. Sections marked by
 * "=== FILE: <path> ===" route subsequent groups to per_file_results[k]
 * (k = matching index in active[]). Stores raw content (no
 * parse_ecmd_offset stripping) for apples-to-apples comparison between
 * .orig and edited files.
 */
static void parse_tmp_file(const char *path, file_patch_t **active, int nactive,
			   parsed_grp_t **per_file_results)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return;

	char line[MAX_LINE];
	int gi = -1;
	int file_idx = -1;
	int in_pat = 0, in_cstrat = 0, in_ecmd = 0;
	int in_content_section =
		0;  /* between GROUP header and first section keyword */
	int ecmd_strat = STRAT_DEFAULT;

	while (fgets(line, sizeof(line), f)) {
		chomp(line);

		/* "=== OFFSET <%+d> MODE <%d> ===" marker right after a
		 * SEARCH PATTERN header: the offset and search mode for that
		 * pattern. Handled before the generic reset so in_pat stays
		 * active. MODE is optional (older files omit it). */
		if (strncmp(line, "=== OFFSET ", 11) == 0) {
			if (in_pat && file_idx >= 0 && gi >= 0 &&
			    gi < active[file_idx]->ngroups) {
				parsed_grp_t *pg = &per_file_results[file_idx][gi];
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
			if (in_pat) {
				in_pat = 0;
			}
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
			continue;
		}
		if (strncmp(line, "=== COMMAND STRATEGY", 20) == 0) {
			in_cstrat = 1;
			continue;
		}
		if (strncmp(line, "=== SEARCH PATTERN", 18) == 0) {
			/* "=== SEARCH PATTERN <1-NPAT> ===", bare legacy form
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
		parsed_grp_t *results = per_file_results[file_idx];

		if (in_ecmd && gi >= 0 && gi < ngroups) {
			parsed_grp_t *pg = &results[gi];
			if (ecmd_strat == STRAT_ABS)
				arr_append(&pg->abs_cmd, &pg->nabs, &pg->abs_cap, line);
			else if (ecmd_strat == STRAT_REL)
				arr_append(&pg->rel_cmd, &pg->nrel, &pg->rel_cap, line);
			else if (ecmd_strat == STRAT_RELC)
				arr_append(&pg->relc_cmd, &pg->nrelc, &pg->relc_cap, line);
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
			parsed_grp_t *pg = &results[gi];
			char *lv = line + 10;
			char *end = strstr(lv, " ===");
			if (end)
				*end = '\0';
			int len = strlen(lv);
			pg->has_star = (len > 0 && lv[len-1] == '*');
			pg->level = atoi(lv);
			if (pg->level < 1)
				pg->level = 2;
			continue;
		}

		if (in_pat && gi >= 0 && gi < ngroups) {
			parsed_grp_t *pg = &results[gi];
			int pi = in_pat - 1;
			arr_append(&pg->pattern[pi], &pg->npattern[pi],
				   &pg->pat_cap[pi], line);
		}

		/* Catch-all: capture every line in the group section into custom_text as-is */
		if (gi >= 0 && gi < ngroups && in_content_section) {
			arr_append(&results[gi].custom_text, &results[gi].ncustom_text,
				   &results[gi].custom_text_cap, line);
		}
	}

	fclose(f);
}

/* Emit one group's delta in human-readable structured format */
static void emit_grp_delta(FILE *out, grp_delta_t *gd)
{
	fprintf(out, "=== GROUP %d ===\n", gd->group_idx);
	for (int i = 0; i < gd->ndel_lines; i++)
		fprintf(out, "-%s\n", gd->del_lines[i]);
	for (int i = 0; i < gd->nadd_lines; i++)
		fprintf(out, "+%s\n", gd->add_lines[i]);
	fprintf(out, "%s\n", end_tag_wr);
	int eglvl = gd->level ? gd->level : 2;
	fprintf(out, "=== LEVEL %d%s ===\n", eglvl,
		gd->has_star ? "*" : "");
	if (gd->ncustom_text > 0) {
		fprintf(out, "=== custom_text ===\n");
		for (int i = 0; i < gd->ncustom_text; i++)
			fprintf(out, "%s\n", gd->custom_text[i]);
		fprintf(out, "%s\n", end_tag_wr);
	}
	if (gd->npre_ctx > 0) {
		fprintf(out, "=== pre_ctx ===\n");
		for (int i = 0; i < gd->npre_ctx; i++)
			fprintf(out, "%s\n", gd->pre_ctx[i]);
		fprintf(out, "%s\n", end_tag_wr);
	}
	if (gd->npost_ctx > 0) {
		fprintf(out, "=== post_ctx ===\n");
		for (int i = 0; i < gd->npost_ctx; i++)
			fprintf(out, "%s\n", gd->post_ctx[i]);
		fprintf(out, "%s\n", end_tag_wr);
	}
	if (gd->strategy != STRAT_DEFAULT) {
		const char *s = "abs";
		if (gd->strategy == STRAT_REL)
			s = "rel";
		else if (gd->strategy == STRAT_RELC)
			s = "relc";
		fprintf(out, "=== strategy ===\n%s\n%s\n", s, end_tag_wr);
	}
	for (int pi = 0; pi < NSEARCH; pi++) {
		if (gd->npattern[pi] > 0) {
			fprintf(out, "=== pattern%d ===\n", pi + 1);
			for (int i = 0; i < gd->npattern[pi]; i++)
				fprintf(out, "%s\n", gd->pattern[pi][i]);
			fprintf(out, "%s\n", end_tag_wr);
		}
		if (gd->pat_has_off[pi])
			fprintf(out, "=== offset%d %+d ===\n",
				pi + 1, gd->pat_off[pi]);
		if (gd->pat_has_mode[pi])
			fprintf(out, "=== mode%d %d ===\n",
				pi + 1, gd->pat_mode[pi]);
	}
	if (gd->nabs > 0) {
		fprintf(out, "=== edit_cmd_abs ===\n");
		for (int i = 0; i < gd->nabs; i++)
			fprintf(out, "%s\n", gd->abs_cmd[i]);
		fprintf(out, "%s\n", end_tag_wr);
	}
	if (gd->nrelc > 0) {
		fprintf(out, "=== edit_cmd_relc ===\n");
		for (int i = 0; i < gd->nrelc; i++)
			fprintf(out, "%s\n", gd->relc_cmd[i]);
		fprintf(out, "%s\n", end_tag_wr);
	}
	if (gd->nrel > 0) {
		fprintf(out, "=== edit_cmd_rel ===\n");
		for (int i = 0; i < gd->nrel; i++)
			fprintf(out, "%s\n", gd->rel_cmd[i]);
		fprintf(out, "%s\n", end_tag_wr);
	}
}

/*
 * Write all groups to fp, optionally injecting stored delta from in_fd.
 */
static void write_groups_to_file(FILE *fp, group_t *groups, int ngroups,
				 file_delta_t *in_fd, int is_new,
				 const char *orig_path)
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

		/* Migrate old deltas: a +N stored in the rel/relc EDIT
		 * COMMAND becomes the pattern 1-2 OFFSET (pattern 3 stays
		 * 0, matching the old read-back semantics). Mutates gd so
		 * the .orig and edit copies agree. */
		if (gd && !gd->pat_has_off[0] && !gd->pat_has_off[1] &&
		    !gd->pat_has_off[2] && (gd->nrel > 0 || gd->nrelc > 0)) {
			int mig = 0;
			if (gd->nrelc > 0)
				mig = parse_ecmd_offset(gd->relc_cmd, &gd->nrelc);
			if (gd->nrel > 0)
				mig = parse_ecmd_offset(gd->rel_cmd, &gd->nrel);
			gd->pat_off[0] = gd->pat_off[1] = mig;
			gd->pat_off[2] = 0;
			gd->pat_has_off[0] = gd->pat_has_off[1] =
				gd->pat_has_off[2] = 1;
		}

		/* Group header */
		fprintf(fp, "=== GROUP %d/%d (line %d) ===\n",
			gi + 1, ngroups, target);
		if (gd && gd->ncustom_text > 0 && gd->has_star && in_fd) {
			for (int i = 0; i < gd->ncustom_text; i++)
				fprintf(fp, "%s\n", gd->custom_text[i]);
		} else {
			for (int i = 0; i < g->ndel; i++)
				fprintf(fp, "-%s\n", g->del_texts[i]);
			for (int i = 0; i < g->nadd; i++)
				fprintf(fp, "+%s\n", g->add_texts[i]);
		}
		fprintf(fp, "%s\n", end_tag_wr);
		int lvl = (gd && gd->level) ? gd->level : 2;
		fprintf(fp, "=== LEVEL %d%s ===\n", lvl, gd && gd->has_star ? "*" : "");

		/* COMMAND STRATEGY: inject stored strategy or keep all commented */
		int sel_strat = (gd && gd->strategy != STRAT_DEFAULT)
				? gd->strategy : STRAT_DEFAULT;
		fprintf(fp, "=== COMMAND STRATEGY ===\n");
		fprintf(fp, "%sabs\n", sel_strat == STRAT_ABS ? "" : "#");
		if (has_anchors && g->ndel == 1 && g->nadd == 1 && g->has_line_diff)
			fprintf(fp, "%srelc\n", sel_strat == STRAT_RELC ? "" : "#");
		if (has_anchors)
			fprintf(fp, "%srel\n", sel_strat == STRAT_REL ? "" : "#");

		/* SEARCH PATTERN 1-NPAT (fallbacks, first match wins):
		 * 1 = whole hunk, 2 = deleted lines + post ctx, 3 = top
		 * context only, 4 = deleted lines, 5 = post ctx only.
		 * Single-line patterns show the ^...$ disamb anchors so the
		 * user can remove them; emit respects the edit. */
		fprintf(fp, "%s\n", end_tag_wr);
		/* Pure adds position on the line to append after, so the
		 * displayed offsets include the -1 step the a verb implies
		 * (matching the a/i choice in the rel EDIT COMMAND). */
		int pure_add = !g->del_start && g->nadd;
		int add_a = pure_add && default_offset - 1 >= 0;
		char **praw = emalloc((g->ndel + 7) * sizeof(char *));
		for (int pi = 0; pi < NPAT; pi++) {
			fprintf(fp, "=== SEARCH PATTERN %d ===\n", pi + 1);
			int doff;
			int n = default_pat_lines(g, pi, praw, &doff);
			/* OFFSET marker: lines from match start to the edit
			 * target when this pattern matches. MODE selects the
			 * search form: 1 = .,$f> (live buffer, default for
			 * single-line patterns), 0 = %;f> (register cache). */
			int poff = (gd && gd->pat_has_off[pi])
				   ? gd->pat_off[pi]
				   : doff - (add_a ? 1 : 0);
			int pat_nlines = (gd && gd->npattern[pi] > 0)
					 ? gd->npattern[pi] : n;
			int pmode = (gd && gd->pat_has_mode[pi])
				    ? gd->pat_mode[pi]
				    : pat_nlines == 1 ? 1 : 0;
			fprintf(fp, "=== OFFSET %+d MODE %d ===\n", poff, pmode);
			if (gd && gd->npattern[pi] > 0) {
				for (int i = 0; i < gd->npattern[pi]; i++)
					fprintf(fp, "%s\n", gd->pattern[pi][i]);
			} else {
				int wrap = n == 1;
				for (int i = 0; i < n; i++) {
					char *esc = escape_regex(praw[i]);
					fprintf(fp, wrap ? "^%s$\n" : "%s\n", esc);
					free(esc);
				}
			}
			fprintf(fp, "%s\n", end_tag_wr);
		}
		free(praw);

		/* File-validated fuzzed (relaxed) SEARCH PATTERN sections,
		 * slots NPAT..NSEARCH. Generated fresh from the original on a
		 * first pass; a recorded delta (a prior edit) wins, so the
		 * user's tweaks round-trip. These lines are already regex
		 * (pre-escaped), so they are written verbatim. */
		fuzzwin_t fz[NFUZZ];
		int nfz = gen_fuzz_windows(g, fz, NFUZZ);
		for (int pi = NPAT; pi < NSEARCH; pi++) {
			int fi = pi - NPAT;
			int recorded = gd && gd->npattern[pi] > 0;
			if (!recorded && fi >= nfz)
				continue;
			fprintf(fp, "=== SEARCH PATTERN %d ===\n", pi + 1);
			int poff = (gd && gd->pat_has_off[pi]) ? gd->pat_off[pi]
				 : recorded ? 0 : fz[fi].offset;
			int pmode = (gd && gd->pat_has_mode[pi]) ? gd->pat_mode[pi]
				  : recorded ? (gd->npattern[pi] == 1) : fz[fi].mode;
			fprintf(fp, "=== OFFSET %+d MODE %d ===\n", poff, pmode);
			if (recorded) {
				for (int i = 0; i < gd->npattern[pi]; i++)
					fprintf(fp, "%s\n", gd->pattern[pi][i]);
			} else {
				for (int i = 0; i < fz[fi].nlines; i++)
					fprintf(fp, "%s\n", fz[fi].lines[i]);
			}
			fprintf(fp, "%s\n", end_tag_wr);
		}
		free_fuzz_windows(fz, nfz);

		/* EDIT COMMAND sections */
#define WG_CONTENT(fp) do { \
	if (g->nadd > 0) { \
		fputc(' ', (fp)); \
		fputs(g->add_texts[0], (fp)); \
		fputc('\n', (fp)); \
		for (int _k = 1; _k < g->nadd; _k++) { \
			fputs(g->add_texts[_k], (fp)); \
			fputc('\n', (fp)); \
		} \
	} else { fputc('\n', (fp)); } \
} while (0)

		/* abs */
		fputs("=== EDIT COMMAND (abs) ===\n", fp);
		if (gd && gd->nabs > 0) {
			for (int k = 0; k < gd->nabs; k++)
				fprintf(fp, "%s\n", gd->abs_cmd[k]);
		} else {
			if (g->del_start && g->nadd) {
				if (g->ndel == 1)
					fprintf(fp, "%dc", g->del_start);
				else
					fprintf(fp, "%d,%dc", g->del_start, g->del_end);
				WG_CONTENT(fp);
			} else if (g->del_start) {
				if (g->ndel == 1)
					fprintf(fp, "%dd\n", g->del_start);
				else
					fprintf(fp, "%d,%dd\n", g->del_start, g->del_end);
			} else if (g->nadd) {
				if (is_new)
					fputs("i", fp);
				else if (g->add_after <= 0)
					fputs("1i", fp);
				else
					fprintf(fp, "%da", g->add_after);
				WG_CONTENT(fp);
			}
		}
		fprintf(fp, "%s\n", end_tag_wr);

		/* relc */
		int show_relc = has_anchors && g->ndel == 1 && g->nadd == 1 && g->has_line_diff;
		if (show_relc || (gd && gd->nrelc > 0)) {
			fputs("=== EDIT COMMAND (relc) ===\n", fp);
			if (gd && gd->nrelc > 0) {
				for (int k = 0; k < gd->nrelc; k++)
					fprintf(fp, "%s\n", gd->relc_cmd[k]);
			} else if (show_relc) {
				if (g->ldc_start == g->ldc_end)
					fprintf(fp, ".;%dc %s\n",
						g->ldc_start, g->ldc_new_text);
				else
					fprintf(fp, ".;%d;%dc %s\n",
						g->ldc_start, g->ldc_end,
						g->ldc_new_text);
			}
			fprintf(fp, "%s\n", end_tag_wr);
		}

		/* rel */
		if (has_anchors || (gd && gd->nrel > 0)) {
			fputs("=== EDIT COMMAND (rel) ===\n", fp);
			if (gd && gd->nrel > 0) {
				for (int k = 0; k < gd->nrel; k++)
					fprintf(fp, "%s\n", gd->rel_cmd[k]);
			} else if (has_anchors) {
				if (g->ndel == 1 && g->nadd == 1 && g->has_line_diff) {
					char *esc_pat = escape_sub_pat(g->ld_old_text, '/');
					char *esc_rep = escape_sub_repl(g->ld_new_text, '/');
					fprintf(fp, "s/%s/%s/\n", esc_pat, esc_rep);
					free(esc_pat);
					free(esc_rep);
				} else if (g->del_start && g->nadd) {
					if (g->ndel == 1)
						fputs("c", fp);
					else
						fprintf(fp, ",#+%dc", g->ndel - 1);
					WG_CONTENT(fp);
				} else if (g->del_start) {
					if (g->ndel == 1)
						fputs("d\n", fp);
					else
						fprintf(fp, ",#+%dd\n", g->ndel - 1);
				} else if (g->nadd) {
					fputs(add_a ? "a" : "i", fp);
					WG_CONTENT(fp);
				}
			}
			fprintf(fp, "%s\n", end_tag_wr);
		}
		if (gi + 1 < ngroups)
			fputc('\n', fp);
	}
	free_orig_file();
}

/*
 * Interactive editing of all groups' search patterns in one file.
 * Opens $EDITOR once with all groups. Pattern lines are shown
 * regex-escaped (as they'll appear to the regex engine).
 * If file saved: all groups use the edited patterns.
 * If file not saved (mtime unchanged): all groups use default behavior.
 */
static void interactive_edit_all_files(file_patch_t **active, int nactive)
{
	if (nactive == 0)
		return;

	char tmptmp[32];
	char tmppath[256];
	char tmppath_orig[262];
	char rejpath[260];
	snprintf(tmptmp, sizeof(tmptmp), "patch2vi_XXXXXX");
	int fd = mkstemp(tmptmp);
	snprintf(tmppath, sizeof(tmppath), "%s.diff", tmptmp);
	if (rename(tmptmp, tmppath) < 0)
		snprintf(tmppath, sizeof(tmppath), "%s", tmptmp);
	if (fd < 0) {
		perror("mkstemp");
		return;
	}
	snprintf(tmppath_orig, sizeof(tmppath_orig), "%s.orig", tmppath);
	snprintf(rejpath, sizeof(rejpath), "%s.rej", tmppath);

	/* Per-file in_fd lookup */
	file_delta_t **in_fd_per = calloc(nactive, sizeof(file_delta_t *));
	if (delta_mode) {
		for (int k = 0; k < nactive; k++)
			for (int di = 0; di < nin_deltas; di++)
				if (strcmp(in_deltas[di].filepath, active[k]->path) == 0) {
					in_fd_per[k] = &in_deltas[di];
					break;
				}
	}

	/* Write auto-generated baseline (no injection) for later comparison */
	FILE *orig_fp = fopen(tmppath_orig, "w");
	if (orig_fp) {
		for (int k = 0; k < nactive; k++) {
			fprintf(orig_fp, "=== FILE: %s ===\n", active[k]->path);
			write_groups_to_file(orig_fp,
					     active[k]->groups, active[k]->ngroups, NULL,
					     active[k]->is_new,
					     active[k]->orig_path ? active[k]->orig_path
								  : active[k]->path);
			fprintf(orig_fp, "%s\n", end_tag_wr);
			fputc('\n', orig_fp);
		}
		fclose(orig_fp);
	}

	/* Rejection check: before writing editor file so we can clear
	 * has_star on rejected deltas (prevents custom_text injection). */
	int delta_rejected = 0;
	FILE *rej = NULL;
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
					rej = fopen(rejpath, "w");
					if (rej)
						fprintf(rej,
							"# Rejected: index out of range"
							" or content mismatch\n\n");
				}
				if (rej) {
					if (!file_header_written) {
						fprintf(rej, "=== FILE: %s ===\n",
							active[k]->path);
						file_header_written = 1;
					}
					emit_grp_delta(rej, stored);
				}
				delta_rejected = 1;
			}
		}
		if (file_header_written && rej) {
			fprintf(rej, "%s\n", end_tag_wr);
			fputc('\n', rej);
		}
	}
	if (rej)
		fclose(rej);

	/* -i (interactive, non-delta): every stored delta is rejected
	 * wholesale. in_fd_per stays NULL so nothing is injected or
	 * preserved; the deltas are dumped to .rej so the user can
	 * re-apply them by hand, mirroring -d's reject flow. */
	if (!delta_mode && nin_deltas) {
		FILE *irej = fopen(rejpath, "w");
		if (irej) {
			fprintf(irej, "# Rejected: interactive (-i)"
				      " discards all stored deltas\n\n");
			for (int di = 0; di < nin_deltas; di++) {
				file_delta_t *in_fd = &in_deltas[di];
				fprintf(irej, "=== FILE: %s ===\n", in_fd->filepath);
				for (int gi = 0; gi < in_fd->ngrps; gi++)
					emit_grp_delta(irej, &in_fd->grps[gi]);
				fprintf(irej, "%s\n", end_tag_wr);
				fputc('\n', irej);
			}
			fclose(irej);
			delta_rejected = 1;
		}
	}

	/* Write editor file with stored delta injected (has_star was cleared
	 * for rejected groups above, so custom_text won't be written). */
	FILE *tmp_fp = fdopen(fd, "w");
	if (!tmp_fp) {
		perror("fdopen");
		close(fd);
		unlink(tmppath);
		unlink(tmppath_orig);
		free(in_fd_per);
		return;
	}
	for (int k = 0; k < nactive; k++) {
		fprintf(tmp_fp, "=== FILE: %s ===\n", active[k]->path);
		write_groups_to_file(tmp_fp,
				     active[k]->groups, active[k]->ngroups,
				     in_fd_per[k], active[k]->is_new,
				     active[k]->orig_path ? active[k]->orig_path
							  : active[k]->path);
		fprintf(tmp_fp, "%s\n", end_tag_wr);
		fputc('\n', tmp_fp);
	}
	fclose(tmp_fp);

	/* Record mtime before editor */
	struct stat st_before;
	if (stat(tmppath, &st_before) < 0) {
		perror("stat");
		goto cleanup_orig;
	}

	/* Open editor */
	const char *editor = getenv("EDITOR");
	if (!editor)
		editor = getenv("VISUAL");
	if (!editor)
		editor = "vi";
	char ed_cmd[MAX_LINE];
	if (delta_rejected)
		snprintf(ed_cmd, sizeof(ed_cmd),
			 "%s '%s' '%s' </dev/tty >/dev/tty",
			 editor, tmppath, rejpath);
	else
		snprintf(ed_cmd, sizeof(ed_cmd),
			 "%s '%s' </dev/tty >/dev/tty",
			 editor, tmppath);
	int ed_ret = system(ed_cmd);
	if (delta_rejected)
		unlink(rejpath);
	if (ed_ret != 0) {
		fprintf(stderr, "editor exited with error %d\n", ed_ret);
		goto cleanup_orig;
	}

	/* Check if file was saved (mtime or size changed) */
	struct stat st_after;
	if (stat(tmppath, &st_after) < 0) {
		perror("stat");
		goto cleanup_orig;
	}
	int unchanged = st_before.st_mtime == st_after.st_mtime
			&& st_before.st_size == st_after.st_size;
#ifdef st_mtime
	if (unchanged)
		unchanged = st_before.st_mtim.tv_nsec == st_after.st_mtim.tv_nsec;
#endif

	/* Parse the edited file once per file slot */
	parsed_grp_t **edit_per = emalloc(nactive * sizeof(parsed_grp_t *));
	for (int k = 0; k < nactive; k++)
		edit_per[k] = calloc(active[k]->ngroups, sizeof(parsed_grp_t));
	parse_tmp_file(tmppath, active, nactive, edit_per);

	if (unchanged) {
		/* User quit without saving. Preserve stored delta if present
		 * so next -d run still injects the previous customizations. */
		for (int k = 0; k < nactive; k++) {
			file_delta_t *in_fd = in_fd_per[k];
			if (!in_fd || in_fd->ngrps == 0)
				continue;
			file_delta_t *od = &out_deltas[nout_deltas++];
			od->filepath = xstrdup(active[k]->path);
			od->gcap = in_fd->ngrps;
			od->grps = emalloc(od->gcap * sizeof(grp_delta_t));
			od->ngrps = in_fd->ngrps;
			for (int gi = 0; gi < in_fd->ngrps; gi++) {
				grp_delta_t *src = &in_fd->grps[gi], *dst = &od->grps[gi];
				memset(dst, 0, sizeof(*dst));
				dst->group_idx = src->group_idx;
				dst->level = src->level;
				dst->has_star = src->has_star;
				dst->strategy = src->strategy;
				arr_clone(&dst->del_lines, &dst->ndel_lines, &dst->del_cap, src->del_lines,
					  src->ndel_lines);
				arr_clone(&dst->add_lines, &dst->nadd_lines, &dst->add_cap, src->add_lines,
					  src->nadd_lines);
				arr_clone(&dst->custom_text, &dst->ncustom_text, &dst->custom_text_cap,
					  src->custom_text, src->ncustom_text);
				arr_clone(&dst->pre_ctx, &dst->npre_ctx, &dst->pre_cap, src->pre_ctx,
					  src->npre_ctx);
				arr_clone(&dst->post_ctx, &dst->npost_ctx, &dst->post_cap, src->post_ctx,
					  src->npost_ctx);
				for (int pi = 0; pi < NSEARCH; pi++) {
					arr_clone(&dst->pattern[pi], &dst->npattern[pi],
						  &dst->pat_cap[pi], src->pattern[pi],
						  src->npattern[pi]);
					dst->pat_off[pi] = src->pat_off[pi];
					dst->pat_has_off[pi] = src->pat_has_off[pi];
					dst->pat_mode[pi] = src->pat_mode[pi];
					dst->pat_has_mode[pi] = src->pat_has_mode[pi];
				}
				arr_clone(&dst->abs_cmd, &dst->nabs, &dst->abs_cap, src->abs_cmd,
					  src->nabs);
				arr_clone(&dst->rel_cmd, &dst->nrel, &dst->rel_cap, src->rel_cmd,
					  src->nrel);
				arr_clone(&dst->relc_cmd, &dst->nrelc, &dst->relc_cap,src->relc_cmd,
					  src->nrelc);
			}
		}
	} else {
		/* Compute structured delta: compare .orig (auto-generated baseline)
		 * against edited file. Only store changed groups. */
		parsed_grp_t **orig_per = emalloc(nactive * sizeof(parsed_grp_t *));
		for (int k = 0; k < nactive; k++)
			orig_per[k] = calloc(active[k]->ngroups, sizeof(parsed_grp_t));
		parse_tmp_file(tmppath_orig, active, nactive, orig_per);

		for (int k = 0; k < nactive; k++) {
			file_delta_t *od = NULL;
			for (int gi = 0; gi < active[k]->ngroups; gi++) {
				parsed_grp_t *og = &orig_per[k][gi];
				parsed_grp_t *eg = &edit_per[k][gi];

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

				if (!strat_ch && !pat_ch &&
				    !abs_ch && !rel_ch && !relc_ch && !level_ch &&
				    !custom_ch)
					continue;

				if (!od) {
					od = &out_deltas[nout_deltas++];
					od->filepath = xstrdup(active[k]->path);
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
				/* customization from user's edits */
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
				arr_clone(&gout->pre_ctx, &gout->npre_ctx, &gout->pre_cap,
					  active[k]->groups[gi].all_pre_ctx, active[k]->groups[gi].nall_pre_ctx);
				arr_clone(&gout->post_ctx, &gout->npost_ctx, &gout->post_cap,
					  active[k]->groups[gi].post_ctx, active[k]->groups[gi].npost_ctx);
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
				free_parsed_grp(&orig_per[k][gi]);
			free(orig_per[k]);
		}
		free(orig_per);
	}

	/* Apply edit_per to groups[], transferring ownership of arrays. */
	for (int k = 0; k < nactive; k++) {
		for (int gi = 0; gi < active[k]->ngroups; gi++) {
			parsed_grp_t *eg = &edit_per[k][gi];
			group_t *g = &active[k]->groups[gi];
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
			free_parsed_grp(eg);
		}
		free(edit_per[k]);
	}
	free(edit_per);

cleanup_orig:
	unlink(tmppath_orig);
	unlink(tmppath);
	free(in_fd_per);
}

/* Emit a custom EDIT COMMAND lines array + trailing SEP.
 * lines[0] = "cmd [first-content]", lines[1..n] = extra content lines.
 * s/pat/repl/: emit_escaped_exarg_only.
 * bare cmd (d, etc.): output verbatim. */
static void emit_custom_edit_lines(FILE *out, char **lines, int nlines)
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
		fwrite(first, 1, sp - first, out);  /* command prefix verbatim */
		fputc(' ', out);
		emit_escaped_text(out, sp + 1);     /* first content line escaped */
		if (!horiz)
			fputc('\n', out);
		for (int k = 1; k < nlines; k++) {
			emit_escaped_text(out, lines[k]);
			fputc('\n', out);
		}
	} else {
		/* No content (d, ,#+Nd, etc.) */
		fputs(first, out);
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

	fp->groups = calloc(fp->nops + 1, sizeof(group_t));
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

		/* Interactive mode: collect post-change context and build block */
		if (interactive_mode && (g->del_start || g->nadd)) {
			g->all_pre_ctx = all_ctx;
			g->nall_pre_ctx = nall_ctx;
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
				int prefix = 0;
				while (old[prefix] && new[prefix] && old[prefix] == new[prefix])
					prefix++;
				int suffix = 0;
				while (suffix < olen - prefix && suffix < nlen - prefix &&
				       old[olen-1-suffix] == new[nlen-1-suffix])
					suffix++;
				g->ldc_start = utf8_char_offset(old, prefix);
				g->ldc_end = utf8_char_offset(old, olen - suffix);
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
}

static void emit_file_script(FILE *out, file_patch_t *fp)
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

	/* Read the pre-patch original (if present) to validate anchor
	 * uniqueness; new files have no original to read. Prefer the "---"
	 * path (it names the pre-patch content); fall back to the edit
	 * target, which holds that same content before the script runs. */
	if (!fp->is_new)
		load_orig_file(fp->orig_path ? fp->orig_path : fp->path);

	/*
	 * Phase 1 (resolve): run every group's search against the register
	 * cache yanked once after file open and record the target line in
	 * a mark. No edits happen here, so the cache never goes stale and
	 * a failed anchor aborts with the file untouched.
	 */
	int next_id = 0;
	int first_search = 1;
	for (int gi = 0; gi < ngroups; gi++) {
		group_t *g = &groups[gi];
		g->mark_id = -1;
		g->insert_i = 0;
		if (!g->del_start && !g->nadd)
			continue;
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
				continue;
			/* New file: empty buffer, nothing to mark; phase 2
			 * emits a bare i (mark_id stays -1) */
			if (fp->is_new && !g->del_start) {
				g->insert_i = 1;
				continue;
			}
			int t = target_line;
			if (!g->del_start && t <= 0) {
				t = 1;
				g->insert_i = 1;
			}
			g->mark_id = next_mark_id(&next_id);
			fprintf(out, "%dm %d", t, g->mark_id);
			EMIT_SEP(out);
			continue;
		}

		/* Build the fallback pattern list: edited SEARCH PATTERN
		 * sections if any, else auto defaults. Duplicates dropped,
		 * first match wins at apply time. */
		pat_spec_t ps[NSEARCH];
		int nps = 0;
		char **raw = NULL;
		fuzzwin_t fz[NFUZZ];   /* owned fuzzed windows (plain -r path) */
		int nfz = 0;
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
			nps++;
		}
		if (nps == 0) {
			/* File-aware uniqueness is trustworthy only when the
			 * on-disk file is the pre-patch original; confirm by
			 * matching this hunk's deleted lines at their expected
			 * line. Pure adds (no del_start) keep blind ordering. */
			int pristine = orig_lines && g->del_start > 0 &&
				window_at(g->del_texts, g->ndel, g->del_start - 1);
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
				/* Ordering key: contiguous-literal specificity, plus
				 * a uniqueness bonus when the file confirms how this
				 * window resolves. expected = the index where the
				 * window must start to put the target (del_start - 1)
				 * at match_start + offset. */
				int sc = specificity_score(slot, n);
				if (pristine) {
					int first;
					int cnt = count_window(slot, n, &first);
					int expected = (g->del_start - 1) - doff;
					if (first == expected)
						sc += cnt == 1 ? UNIQUE_BONUS
							       : CORRECT_BONUS;
				}
				ps[nps].score = sc;
				nps++;
			}
			/* File-validated fuzzed anchors: relax the whole-hunk
			 * window and keep each relaxation the file proves still
			 * resolves uniquely to the right line. Interactive mode
			 * surfaces these as extra SEARCH PATTERN sections (and so
			 * arrives here via custom_pat, not this block); this
			 * covers the plain -r default path. */
			nfz = gen_fuzz_windows(g, fz, NFUZZ);
			for (int i = 0; i < nfz && nps < NSEARCH; i++) {
				ps[nps].lines = fz[i].lines;
				ps[nps].nlines = fz[i].nlines;
				ps[nps].pre_escaped = 1;
				ps[nps].offset = fz[i].offset;
				ps[nps].off_final = 0;
				ps[nps].mode = fz[i].mode;
				ps[nps].score = fz[i].score;
				nps++;
			}
			/* Order the auto-default chain objectively strict to
			 * loose by descending score (see specificity_score). A
			 * superset window (e.g. whole hunk over del+post) always
			 * scores higher, so this respects every superset relation
			 * while resolving region-incomparable cases by actual
			 * content; a file-proven unique anchor outranks every
			 * unproven one. The stable sort preserves
			 * default_pat_lines' curated tie-break. Custom patterns
			 * keep the user's explicit slot order. */
			for (int i = 1; i < nps; i++) {
				pat_spec_t key = ps[i];
				int j = i - 1;
				while (j >= 0 && ps[j].score < key.score) {
					ps[j + 1] = ps[j];
					j--;
				}
				ps[j + 1] = key;
			}
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
		 * offset (the displayed "+Na" already includes the a step),
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

		g->mark_id = next_mark_id(&next_id);
		if (nps == 0) {
			/* No usable anchor: mark the absolute line */
			fprintf(out, "%dm %d",
				target_line > 0 ? target_line : 1, g->mark_id);
			EMIT_SEP(out);
			free_fuzz_windows(fz, nfz);
			free(raw);
			continue;
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
		free(raw);
	}

	/*
	 * Phase 2 (commit): apply edits at the marks, forward order.
	 * Marks auto-adjust as edits shift lines above them.
	 */

	/* Helper: emit a custom edit command (lines array) at the mark.
	 * Substitute (lines[0] starts s+non-alnum): exarg escaping.
	 * Otherwise: verbs attach directly to the mark address. */
#define EMIT_MARK_EDIT(rlines, rnlines) do { \
		fprintf(out, "'%d", g->mark_id); \
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
		if (!g->del_start && !g->nadd) {
			free_group(g);
			continue;
		}
		int strat = g->res_strat;
		int tline = g->del_start ? g->del_start : g->add_after;
		fputs("${LB}\n", out);
		EMIT_SEP(out);

		/* Custom abs/rel edit commands apply regardless of del/add shape */
		if (strat == STRAT_ABS && g->custom_abs_lines) {
			emit_custom_edit_lines(out, g->custom_abs_lines,
					       g->custom_abs_nlines);
			EMIT_SEP(out);
			emit_err_check_mark(out, tline, g->mark_id);
		} else if (strat == STRAT_REL && g->custom_rel_lines
			   && g->custom_rel_nlines > 0) {
			EMIT_MARK_EDIT(g->custom_rel_lines, g->custom_rel_nlines);
		} else if (g->del_start && g->nadd) {
			if (strat == STRAT_RELC) {
				if (g->custom_relc_lines && g->custom_relc_nlines > 0) {
					/* custom relc lines address the current
					 * line (".;A;Bc"): jump to the mark first */
					fprintf(out, "'%d", g->mark_id);
					EMIT_SEP(out);
					emit_custom_edit_lines(out, g->custom_relc_lines,
							       g->custom_relc_nlines);
					EMIT_SEP(out);
				} else {
					fprintf(out, "'%d", g->mark_id);
					emit_horiz_tail(out, g);
				}
				emit_err_check_mark(out, tline, g->mark_id);
			} else if (strat == STRAT_REL && g->ndel == 1 && g->nadd == 1
				   && g->has_line_diff) {
				emit_mark_substitute(out, tline, g->mark_id,
						     g->ld_old_text, g->ld_new_text);
			} else if (strat == STRAT_ABS && g->ndel == 1 && g->nadd == 1
				   && g->has_line_diff) {
				fprintf(out, "'%d", g->mark_id);
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
		free_group(g);
	}
	free_orig_file();
}

/* set by "--- /dev/null", consumed by the next "+++" */
static int pending_is_new;
/* "---" path (pre-patch original), consumed by the next "+++" */
static char *pending_orig_path;

static void new_file(const char *path)
{
	files[nfiles].path = xstrdup(path);
	files[nfiles].nops = 0;
	files[nfiles].is_new = pending_is_new;
	files[nfiles].orig_path = pending_orig_path;
	pending_is_new = 0;
	pending_orig_path = NULL;
	nfiles++;
	/* path appears in the FAIL <path>:<line> error message inside EXINIT */
	mark_bytes_used(path);
}

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
	fp->ops[fp->nops].text = text ? xstrdup(text) : NULL;
	fp->nops++;

	/* Track bytes used in patch content */
	if (text)
		mark_bytes_used(text);
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-aridh] [-d[N]] [-er TAG] [-ew TAG] [input.patch]\n", prog);
	fprintf(stderr,
		"Converts unified diff to shell script using nextvi ex commands\n");
	fprintf(stderr, "  -a    Use absolute line numbers\n");
	fprintf(stderr,
		"  -r    Use relative regex patterns instead of line numbers\n");
	fprintf(stderr, "  -i    Interactive mode: edit search patterns in $EDITOR\n");
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
	int in_hunk = 0;
	int old_line = 0;
	const char *input_file = NULL;
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
		for (j = 1; argv[i][j]; j++) {
			if (argv[i][j] == 'a')
				relative_mode = 0;
			else if (argv[i][j] == 'r')
				relative_mode = 1;
			else if (argv[i][j] == 'i')
				interactive_mode = 1;
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
	}
	if (i < argc)
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

	FILE *in = stdin;
	if (input_file) {
		in = fopen(input_file, "r");
		if (!in) {
			perror(input_file);
			return 1;
		}
	}

	/* Detect if input is a previously generated patch2vi script */
	if (fgets(line, sizeof(line), in)) {
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
			int in_sect = 0; /* 1=pat 2=abs 3=rel 4=relc */
			int pat_idx = 1; /* which pattern[] for in_sect 1 */
			while (fgets(line, sizeof(line), in)) {
				chomp(line);
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
					if (interactive_mode) {
						char *p = line + 10;
						char *end = strstr(p, " ===");
						if (end)
							*end = '\0';
						cur_fd = &in_deltas[nin_deltas++];
						cur_fd->filepath = xstrdup(p);
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
						in_sect = 5;
						continue;
					}
					if (strncmp(line, "=== LEVEL ", 10) == 0) {
						char *lv = line + 10;
						char *end = strstr(lv, " ===");
						if (end)
							*end = '\0';
						int len = strlen(lv);
						cur_gd->has_star = (len > 0 && lv[len-1] == '*');
						cur_gd->level = atoi(lv);
						if (cur_gd->level < 1)
							cur_gd->level = 2;
						continue;
					}
					if (strcmp(line, "=== custom_text ===") == 0) {
						in_sect = 8;
						continue;
					}
					if (strcmp(line, "=== pre_ctx ===") == 0) {
						in_sect = 6;
						continue;
					}
					if (strcmp(line, "=== post_ctx ===") == 0) {
						in_sect = 7;
						continue;
					}
					if (strcmp(line, "=== strategy ===") == 0) {
						in_sect = 10;
						continue;
					}
					if (strncmp(line, "=== pattern", 11) == 0) {
						/* "pattern<1-NPAT>"; legacy bare
						 * "pattern" maps to the top-context
						 * slot (pattern 3) */
						char c = line[11];
						pat_idx = (c >= '1' && c <= '0' + NSEARCH)
							? c - '1' : 2;
						in_sect = 1;
						continue;
					}
					if (strncmp(line, "=== offset", 10) == 0) {
						/* "=== offset<1-NPAT> <%+d> ===" */
						char c = line[10];
						int oi = (c >= '1' && c <= '0' + NSEARCH)
							? c - '1' : 2;
						cur_gd->pat_off[oi] = atoi(line + 11);
						cur_gd->pat_has_off[oi] = 1;
						continue;
					}
					if (strncmp(line, "=== mode", 8) == 0) {
						/* "=== mode<1-NPAT> <%d> ===" */
						char c = line[8];
						int mi = (c >= '1' && c <= '0' + NSEARCH)
							? c - '1' : 2;
						cur_gd->pat_mode[mi] = atoi(line + 9);
						cur_gd->pat_has_mode[mi] = 1;
						continue;
					}
					if (strcmp(line, "=== edit_cmd_abs ===") == 0) {
						in_sect = 2;
						continue;
					}
					if (strcmp(line, "=== edit_cmd_relc ===") == 0) {
						in_sect = 4;
						continue;
					}
					if (strcmp(line, "=== edit_cmd_rel ===") == 0) {
						in_sect = 3;
						continue;
					}
					continue;
				}
				if (!cur_gd)
					continue;
				if (in_sect == 5) {
					if (line[0] == '-') {
						arr_append(&cur_gd->del_lines,
							   &cur_gd->ndel_lines,
							   &cur_gd->del_cap, line + 1);
					} else if (line[0] == '+') {
						arr_append(&cur_gd->add_lines,
							   &cur_gd->nadd_lines,
							   &cur_gd->add_cap, line + 1);
					}
					continue;
				}
				if (in_sect == 10) {
					cur_gd->strategy = strat_from_name(line, strlen(line));
					continue;
				}
				switch (in_sect) {
				case 1:
					arr_append(&cur_gd->pattern[pat_idx],
						   &cur_gd->npattern[pat_idx],
						   &cur_gd->pat_cap[pat_idx], line);
					break;
				case 2:
					arr_append(&cur_gd->abs_cmd,
						   &cur_gd->nabs,
						   &cur_gd->abs_cap, line);
					break;
				case 3:
					arr_append(&cur_gd->rel_cmd,
						   &cur_gd->nrel,
						   &cur_gd->rel_cap, line);
					break;
				case 4:
					arr_append(&cur_gd->relc_cmd,
						   &cur_gd->nrelc,
						   &cur_gd->relc_cap, line);
					break;
				case 6:
					arr_append(&cur_gd->pre_ctx,
						   &cur_gd->npre_ctx,
						   &cur_gd->pre_cap, line);
					break;
				case 7:
					arr_append(&cur_gd->post_ctx,
						   &cur_gd->npost_ctx,
						   &cur_gd->post_cap, line);
					break;
				case 8:
					arr_append(&cur_gd->custom_text,
						   &cur_gd->ncustom_text,
						   &cur_gd->custom_text_cap, line);
					break;
				}
			}
		} else {
			/* Not a script; store and process this first line */
			raw_lines[nraw++] = xstrdup(line);
			chomp(line);
			goto process_line;
		}
	}

	while (fgets(line, sizeof(line), in)) {
		raw_lines[nraw++] = xstrdup(line);
		chomp(line);
process_line:

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
			in_hunk = 0;
			continue;
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
				pending_orig_path = xstrdup(p);
			}
			continue;
		}

		/* Skip diff line */
		if (strncmp(line, "diff ", 5) == 0)
			continue;

		/* Skip index line */
		if (strncmp(line, "index ", 6) == 0)
			continue;

		/* Hunk header */
		int os, oc;
		if (parse_hunk_header(line, &os, &oc)) {
			in_hunk = 1;
			old_line = os;
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
			continue;
		}

		if (!in_hunk || nfiles == 0)
			continue;

		/* Process hunk content */
		if (line[0] == ' ') {
			/* Context line - store content for relative mode */
			add_op('c', old_line, line + 1);
			old_line++;
		} else if (line[0] == '-') {
			/* Delete line - store content for horizontal edit detection */
			add_op('d', old_line, line + 1);
			old_line++;
		} else if (line[0] == '+') {
			/* Add line */
			add_op('a', old_line, line + 1);
		} else if (line[0] == '\\') {
			/* "\ No newline at end of file" - skip */
			continue;
		} else {
			/* Unknown line in hunk */
			in_hunk = 0;
		}
	}

	if (in != stdin)
		fclose(in);

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

	/* Emit shell script header */
	fputs("#!/bin/sh -e\n# Generated by patch2vi from unified diff\n", stdout);
	list_unused_bytes(stdout);
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
	if ((relative_mode || interactive_mode) && dyn_esc)
		fputs("# Command that handles readability line breaks\n"
		      "LB=\"0?\"\n"
		      "# Phase 1 (search/mark): errors disabled by default,\n"
		      "# DBG1=1 enables error reporting, QF1=1 quits on failure\n"
		      "# OK1: with DBG1=1 also report fallback anchor successes\n"
		      "[ \"$DBG1\" = \"1\" ] && OK1= || OK1=\"0?\"\n"
		      "[ \"$DBG1\" = \"1\" ] && DBG1= || DBG1=\"0?\"\n"
		      "[ \"$QF1\" = \"1\" ] && QF1=\"${ESC}${SEP}vis 2${ESC}${SEP}q!1\" || QF1=\n"
		      "# Phase 2 (edits): DBG2=1 disables errors, QF2=1 ignores them\n"
		      "[ \"$DBG2\" = \"1\" ] && DBG2=\"0?\" || DBG2=\n"
		      "[ \"$QF2\" = \"1\" ] && QF2= || QF2=\"${ESC}${SEP}vis 2${ESC}${SEP}q!1\"\n"
		      "# Enters vi at failing code line in this script\n"
		      "# Designed for state inspection mid execution\n"
		      "[ \"$INTR\" = \"1\" ] && INTR=\"${ESC}${SEP}|sc|${ESC}${SEP}vis 2:0reg:e $0:83reg %@47:%f> %@p:&Q:b0:"
		      "|sc! ${ESC}${ESC}${ESC}${SEP}|:vis 3${ESC}${SEP}q1\" || INTR=\n", stdout);
	else if (relative_mode || interactive_mode)
		fputs("# Command that handles readability line breaks\n"
		      "LB=\"0?\"\n"
		      "# Phase 1 (search/mark): errors disabled by default,\n"
		      "# DBG1=1 enables error reporting, QF1=1 quits on failure\n"
		      "# OK1: with DBG1=1 also report fallback anchor successes\n"
		      "[ \"$DBG1\" = \"1\" ] && OK1= || OK1=\"0?\"\n"
		      "[ \"$DBG1\" = \"1\" ] && DBG1= || DBG1=\"0?\"\n"
		      "[ \"$QF1\" = \"1\" ] && QF1=\"\\\\${SEP}vis 2\\\\${SEP}q!1\" || QF1=\n"
		      "# Phase 2 (edits): DBG2=1 disables errors, QF2=1 ignores them\n"
		      "[ \"$DBG2\" = \"1\" ] && DBG2=\"0?\" || DBG2=\n"
		      "[ \"$QF2\" = \"1\" ] && QF2= || QF2=\"\\\\${SEP}vis 2\\\\${SEP}q!1\"\n"
		      "# Enters vi at failing code line in this script\n"
		      "# Designed for state inspection mid execution\n"
		      "[ \"$INTR\" = \"1\" ] && INTR=\"\\\\${SEP}|sc|\\\\${SEP}vis 2:0reg:e $0:83reg %@47:%f> %@p:&Q:b0:"
		      "|sc! \\\\\\\\\\\\${SEP}|:vis 3\\\\${SEP}q1\" || INTR=\n", stdout);

	/* Build groups for every file */
	for (int i = 0; i < nfiles; i++)
		build_file_groups(&files[i]);

	/* Collect files with groups */
	file_patch_t *active[256];
	int nactive = 0;
	for (int i = 0; i < nfiles; i++)
		if (files[i].ngroups > 0)
			active[nactive++] = &files[i];

	/* Interactive editing: one $EDITOR session for all files */
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
		      "if ( : > /tmp/p2vi.$$ ) 2>/dev/null; then\n"
		      "    P2VIF=/tmp/p2vi.$$\n"
		      "else\n"
		      "    P2VIF=./p2vi.$$\n"
		      "fi\n"
		      "trap 'rm -f \"$P2VIF\"' EXIT\n", stdout);
		if (dyn_esc)
			fputs("printf '%s\\n' \"|sc! ${ESC}${SEP}|:vis 3${SEP}", stdout);
		else
			fputs("printf '%s\\n' \"|sc! \\\\\\\\${SEP}|:vis 3${SEP}", stdout);
		/* default register <b> caches the buffer for f> searches */
		if (relative_mode || interactive_mode)
			fputs("98reg${SEP}", stdout);
		for (int k = 0; k < nactive; k++) {
			fprintf(stdout, "b%d${SEP}", k);
			/* nothing to cache or search in a brand new file */
			if ((relative_mode || interactive_mode) && !active[k]->is_new)
				fputs("%ya 98${SEP}", stdout);
			cur_file_path = active[k]->path;
			emit_file_script(stdout, active[k]);
		}
		fputs("vis 2${SEP}", stdout);
		/* Write all buffers at the end */
		for (int k = 0; k < nactive; k++)
			fprintf(stdout, "b%d${SEP}w${SEP}", k);
		fputs("2q\" > \"$P2VIF\"\n"
		      "EXINIT='%ya 97:? %@97' $VI -e", stdout);
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
		for (int j = 0; j < od->ngrps; j++)
			emit_grp_delta(stdout, &od->grps[j]);
		printf("%s\n", end_tag_wr);
	}
	printf("=== PATCH2VI PATCH ===\n");
	for (int i = 0; i < nraw; i++)
		fputs(raw_lines[i], stdout);

	return 0;
}
