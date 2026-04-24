/*
 * patch2vi - Convert unified diff patches to shell scripts using Nextvi ex commands
 *
 * Usage: patch2vi [input.patch]
 *
 * Uses raw ex mode (:vis 3) with dynamic separator selection via :sc!
 * to avoid conflicts with : % ! characters in patch content.
 *
 * The generated script uses EXINIT with ex commands to apply changes.
 * The user can then modify the script to add regex-based matching for
 * more robust patch application.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_LINE 8192
#define MAX_OPS 65536

typedef struct {
	int type;       /* 'd'=delete, 'a'=add, 'c'=context */
	int oline;      /* line number in original file */
	char *text;     /* line content (for add operations) */
} op_t;

typedef struct {
	char *path;
	op_t ops[MAX_OPS];
	int nops;
} file_patch_t;

static file_patch_t files[256];
static int nfiles = 0;
static int relative_mode = 0;  /* 0=absolute, 1=relative search (-r) */
static int interactive_mode = 0; /* 1=interactive editing of search patterns (-i) */
static int delta_mode = 0;      /* 1=re-apply previous delta from script (-d) */

/* Per-group delta: structured customizations from interactive editing */
typedef struct {
	int group_idx;      /* 1-based */
	int strategy;       /* STRAT_DEFAULT = not recorded */
	char *cmd;
	char **pattern;  int npattern, pat_cap;
	char **abs_cmd;  int nabs, abs_cap;
	char **rel_cmd;  int nrel, rel_cap;
	char **relc_cmd; int nrelc, relc_cap;
} grp_delta_t;

typedef struct {
	char *filepath;
	grp_delta_t *grps;
	int ngrps;
	int gcap;
} file_delta_t;

static file_delta_t out_deltas[256];   /* output: captured from editor */
static int nout_deltas = 0;

static file_delta_t in_deltas[256];    /* input: read from script */
static int nin_deltas = 0;

enum strategy {
	STRAT_DEFAULT = 0,  /* use global mode default */
	STRAT_ABS,          /* absolute line numbers (;c for single-line diffs) */
	STRAT_REL,          /* f> regex search (s// for single-line diffs) */
	STRAT_RELC,         /* f> regex search + ;c horizontal edit */
};

/* Raw input lines for embedding in output */
static char *raw_lines[MAX_OPS * 4];
static int nraw = 0;

/* Track which bytes appear in patch content */
static unsigned char byte_used[256];

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

/* Escape regex metacharacters for use in search pattern */
static char *escape_regex(const char *s)
{
	/* Metacharacters that need escaping in regex */
	const char *meta = "\\^$.*+?[](){}|";
	int len = strlen(s);
	int extra = 0;

	/* Count how many escapes needed */
	for (const char *p = s; *p; p++)
		if (strchr(meta, *p))
			extra++;

	char *result = malloc(len + extra + 1);
	char *dst = result;
	for (const char *p = s; *p; p++) {
		if (strchr(meta, *p))
			*dst++ = '\\';
		*dst++ = *p;
	}
	*dst = '\0';
	return result;
}

/* Append a string to a dynamic array */
static void arr_append(char ***arr, int *n, int *cap, const char *s)
{
	if (*n >= *cap) {
		*cap = *cap ? *cap * 2 : 4;
		*arr = realloc(*arr, *cap * sizeof(char *));
	}
	(*arr)[(*n)++] = xstrdup(s);
}

static int lines_equal(char **a, int na, char **b, int nb)
{
	if (na != nb) return 0;
	for (int i = 0; i < na; i++)
		if (strcmp(a[i], b[i]) != 0) return 0;
	return 1;
}


static grp_delta_t *find_grp_delta(file_delta_t *fd, int idx)
{
	for (int i = 0; fd && i < fd->ngrps; i++)
		if (fd->grps[i].group_idx == idx)
			return &fd->grps[i];
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
		if (old_diff_start > 0) {
			old_diff_start--;
			new_diff_start--;
			while (old_diff_start > 0 &&
			       (old[old_diff_start] & 0xC0) == 0x80) {
				old_diff_start--;
				new_diff_start--;
			}
		}
		if (old_diff_end < old_len) {
			old_diff_end++;
			new_diff_end++;
			while (old_diff_end < old_len &&
			       (old[old_diff_end] & 0xC0) == 0x80) {
				old_diff_end++;
				new_diff_end++;
			}
		}
		if (old_diff_end == old_diff_start)
			return 0;
	}

	/* Expand diff region until old_text is unique on the line.
	 * Since prefix and suffix are shared between old and new,
	 * expanding symmetrically keeps both regions aligned. */
	while (old_diff_end - old_diff_start > 0) {
		/* Build a temporary substring to check uniqueness */
		int dlen = old_diff_end - old_diff_start;
		char tmp[dlen + 1];
		memcpy(tmp, old + old_diff_start, dlen);
		tmp[dlen] = '\0';
		if (count_occurrences(old, tmp) <= 1)
			break;
		/* Expand: prefer left, then right */
		int expanded = 0;
		if (old_diff_start > 0) {
			old_diff_start--;
			new_diff_start--;
			/* Align to UTF-8 char boundary (skip continuation bytes) */
			while (old_diff_start > 0 &&
			       (old[old_diff_start] & 0xC0) == 0x80) {
				old_diff_start--;
				new_diff_start--;
			}
			expanded = 1;
		}
		/* Check if left expansion alone achieved uniqueness.
		 * Use a null-terminated copy of the expanded region. */
		int elen = old_diff_end - old_diff_start;
		char etmp[elen + 1];
		memcpy(etmp, old + old_diff_start, elen);
		etmp[elen] = '\0';
		if (!expanded || count_occurrences(old, etmp) > 1) {
			if (old_diff_end < old_len) {
				old_diff_end++;
				new_diff_end++;
				/* Skip past UTF-8 continuation bytes */
				while (old_diff_end < old_len &&
				       (old[old_diff_end] & 0xC0) == 0x80) {
					old_diff_end++;
					new_diff_end++;
				}
				expanded = 1;
			}
		}
		if (!expanded)
			break;
		if (old_diff_start == 0 && old_diff_end == old_len)
			break;
	}

	/* Recalculate diff lengths after expansion */
	old_diff_len = old_diff_end - old_diff_start;
	new_diff_len = new_diff_end - new_diff_start;

	/* Extract the old text */
	*old_text = malloc(old_diff_len + 1);
	memcpy(*old_text, old + old_diff_start, old_diff_len);
	(*old_text)[old_diff_len] = '\0';

	/* Extract the new text */
	*new_text = malloc(new_diff_len + 1);
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
/* escaped separator inside ??! block: \<sep> for ex_arg */
#define EMIT_ESCSEP(out) fputs("\\\\${SEP}", out)

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
 *
 * When emitting relative-mode positions (offset from search result),
 * +N / -N are equivalent to .+N / .-N since +/- default to current line.
 * Offset 0 emits . in search functions to avoid empty commands between
 * consecutive separators (which would trigger ec_print output).
 */

/* Emit ex commands for inserting text after line N */
static void emit_insert_after(FILE *out, int line, char **texts, int ntexts)
{
	if (ntexts == 0)
		return;

	if (line <= 0)
		fprintf(out, "1i ");
	else
		fprintf(out, "%da ", line);
	for (int i = 0; i < ntexts; i++) {
		emit_escaped_text(out, texts[i]);
		fputc('\n', out);
	}
	fputs(".\n", out);
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
static void emit_horizontal_change(FILE *out, int line, int char_start, int char_end,
					const char *new_text)
{
	if (char_start == char_end)
		fprintf(out, "%d;%dc ", line, char_start);
	else
		fprintf(out, "%d;%d;%dc ", line, char_start, char_end);
	emit_escaped_text(out, new_text);
	fputs("\n.\n", out);
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

	for (int i = 0; i < ntexts; i++) {
		emit_escaped_text(out, texts[i]);
		fputc('\n', out);
	}
	fputs(".\n", out);
	EMIT_SEP(out);
}

/*
 * Relative mode emit functions - use regex patterns instead of line numbers.
 *
 * Forward processing: groups are processed top-to-bottom.
 * Each f>/f+ search starts from current cursor position (xrow)
 * and finds the next occurrence forward. After each edit, the cursor
 * advances, so subsequent searches naturally find the correct occurrence.
 *
 * Single-line context: uses %f> (first) or .,$f+ (subsequent) for
 * single-line anchor search. No semicolon prefix.
 *
 * Multi-line context: when 2+ context lines are available, uses %;f>
 * (first) or .,$;f+ (subsequent) for multiline search (semicolon joins
 * lines for horizontal range matching).
 *
 * All search paths use ex_arg escaping uniformly via emit_escaped_regex_exarg.
 *
 * Error checking: each search is followed by ??! to detect failure,
 * print debug info, and quit before corrupting the file.
 */

typedef struct {
	char **anchors;
	int nanchors;
	char *follow_ctx;
	int follow_offset;
	int anchor_offset;   /* lines from last anchor to first change */
	int target_line;     /* original line number for error reporting */
} rel_ctx_t;

/* Emit ??! error check after a command that may fail. */
static void emit_err_check(FILE *out, int line)
{
	fputs("?" "?!${DBG:-", out);
	fprintf(out, "re p FAIL line %d", line);
	EMIT_ESCSEP(out);
	fprintf(out, "p FAIL line %d", line);
	fputs("${INTR}", out);
	fputs("${QF}}", out);
	EMIT_SEP(out);
}

/* Double backslashes for ex_arg level escaping.
 * ex_arg treats \\ as escaped \, so \\\\ is needed to preserve \\. */
static char *escape_exarg(const char *s)
{
	int len = strlen(s);
	int extra = 0;
	for (const char *p = s; *p; p++)
		if (*p == '\\') extra++;
	char *result = malloc(len + extra + 1);
	char *dst = result;
	for (const char *p = s; *p; p++) {
		if (*p == '\\')
			*dst++ = '\\';
		*dst++ = *p;
	}
	*dst = '\0';
	return result;
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

/* Write a regex-escaped string with ex_arg + shell escaping.
 * Three layers: regex metachar escaping → ex_arg \\ escaping → shell escaping.
 * Used for patterns parsed by ex_arg (f>/f+ multiline search). */
static void emit_escaped_regex_exarg(FILE *out, const char *s)
{
	char *regex_esc = escape_regex(s);
	char *exarg_esc = escape_exarg(regex_esc);
	emit_escaped_line(out, exarg_esc);
	free(exarg_esc);
	free(regex_esc);
}

/* Emit a single-line f>/f+ search with error check.
 * first=1 uses %f> (search whole file), first=0 uses .,$f> (current to end).
 * Uses emit_escaped_regex_exarg for uniform escaping (same as multiline path). */
static void emit_line_search(FILE *out, const char *pattern, int offset,
			      int first, int target_line)
{
	fprintf(out, "%sf> ", first ? "%" : ".,\\$");
	emit_escaped_regex_exarg(out, pattern);
	EMIT_SEP(out);
	emit_err_check(out, target_line);
	fputs(";=\n", out);
	EMIT_SEP(out);
	/* Emit offset; . needed at offset 0 to avoid empty command
	 * between consecutive separators (would trigger ec_print). */
	if (offset)
		fprintf(out, "%+d", offset);
	else
		fputc('.', out);
}

/* Emit multiline f>/f+ position using 2+ context lines.
 * first=1 uses %;f> (search whole file), first=0 uses .,$;f> (current to end) */
static void emit_multiline_pos(FILE *out, char **anchors, int nanchors,
				int offset, int first, int target_line)
{
	fprintf(out, "%s;f> ", first ? "%" : ".,\\$");
	for (int i = 0; i < nanchors; i++) {
		emit_escaped_regex_exarg(out, anchors[i]);
		if (i < nanchors - 1)
			fputc('\n', out);  /* literal newline between context lines */
	}
	/* Ensure trailing newline when last anchor is empty */
	if (nanchors > 0 && anchors[nanchors - 1][0] == '\0')
		fputc('\n', out);
	EMIT_SEP(out);
	emit_err_check(out, target_line);
	fputs(";=\n", out);
	EMIT_SEP(out);
	/* After f>, cursor is at match position; use +offset for target */
	if (offset)
		fprintf(out, "%+d", offset);
	else
		fputc('.', out);
}

typedef struct {
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
	char **custom_lines;     /* edited lines from $EDITOR (pre-escaped regex) */
	int ncustom;
	int custom_offset;       /* user-edited offset value */
	char *custom_cmd;        /* user-edited search command prefix */
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
} group_t;

/* Emit a line with exarg + shell escaping only (no regex escaping).
 * Used for user-edited regex lines in interactive mode. */
static void emit_escaped_exarg_only(FILE *out, const char *s)
{
	char *exarg_esc = escape_exarg(s);
	emit_escaped_line(out, exarg_esc);
	free(exarg_esc);
}

/* Emit multiline f>/f+ position using custom edited lines.
 * Lines are pre-escaped regex: apply exarg+shell escaping only.
 * If cmd is non-NULL, use it as the command prefix instead of the default. */
static void emit_custom_multiline_pos(FILE *out, char **lines, int nlines,
				      int offset, int first,
				      int target_line, const char *cmd)
{
	if (cmd)
		fprintf(out, "%s ", cmd);
	else
		fprintf(out, "%s;f> ", first ? "%" : ".,\\$");
	for (int i = 0; i < nlines; i++) {
		emit_escaped_exarg_only(out, lines[i]);
		if (i < nlines - 1)
			fputc('\n', out);
	}
	/* Ensure trailing newline when last line is empty */
	if (nlines > 0 && !lines[nlines - 1][0])
		fputc('\n', out);
	EMIT_SEP(out);
	emit_err_check(out, target_line);
	fputs(";=\n", out);
	EMIT_SEP(out);
	if (offset)
		fprintf(out, "%+d", offset);
	else
		fputc('.', out);
}

/* Emit single-line custom f> search with error check.
 * Line is pre-escaped regex: apply exarg+shell escaping only. */
static void emit_custom_line_search(FILE *out, const char *line, int offset,
				     int first, int target_line)
{
	fprintf(out, "%sf> ", first ? "%" : ".,\\$");
	emit_escaped_exarg_only(out, line);
	EMIT_SEP(out);
	emit_err_check(out, target_line);
	fputs(";=\n", out);
	EMIT_SEP(out);
	if (offset)
		fprintf(out, "%+d", offset);
	else
		fputc('.', out);
}

/* Emit positioning for a custom-edited group.
 * Returns: 0 = unused, 1 = two-command (search then offset). */
static int emit_custom_pos(FILE *out, group_t *g, int *first_ml)
{
	int target_line = g->del_start ? g->del_start : g->add_after;
	/* Custom command forces multiline-style emission (f> path) */
	if (g->custom_cmd) {
		emit_custom_multiline_pos(out, g->custom_lines, g->ncustom,
					  g->custom_offset,
					  *first_ml, target_line, g->custom_cmd);
		*first_ml = 0;
		return 1;
	}
	if (g->ncustom >= 2 ||
	    (g->ncustom == 1 && !g->custom_lines[0][0])) {
		emit_custom_multiline_pos(out, g->custom_lines, g->ncustom,
					  g->custom_offset,
					  *first_ml, target_line, NULL);
		*first_ml = 0;
		return 1;
	}
	if (g->ncustom == 1) {
		emit_custom_line_search(out, g->custom_lines[0],
					g->custom_offset,
					*first_ml, target_line);
		*first_ml = 0;
		return 1;
	}
	return -1;
}

/*
 * Emit the search/positioning part for a relative group.
 * Returns: 0 = single-command positioning (pos is part of final cmd),
 *          1 = two-command positioning (f>/f+ then .+offset as separate cmd)
 *
 * For single-line: emits "%f> pattern" or ".,$f+ pattern" with error check
 * For multiline:   emits "%;f>ctx1\nctx2" (first) or ";f+" (subsequent)
 * For follow ctx:  emits "%f> follow" or ".,$f> follow" with negative offset
 *
 * *first_ml: pointer to flag, 1 for first search (uses %f>),
 *            cleared to 0 after first use (subsequent use .,$f>)
 */
static int emit_rel_pos(FILE *out, rel_ctx_t *rc, int *first_ml)
{
	if (rc->nanchors >= 2) {
		/* Multiline: f> search, then .+offset as position for next command */
		int offset = rc->nanchors + rc->anchor_offset - 1;
		emit_multiline_pos(out, rc->anchors, rc->nanchors, offset, *first_ml, rc->target_line);
		*first_ml = 0;
		return 1;
	}
	if (rc->nanchors == 1 && rc->anchors[0] && rc->anchors[0][0]) {
		emit_line_search(out, rc->anchors[0], rc->anchor_offset,
				 *first_ml, rc->target_line);
		*first_ml = 0;
		return 1;
	}
	if (rc->follow_ctx && rc->follow_ctx[0]) {
		emit_line_search(out, rc->follow_ctx, -(rc->follow_offset),
				 *first_ml, rc->target_line);
		*first_ml = 0;
		return 1;
	}
	return -1;  /* no anchor available */
}

/* Emit delete using relative pattern */
static void emit_relative_delete(FILE *out, rel_ctx_t *rc, int count,
				  int *first_ml)
{
	int mode = emit_rel_pos(out, rc, first_ml);
	if (count == 1)
		fputc('d', out);
	else
		fprintf(out, ",#+%dd", count - 1);
	EMIT_SEP(out);
	if (mode == 0)
		emit_err_check(out, rc->target_line);
}

/* Emit insert using relative pattern */
static void emit_relative_insert(FILE *out, rel_ctx_t *rc,
				  char **texts, int ntexts, int *first_ml)
{
	if (ntexts == 0)
		return;

	int mode = emit_rel_pos(out, rc, first_ml);
	fprintf(out, "a ");
	for (int i = 0; i < ntexts; i++) {
		emit_escaped_text(out, texts[i]);
		fputc('\n', out);
	}
	fputs(".\n", out);
	EMIT_SEP(out);
	if (mode == 0)
		emit_err_check(out, rc->target_line);
}

/* Emit change using relative pattern */
static void emit_relative_change(FILE *out, rel_ctx_t *rc,
				  int del_count, char **texts, int ntexts,
				  int *first_ml)
{
	if (ntexts == 0) {
		emit_relative_delete(out, rc, del_count, first_ml);
		return;
	}

	int mode = emit_rel_pos(out, rc, first_ml);
	if (del_count == 1)
		fprintf(out, "c ");
	else
		fprintf(out, ",#+%dc ", del_count - 1);

	for (int i = 0; i < ntexts; i++) {
		emit_escaped_text(out, texts[i]);
		fputc('\n', out);
	}
	fputs(".\n", out);
	EMIT_SEP(out);
	if (mode == 0)
		emit_err_check(out, rc->target_line);
}

/* Escape replacement text for substitute command.
 * In nextvi :s replacement, only \ is special (for backreferences \0-\9).
 * \ must be doubled to produce a literal \. The delimiter must also be escaped.
 * Returns allocated string with re_read-level escaping (no ex_arg/shell yet). */
static char *escape_sub_repl(const char *s, char delim)
{
	int len = strlen(s);
	int extra = 0;
	for (const char *p = s; *p; p++)
		if (*p == '\\' || *p == delim)
			extra++;
	char *result = malloc(len + extra + 1);
	char *dst = result;
	for (const char *p = s; *p; p++) {
		if (*p == '\\' || *p == delim)
			*dst++ = '\\';
		*dst++ = *p;
	}
	*dst = '\0';
	return result;
}

/* Escape regex pattern for substitute command.
 * Like escape_regex() but also escapes the delimiter for re_read. */
static char *escape_sub_pat(const char *s, char delim)
{
	const char *meta = "\\^$.*+?[](){}|";
	int len = strlen(s);
	int extra = 0;
	for (const char *p = s; *p; p++) {
		if (strchr(meta, *p))
			extra++;
		else if (*p == delim)
			extra++;
	}
	char *result = malloc(len + extra + 1);
	char *dst = result;
	for (const char *p = s; *p; p++) {
		if (strchr(meta, *p))
			*dst++ = '\\';
		else if (*p == delim)
			*dst++ = '\\';
		*dst++ = *p;
	}
	*dst = '\0';
	return result;
}

/* Emit the s/old/new/ substitute command (no positioning).
 * Escapes old_text as regex pattern and new_text as replacement,
 * both through re_read delimiter + ex_arg + shell layers. */
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

/* Emit substitute using relative pattern positioning */
static void emit_relative_substitute(FILE *out, rel_ctx_t *rc,
				      const char *old_text,
				      const char *new_text, int *first_ml)
{
	int mode = emit_rel_pos(out, rc, first_ml);
	/* Separate position from substitute: s/ doesn't move xrow,
	 * so make position a standalone command to advance cursor first */
	EMIT_SEP(out);
	if (mode == 0)
		emit_err_check(out, rc->target_line);
	emit_substitute_cmd(out, old_text, new_text);
	EMIT_SEP(out);
	emit_err_check(out, rc->target_line);
}

/* Parse and strip relative offset prefix from custom edit lines (rel/relc).
 * "+N" or "-N" alone as lines[0]: offset-only line (substitute with offset).
 *   Extract offset, remove lines[0], shift remaining lines down, decrement *nlines.
 * "+N" or "-N" followed immediately by verb (e.g. "+3a text"): embedded prefix.
 *   Extract offset number, strip prefix from lines[0] in-place.
 * Returns the extracted offset (0 if no prefix found). */
static int parse_ecmd_offset(char **lines, int *nlines)
{
	if (*nlines == 0) return 0;
	char *first = lines[0];
	if (first[0] != '+' && first[0] != '-') return 0;
	int i = 1;
	while (first[i] >= '0' && first[i] <= '9') i++;
	if (i == 1) return 0; /* sign but no digits */
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

/* Per-group parsed data from an interactive temp file (raw, no offset stripping) */
typedef struct {
	int strategy;
	char *cmd;
	char **pattern;  int npattern, pat_cap;
	char **abs_cmd;  int nabs, abs_cap;
	char **rel_cmd;  int nrel, rel_cap;
	char **relc_cmd; int nrelc, relc_cap;
} parsed_grp_t;

static void free_parsed_grp(parsed_grp_t *p)
{
	free(p->cmd);
	for (int i = 0; i < p->npattern; i++) free(p->pattern[i]);
	free(p->pattern);
	for (int i = 0; i < p->nabs; i++) free(p->abs_cmd[i]);
	free(p->abs_cmd);
	for (int i = 0; i < p->nrel; i++) free(p->rel_cmd[i]);
	free(p->rel_cmd);
	for (int i = 0; i < p->nrelc; i++) free(p->relc_cmd[i]);
	free(p->relc_cmd);
}

/*
 * Parse an interactive group temp file into results[0..ngroups-1].
 * Stores raw content (no parse_ecmd_offset stripping) for apples-to-apples
 * comparison between .orig and edited files.
 */
static void parse_tmp_file(const char *path, parsed_grp_t *results, int ngroups)
{
	FILE *f = fopen(path, "r");
	if (!f) return;

	memset(results, 0, ngroups * sizeof(parsed_grp_t));

	char line[MAX_LINE];
	int gi = -1;
	int in_pat = 0, in_cstrat = 0, in_ecmd = 0;
	int ecmd_strat = STRAT_DEFAULT;
	int cs_take = 0, cs_want_cmd = 0;
	int cs_strat = STRAT_DEFAULT;
	int skip_extra = 0;

	while (fgets(line, sizeof(line), f)) {
		chomp(line);

		if (strncmp(line, "=== ", 4) == 0) {
			in_ecmd = 0;
			if (in_pat) { in_pat = 0; }
			in_cstrat = 0;
			cs_take = 0; cs_want_cmd = 0;
		}

		if (strncmp(line, "=== GROUP ", 10) == 0) {
			gi = atoi(line + 10) - 1;
			skip_extra = 0;
			continue;
		}
		if (strncmp(line, "=== COMMAND STRATEGY", 20) == 0) {
			in_cstrat = 1; continue;
		}
		if (strncmp(line, "=== SEARCH PATTERN", 18) == 0) {
			in_pat = 1; skip_extra = 0; continue;
		}
		if (strncmp(line, "=== EDIT COMMAND (", 18) == 0) {
			in_ecmd = 1;
			const char *p = line + 18;
			if (strncmp(p, "abs)", 4) == 0) ecmd_strat = STRAT_ABS;
			else if (strncmp(p, "relc)", 5) == 0) ecmd_strat = STRAT_RELC;
			else if (strncmp(p, "rel)", 4) == 0) ecmd_strat = STRAT_REL;
			else ecmd_strat = STRAT_DEFAULT;
			continue;
		}

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
			if (cs_want_cmd) {
				if (cs_take && gi >= 0 && gi < ngroups) {
					parsed_grp_t *pg = &results[gi];
					if (pg->strategy == STRAT_DEFAULT)
						pg->strategy = cs_strat;
					free(pg->cmd);
					pg->cmd = xstrdup(line);
				}
				cs_want_cmd = 0; cs_take = 0;
			} else {
				const char *name = (line[0] == '#') ? line + 1 : line;
				cs_take = (line[0] != '#');
				if (strcmp(name, "abs") == 0) {
					if (cs_take && gi >= 0 && gi < ngroups
					    && results[gi].strategy == STRAT_DEFAULT)
						results[gi].strategy = STRAT_ABS;
					cs_strat = STRAT_ABS;
					cs_take = 0; cs_want_cmd = 0;
				} else if (strcmp(name, "rel") == 0) {
					cs_strat = STRAT_REL;
					cs_want_cmd = 1;
				} else if (strcmp(name, "relc") == 0) {
					cs_strat = STRAT_RELC;
					cs_want_cmd = 1;
				} else {
					cs_take = 0; cs_want_cmd = 0;
				}
			}
			continue;
		}

		if (in_pat && gi >= 0 && gi < ngroups) {
			if (strncmp(line, "--- extra", 9) == 0) {
				skip_extra = 1;
				in_pat = 0;
				continue;
			}
			if (!skip_extra) {
				parsed_grp_t *pg = &results[gi];
				arr_append(&pg->pattern, &pg->npattern, &pg->pat_cap, line);
			}
		}
	}

	fclose(f);
}

/* Emit one group's delta in human-readable structured format */
static void emit_grp_delta(FILE *out, grp_delta_t *gd)
{
	fprintf(out, "GROUP %d\n", gd->group_idx);
	if (gd->strategy != STRAT_DEFAULT) {
		const char *s = "abs";
		if (gd->strategy == STRAT_REL) s = "rel";
		else if (gd->strategy == STRAT_RELC) s = "relc";
		fprintf(out, "strategy: %s\n", s);
	}
	if (gd->cmd)
		fprintf(out, "cmd: %s\n", gd->cmd);
	if (gd->npattern > 0) {
		fputs("pattern:\n", out);
		for (int i = 0; i < gd->npattern; i++)
			fprintf(out, "%s\n", gd->pattern[i]);
	}
	if (gd->nabs > 0) {
		fputs("edit_cmd_abs:\n", out);
		for (int i = 0; i < gd->nabs; i++)
			fprintf(out, "%s\n", gd->abs_cmd[i]);
	}
	if (gd->nrelc > 0) {
		fputs("edit_cmd_relc:\n", out);
		for (int i = 0; i < gd->nrelc; i++)
			fprintf(out, "%s\n", gd->relc_cmd[i]);
	}
	if (gd->nrel > 0) {
		fputs("edit_cmd_rel:\n", out);
		for (int i = 0; i < gd->nrel; i++)
			fprintf(out, "%s\n", gd->rel_cmd[i]);
	}
}

/*
 * Write all groups to fp, optionally injecting stored delta from in_fd.
 * Returns a freshly allocated default_cmds[] array (caller frees entries + array).
 */
static char **write_groups_to_file(FILE *fp, group_t *groups, int ngroups,
				    file_delta_t *in_fd)
{
	int sim_first_ml = 1;
	char **default_cmds = calloc(ngroups, sizeof(char *));

	for (int gi = 0; gi < ngroups; gi++) {
		group_t *g = &groups[gi];
		if (!g->del_start && !g->nadd)
			continue;
		int target = g->del_start ? g->del_start : g->add_after;

		grp_delta_t *gd = find_grp_delta(in_fd, gi + 1);

		int has_anchors = g->nanchors >= 2
			|| (g->nanchors == 1 && g->anchors[0] && g->anchors[0][0])
			|| (g->follow_ctx && g->follow_ctx[0])
			|| (g->ndel > 0 && g->del_texts[0] && g->del_texts[0][0]);

		int default_offset;
		const char *dcmd = NULL;
		if (g->nanchors >= 2) {
			default_offset = g->nanchors + g->anchor_offset - 1;
			dcmd = sim_first_ml ? "%;f>" : ".,\\$;f>";
			sim_first_ml = 0;
		} else if (g->nanchors == 1) {
			default_offset = g->anchor_offset;
			dcmd = sim_first_ml ? "%f>" : ".,\\$f>";
			sim_first_ml = 0;
		} else if ((g->follow_ctx && g->follow_ctx[0])
			   || (g->ndel > 0 && g->del_texts[0]
			       && g->del_texts[0][0])) {
			default_offset = g->block_change_idx;
			dcmd = sim_first_ml ? "%f>" : ".,\\$f>";
			sim_first_ml = 0;
		} else {
			default_offset = g->block_change_idx;
		}

		char abs_cmd[64] = "";
		{
			int from = g->del_start, to = g->del_end, after = g->add_after;
			if (from && g->nadd) {
				if (from == to)
					snprintf(abs_cmd, sizeof(abs_cmd), "%dc", from);
				else
					snprintf(abs_cmd, sizeof(abs_cmd), "%d,%dc", from, to);
			} else if (from) {
				if (from == to)
					snprintf(abs_cmd, sizeof(abs_cmd), "%dd", from);
				else
					snprintf(abs_cmd, sizeof(abs_cmd), "%d,%dd", from, to);
			} else if (g->nadd) {
				if (after == -1)
					snprintf(abs_cmd, sizeof(abs_cmd), "i");
				else
					snprintf(abs_cmd, sizeof(abs_cmd), "%da", after);
			}
		}

		const char *def_strat = has_anchors ? "rel" : "abs";
		const char *show_cmd = dcmd ? dcmd : abs_cmd[0] ? abs_cmd : "";
		default_cmds[gi] = show_cmd[0] ? xstrdup(show_cmd) : NULL;

		/* Group header */
		fprintf(fp, "=== GROUP %d/%d (line %d) ===\n",
			gi + 1, ngroups, target);
		for (int i = 0; i < g->ndel; i++)
			fprintf(fp, "-%s\n", g->del_texts[i]);
		for (int i = 0; i < g->nadd; i++)
			fprintf(fp, "+%s\n", g->add_texts[i]);

		/* COMMAND STRATEGY: inject stored strategy or keep all commented */
		int sel_strat = (gd && gd->strategy != STRAT_DEFAULT)
			? gd->strategy : STRAT_DEFAULT;
		const char *use_cmd = (gd && gd->cmd) ? gd->cmd
			: dcmd ? dcmd : "";
		fprintf(fp, "=== COMMAND STRATEGY (default: %s) ===\n", def_strat);
		fprintf(fp, "%sabs\n", sel_strat == STRAT_ABS ? "" : "#");
		if (has_anchors && g->ndel == 1 && g->nadd == 1 && g->has_line_diff)
			fprintf(fp, "%srelc\n%s\n",
				sel_strat == STRAT_RELC ? "" : "#", use_cmd);
		if (has_anchors)
			fprintf(fp, "%srel\n%s\n",
				sel_strat == STRAT_REL ? "" : "#", use_cmd);

		/* SEARCH PATTERN: inject stored or auto-generate */
		fprintf(fp, "=== SEARCH PATTERN ===\n");
		if (gd && gd->npattern > 0) {
			for (int i = 0; i < gd->npattern; i++)
				fprintf(fp, "%s\n", gd->pattern[i]);
		} else {
			for (int i = 0; i < g->nanchors; i++) {
				char *esc = escape_regex(g->anchors[i]);
				fprintf(fp, "%s\n", esc);
				free(esc);
			}
		}
		int has_extra = g->ndel > 0 || g->npost_ctx > 0;
		if (has_extra) {
			int show_sep = (gd && gd->npattern > 0) || g->nanchors > 0;
			if (show_sep)
				fprintf(fp, "--- extra (delete to include) ---\n");
			for (int i = 0; i < g->ndel; i++) {
				char *esc = escape_regex(g->del_texts[i]);
				fprintf(fp, "%s\n", esc);
				free(esc);
			}
			for (int i = 0; i < g->npost_ctx; i++) {
				char *esc = escape_regex(g->post_ctx[i]);
				fprintf(fp, "%s\n", esc);
				free(esc);
			}
		}

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
				if (g->add_after <= 0)
					fputs("i", fp);
				else
					fprintf(fp, "%da", g->add_after);
				WG_CONTENT(fp);
			}
		}

		/* relc */
		int show_relc = has_anchors && g->ndel == 1 && g->nadd == 1 && g->has_line_diff;
		if (show_relc || (gd && gd->nrelc > 0)) {
			fputs("=== EDIT COMMAND (relc) ===\n", fp);
			if (gd && gd->nrelc > 0) {
				for (int k = 0; k < gd->nrelc; k++)
					fprintf(fp, "%s\n", gd->relc_cmd[k]);
			} else if (show_relc) {
				if (default_offset != 0)
					fprintf(fp, "%+d\n", default_offset);
				if (g->ldc_start == g->ldc_end)
					fprintf(fp, ".;%dc %s\n",
						g->ldc_start, g->ldc_new_text);
				else
					fprintf(fp, ".;%d;%dc %s\n",
						g->ldc_start, g->ldc_end,
						g->ldc_new_text);
			}
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
					if (default_offset != 0)
						fprintf(fp, "%+d\n", default_offset);
					fprintf(fp, "s/%s/%s/\n", esc_pat, esc_rep);
					free(esc_pat);
					free(esc_rep);
				} else if (g->del_start && g->nadd) {
					if (default_offset != 0)
						fprintf(fp, "%+d", default_offset);
					if (g->ndel == 1)
						fputs("c", fp);
					else
						fprintf(fp, ",#+%dc", g->ndel - 1);
					WG_CONTENT(fp);
				} else if (g->del_start) {
					if (default_offset != 0)
						fprintf(fp, "%+d", default_offset);
					if (g->ndel == 1)
						fputs("d\n", fp);
					else
						fprintf(fp, ",#+%dd\n", g->ndel - 1);
				} else if (g->nadd) {
					int aoff = default_offset - 1;
					if (aoff >= 0) {
						if (aoff)
							fprintf(fp, "%+d", aoff);
						fputs("a", fp);
					} else {
						fputs("i", fp);
					}
					WG_CONTENT(fp);
				}
			}
		}
#undef WG_CONTENT
		fprintf(fp, "=== END GROUP ===\n\n");
	}
	return default_cmds;
}

/*
 * Interactive editing of all groups' search patterns in one file.
 * Opens $EDITOR once with all groups. Pattern lines are shown
 * regex-escaped (as they'll appear to the regex engine).
 * If file saved: all groups use the edited patterns.
 * If file not saved (mtime unchanged): all groups use default behavior.
 */
static void interactive_edit_groups(group_t *groups, int ngroups,
				    const char *filepath)
{
	if (ngroups == 0)
		return;

	/* Extract basename from filepath for the temp filename */
	const char *base = strrchr(filepath, '/');
	base = base ? base + 1 : filepath;
	char tmptmp[32];
	char tmppath[256];
	char tmppath_orig[262];
	char rejpath[260];
	snprintf(tmptmp, sizeof(tmptmp), "patch2vi_XXXXXX");
	int fd = mkstemp(tmptmp);
	snprintf(tmppath, sizeof(tmppath), "%s_%s.diff", tmptmp, base);
	if (rename(tmptmp, tmppath) < 0)
		snprintf(tmppath, sizeof(tmppath), "%s", tmptmp);
	if (fd < 0) {
		perror("mkstemp");
		return;
	}
	snprintf(tmppath_orig, sizeof(tmppath_orig), "%s.orig", tmppath);
	snprintf(rejpath, sizeof(rejpath), "%s.rej", tmppath);

	/* Find stored delta for this file */
	file_delta_t *in_fd = NULL;
	if (delta_mode) {
		for (int di = 0; di < nin_deltas; di++)
			if (strcmp(in_deltas[di].filepath, filepath) == 0) {
				in_fd = &in_deltas[di];
				break;
			}
	}

	/* Write auto-generated baseline (no injection) for later comparison */
	{
		FILE *orig_fp = fopen(tmppath_orig, "w");
		if (orig_fp) {
			char **dc = write_groups_to_file(orig_fp, groups, ngroups, NULL);
			fclose(orig_fp);
			for (int i = 0; i < ngroups; i++) free(dc[i]);
			free(dc);
		}
	}

	/* Write editor file with stored delta injected */
	char **default_cmds;
	{
		FILE *tmp = fdopen(fd, "w");
		if (!tmp) {
			perror("fdopen");
			close(fd);
			unlink(tmppath);
			unlink(tmppath_orig);
			return;
		}
		default_cmds = write_groups_to_file(tmp, groups, ngroups, in_fd);
		fclose(tmp);
	}

	/* Write rejection file for stored groups whose index exceeds ngroups */
	int delta_rejected = 0;
	if (in_fd) {
		FILE *rej = NULL;
		for (int k = 0; k < in_fd->ngrps; k++) {
			if (in_fd->grps[k].group_idx > ngroups) {
				if (!rej) {
					rej = fopen(rejpath, "w");
					if (rej)
						fprintf(rej,
							"# Rejected: index out of range"
							" (%d groups in current file)\n\n",
							ngroups);
				}
				if (rej)
					emit_grp_delta(rej, &in_fd->grps[k]);
				delta_rejected = 1;
			}
		}
		if (rej)
			fclose(rej);
	}

	/* Record mtime before editor */
	struct stat st_before;
	if (stat(tmppath, &st_before) < 0) {
		perror("stat");
		goto cleanup_orig;
	}

	/* Open editor */
	{
		const char *editor = getenv("EDITOR");
		if (!editor)
			editor = getenv("VISUAL");
		if (!editor)
			editor = "vi";
		char cmd[MAX_LINE];
		if (delta_rejected)
			snprintf(cmd, sizeof(cmd),
				 "%s '%s' '%s' </dev/tty >/dev/tty",
				 editor, tmppath, rejpath);
		else
			snprintf(cmd, sizeof(cmd),
				 "%s '%s' </dev/tty >/dev/tty",
				 editor, tmppath);
		int ret = system(cmd);
		if (delta_rejected)
			unlink(rejpath);
		if (ret != 0) {
			fprintf(stderr, "editor exited with error %d\n", ret);
			goto cleanup_orig;
		}
	}

	/* Check if file was saved (mtime changed) */
	{
		struct stat st_after;
		if (stat(tmppath, &st_after) < 0) {
			perror("stat");
			goto cleanup_orig;
		}
		if (st_before.st_mtime == st_after.st_mtime) {
			/* User quit without saving. Preserve stored delta if present
			 * so next -d run still injects the previous customizations. */
			if (!in_fd || in_fd->ngrps == 0)
				goto cleanup_orig;
			file_delta_t *od = &out_deltas[nout_deltas++];
			od->filepath = xstrdup(filepath);
			od->gcap = in_fd->ngrps;
			od->grps = malloc(od->gcap * sizeof(grp_delta_t));
			od->ngrps = in_fd->ngrps;
			for (int k = 0; k < in_fd->ngrps; k++) {
				grp_delta_t *src = &in_fd->grps[k];
				grp_delta_t *dst = &od->grps[k];
				memset(dst, 0, sizeof(*dst));
				dst->group_idx = src->group_idx;
				dst->strategy = src->strategy;
				if (src->cmd) dst->cmd = xstrdup(src->cmd);
				for (int j = 0; j < src->npattern; j++)
					arr_append(&dst->pattern, &dst->npattern,
						   &dst->pat_cap, src->pattern[j]);
				for (int j = 0; j < src->nabs; j++)
					arr_append(&dst->abs_cmd, &dst->nabs,
						   &dst->abs_cap, src->abs_cmd[j]);
				for (int j = 0; j < src->nrel; j++)
					arr_append(&dst->rel_cmd, &dst->nrel,
						   &dst->rel_cap, src->rel_cmd[j]);
				for (int j = 0; j < src->nrelc; j++)
					arr_append(&dst->relc_cmd, &dst->nrelc,
						   &dst->relc_cap, src->relc_cmd[j]);
			}
			goto read_back;
		}
	}

	/* Compute structured delta: compare .orig (auto-generated baseline)
	 * against edited file section by section. Only store changed groups. */
	{
		parsed_grp_t *orig_grps = calloc(ngroups, sizeof(parsed_grp_t));
		parsed_grp_t *edit_grps = calloc(ngroups, sizeof(parsed_grp_t));
		parse_tmp_file(tmppath_orig, orig_grps, ngroups);
		parse_tmp_file(tmppath, edit_grps, ngroups);

		file_delta_t *od = NULL;
		for (int gi = 0; gi < ngroups; gi++) {
			parsed_grp_t *og = &orig_grps[gi];
			parsed_grp_t *eg = &edit_grps[gi];

			int strat_ch = (eg->strategy != og->strategy);
			int cmd_ch = (eg->cmd != og->cmd) &&
				((eg->cmd == NULL) != (og->cmd == NULL) ||
				 (eg->cmd && og->cmd &&
				  strcmp(eg->cmd, og->cmd) != 0));
			int pat_ch = !lines_equal(eg->pattern, eg->npattern,
						  og->pattern, og->npattern);
			int abs_ch = !lines_equal(eg->abs_cmd, eg->nabs,
						  og->abs_cmd, og->nabs);
			int rel_ch = !lines_equal(eg->rel_cmd, eg->nrel,
						  og->rel_cmd, og->nrel);
			int relc_ch = !lines_equal(eg->relc_cmd, eg->nrelc,
						   og->relc_cmd, og->nrelc);

			if (!strat_ch && !cmd_ch && !pat_ch &&
			    !abs_ch && !rel_ch && !relc_ch)
				continue;

			if (!od) {
				od = &out_deltas[nout_deltas++];
				od->filepath = xstrdup(filepath);
				od->grps = NULL;
				od->ngrps = 0;
				od->gcap = 0;
			}
			if (od->ngrps >= od->gcap) {
				od->gcap = od->gcap ? od->gcap * 2 : 4;
				od->grps = realloc(od->grps,
					od->gcap * sizeof(grp_delta_t));
			}
			grp_delta_t *gout = &od->grps[od->ngrps++];
			memset(gout, 0, sizeof(*gout));
			gout->group_idx = gi + 1;
			if (strat_ch) gout->strategy = eg->strategy;
			if (cmd_ch && eg->cmd) gout->cmd = xstrdup(eg->cmd);
			for (int j = 0; pat_ch && j < eg->npattern; j++)
				arr_append(&gout->pattern, &gout->npattern,
					   &gout->pat_cap, eg->pattern[j]);
			for (int j = 0; abs_ch && j < eg->nabs; j++)
				arr_append(&gout->abs_cmd, &gout->nabs,
					   &gout->abs_cap, eg->abs_cmd[j]);
			for (int j = 0; rel_ch && j < eg->nrel; j++)
				arr_append(&gout->rel_cmd, &gout->nrel,
					   &gout->rel_cap, eg->rel_cmd[j]);
			for (int j = 0; relc_ch && j < eg->nrelc; j++)
				arr_append(&gout->relc_cmd, &gout->nrelc,
					   &gout->relc_cap, eg->relc_cmd[j]);
		}

		for (int gi = 0; gi < ngroups; gi++) {
			free_parsed_grp(&orig_grps[gi]);
			free_parsed_grp(&edit_grps[gi]);
		}
		free(orig_grps);
		free(edit_grps);
	}

read_back:
	/* Read back all groups from the edited file */
	{
		FILE *rd = fopen(tmppath, "r");
		if (!rd) {
			perror("fopen");
			goto cleanup_orig;
		}

		char line[MAX_LINE];
		int cur_gi = -1;
		int in_pattern = 0, in_cmdstrat = 0, in_edit_cmd = 0;
		int cmdstrat_want_cmd = 0, cmdstrat_take = 0;
		int cmdstrat_strat = STRAT_DEFAULT;
		int edit_cmd_strat = STRAT_DEFAULT;
		int skip_extra = 0;
		char edited_cmd[MAX_LINE] = "";
		int edited_offset = 0;
		int edited_strategy = STRAT_DEFAULT;
		char **ecmd_lines = NULL;
		int necmd = 0, ecmd_cap = 0;
		char **lines = NULL;
		int nlines = 0, line_cap = 0;

#define FLUSH_ECMD() do { \
		if (necmd > 0 && cur_gi >= 0 && cur_gi < ngroups) { \
			group_t *_eg = &groups[cur_gi]; \
			switch (edit_cmd_strat) { \
			case STRAT_ABS: \
				_eg->custom_abs_lines = ecmd_lines; \
				_eg->custom_abs_nlines = necmd; \
				break; \
			case STRAT_RELC: \
				_eg->custom_offset = parse_ecmd_offset(ecmd_lines, &necmd); \
				_eg->custom_relc_lines = ecmd_lines; \
				_eg->custom_relc_nlines = necmd; \
				break; \
			case STRAT_REL: \
				_eg->custom_offset = parse_ecmd_offset(ecmd_lines, &necmd); \
				_eg->custom_rel_lines = ecmd_lines; \
				_eg->custom_rel_nlines = necmd; \
				break; \
			default: \
				for (int _j = 0; _j < necmd; _j++) free(ecmd_lines[_j]); \
				free(ecmd_lines); \
				break; \
			} \
		} else { \
			for (int _j = 0; _j < necmd; _j++) free(ecmd_lines[_j]); \
			free(ecmd_lines); \
		} \
		ecmd_lines = NULL; necmd = 0; ecmd_cap = 0; \
} while (0)

		while (fgets(line, sizeof(line), rd)) {
			chomp(line);

			if (strncmp(line, "=== ", 4) == 0 && in_edit_cmd) {
				FLUSH_ECMD();
				in_edit_cmd = 0;
			}

			if (strncmp(line, "=== GROUP ", 10) == 0) {
				if (cur_gi >= 0 && cur_gi < ngroups) {
					groups[cur_gi].strategy = edited_strategy;
					const char *orig = default_cmds[cur_gi]
						? default_cmds[cur_gi] : "";
					if (strcmp(edited_cmd, orig) != 0 && edited_cmd[0])
						groups[cur_gi].custom_cmd =
							xstrdup(edited_cmd);
					if (nlines > 0) {
						groups[cur_gi].custom_lines = lines;
						groups[cur_gi].ncustom = nlines;
						if (edited_offset != 0)
							groups[cur_gi].custom_offset =
								edited_offset;
						lines = NULL;
						nlines = 0;
						line_cap = 0;
					}
				}
				cur_gi = atoi(line + 10) - 1;
				in_pattern = 0;
				in_cmdstrat = 0;
				cmdstrat_want_cmd = 0;
				cmdstrat_take = 0;
				skip_extra = 0;
				edited_cmd[0] = '\0';
				edited_offset = 0;
				edited_strategy = STRAT_DEFAULT;
				continue;
			}
			if (strncmp(line, "=== COMMAND STRATEGY", 20) == 0) {
				in_cmdstrat = 1;
				in_pattern = 0;
				cmdstrat_want_cmd = 0;
				cmdstrat_take = 0;
				continue;
			}
			if (strncmp(line, "=== SEARCH PATTERN", 18) == 0) {
				in_pattern = 1;
				in_cmdstrat = 0;
				cmdstrat_want_cmd = 0;
				continue;
			}
			if (strncmp(line, "=== EDIT COMMAND (", 18) == 0) {
				const char *p = line + 18;
				in_edit_cmd = 1;
				in_pattern = 0;
				in_cmdstrat = 0;
				cmdstrat_want_cmd = 0;
				if (strncmp(p, "abs)", 4) == 0)
					edit_cmd_strat = STRAT_ABS;
				else if (strncmp(p, "relc)", 5) == 0)
					edit_cmd_strat = STRAT_RELC;
				else if (strncmp(p, "rel)", 4) == 0)
					edit_cmd_strat = STRAT_REL;
				else
					edit_cmd_strat = STRAT_DEFAULT;
				continue;
			}
			if (strncmp(line, "=== END GROUP ===", 17) == 0) {
				in_pattern = 0;
				in_cmdstrat = 0;
				cmdstrat_want_cmd = 0;
				continue;
			}
			if (in_edit_cmd) {
				if (necmd >= ecmd_cap) {
					ecmd_cap = ecmd_cap ? ecmd_cap * 2 : 4;
					ecmd_lines = realloc(ecmd_lines,
						ecmd_cap * sizeof(char *));
				}
				ecmd_lines[necmd++] = xstrdup(line);
				continue;
			}
			if (in_cmdstrat) {
				if (cmdstrat_want_cmd) {
					if (cmdstrat_take &&
					    edited_strategy == STRAT_DEFAULT) {
						edited_strategy = cmdstrat_strat;
						snprintf(edited_cmd,
							 sizeof(edited_cmd),
							 "%s", line);
					}
					cmdstrat_want_cmd = 0;
					cmdstrat_take = 0;
				} else {
					const char *name = (line[0] == '#')
						? line + 1 : line;
					cmdstrat_take = (line[0] != '#');
					if (strcmp(name, "abs") == 0) {
						cmdstrat_strat = STRAT_ABS;
						if (cmdstrat_take &&
						    edited_strategy == STRAT_DEFAULT)
							edited_strategy = STRAT_ABS;
						cmdstrat_take = 0;
						cmdstrat_want_cmd = 0;
					} else if (strcmp(name, "rel") == 0) {
						cmdstrat_strat = STRAT_REL;
						cmdstrat_want_cmd = 1;
					} else if (strcmp(name, "relc") == 0) {
						cmdstrat_strat = STRAT_RELC;
						cmdstrat_want_cmd = 1;
					} else {
						cmdstrat_take = 0;
						cmdstrat_want_cmd = 0;
					}
				}
				continue;
			}
			if (in_pattern && strncmp(line, "--- extra", 9) == 0) {
				skip_extra = 1;
				continue;
			}
			if (in_pattern && !skip_extra) {
				if (nlines >= line_cap) {
					line_cap = line_cap ? line_cap * 2 : 16;
					lines = realloc(lines,
						line_cap * sizeof(char *));
				}
				lines[nlines++] = xstrdup(line);
			}
		}
		if (in_edit_cmd)
			FLUSH_ECMD();
		if (cur_gi >= 0 && cur_gi < ngroups) {
			groups[cur_gi].strategy = edited_strategy;
			const char *orig = default_cmds[cur_gi]
				? default_cmds[cur_gi] : "";
			if (strcmp(edited_cmd, orig) != 0 && edited_cmd[0])
				groups[cur_gi].custom_cmd = xstrdup(edited_cmd);
			if (nlines > 0) {
				groups[cur_gi].custom_lines = lines;
				groups[cur_gi].ncustom = nlines;
				if (edited_offset != 0)
					groups[cur_gi].custom_offset = edited_offset;
			} else {
				free(lines);
			}
		} else {
			free(lines);
		}
		fclose(rd);
	}
#undef FLUSH_ECMD

cleanup_orig:
	unlink(tmppath_orig);
	unlink(tmppath);
	for (int gi = 0; gi < ngroups; gi++)
		free(default_cmds[gi]);
	free(default_cmds);
}


/* Emit a custom EDIT COMMAND lines array + trailing SEP.
 * lines[0] = "cmd [first-content]", lines[1..n] = extra content lines.
 * s/pat/repl/: emit_escaped_exarg_only; no ".\n" appended.
 * cmd first-line: cmd prefix verbatim, content via emit_escaped_text, ".\n".
 * bare cmd (d, etc.): output verbatim; no ".\n". */
static void emit_custom_edit_lines(FILE *out, char **lines, int nlines)
{
	if (nlines == 0)
		return;
	const char *first = lines[0];
	/* Detect substitute: s + non-alphanumeric separator */
	if (first[0] == 's' && first[1] &&
	    !((unsigned char)first[1] >= 'a' && (unsigned char)first[1] <= 'z') &&
	    !((unsigned char)first[1] >= 'A' && (unsigned char)first[1] <= 'Z') &&
	    !((unsigned char)first[1] >= '0' && (unsigned char)first[1] <= '9')) {
		emit_escaped_exarg_only(out, first);
		return;
	}
	/* Find first space: split command prefix from inline content */
	const char *sp = strchr(first, ' ');
	if (sp) {
		fwrite(first, 1, sp - first, out);  /* command prefix verbatim */
		fputc(' ', out);
		emit_escaped_text(out, sp + 1);     /* first content line escaped */
		fputc('\n', out);
		for (int k = 1; k < nlines; k++) {
			emit_escaped_text(out, lines[k]);
			fputc('\n', out);
		}
		fputs(".\n", out);
	} else {
		/* No content (d, ,#+Nd, etc.) */
		fputs(first, out);
	}
}

/* Process operations for one file and emit script */
static void emit_file_script(FILE *out, file_patch_t *fp)
{
	if (fp->nops == 0)
		return;

	fprintf(out, "\n# Patch: %s\n", fp->path);
	fputs("EXINIT=\"|sc! \\\\\\\\${SEP}|:vis 3${SEP}", out);

	/*
	 * Strategy: process operations in groups.
	 * A group is a contiguous sequence of deletes/adds.
	 * We process from end to start to avoid line number shifts.
	 */

	static group_t groups[MAX_OPS];
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
					all_ctx = realloc(all_ctx, all_ctx_cap * sizeof(char*));
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
		g->del_texts = malloc(g->ndel * sizeof(char*));
		for (int j = 0; j < g->ndel; j++)
		  g->del_texts[j] = fp->ops[del_start_idx + j].text;

		/* Collect consecutive adds */
		int add_start = i;
		while (i < fp->nops && fp->ops[i].type == 'a')
			i++;
		g->nadd = i - add_start;
		if (g->nadd > 0) {
			g->add_texts = malloc(g->nadd * sizeof(char*));
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
				g->post_ctx = malloc(post_avail * sizeof(char*));
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
				g->ldc_new_text = malloc(ne - ns + 1);
				memcpy(g->ldc_new_text, new + ns, ne - ns);
				g->ldc_new_text[ne - ns] = '\0';
			}
		}

		if (g->del_start || g->nadd)
			ngroups++;
	}

	/* Interactive mode: open $EDITOR for all groups at once */
	if (interactive_mode)
		interactive_edit_groups(groups, ngroups, fp->path);

	/*
	 * Emit groups.
	 * Absolute mode: reverse order (bottom-to-top) to preserve line numbers.
	 * Relative/interactive mode: forward order (top-to-bottom).
	 *   Each f>/f+ search starts from cursor position which advances
	 *   after each edit, so subsequent searches find the correct occurrence.
	 */
	int forward = relative_mode || interactive_mode;
	int gi_start = forward ? 0 : ngroups - 1;
	int gi_end = forward ? ngroups : -1;
	int gi_step = forward ? 1 : -1;

	int first_ml = 1;  /* first multiline search uses f>, subsequent use f+ */
	int cum_delta = 0;   /* cumulative line count change from previous groups */

	for (int gi = gi_start; gi != gi_end; gi += gi_step) {
		group_t *g = &groups[gi];
		int target_line = g->del_start ? g->del_start : g->add_after;

		/*
		 * Resolve strategy for this group.
		 * In non-interactive mode, strategy is determined by flags:
		 *   relative_mode -> REL, else ABS.
		 * In interactive mode, strategy comes from user selection
		 * (g->strategy), with STRAT_DEFAULT resolved here.
		 */
		int strat = g->strategy;

		/* Check availability of each approach */
		int has_anchors = 0;
		rel_ctx_t rc;
		rc.target_line = target_line;
		rc.anchors = g->anchors;
		rc.nanchors = g->nanchors;
		rc.follow_ctx = g->follow_ctx;
		rc.follow_offset = g->follow_offset;
		rc.anchor_offset = g->anchor_offset;
		if (rc.nanchors >= 2
		    || (rc.nanchors == 1 && rc.anchors[0] && rc.anchors[0][0])
		    || (rc.follow_ctx && rc.follow_ctx[0]))
			has_anchors = 1;
		/* Fallback: use first deleted line as anchor */
		if (!has_anchors && g->ndel > 0 && g->del_texts[0] && g->del_texts[0][0]) {
			rc.anchors = g->del_texts;
			rc.nanchors = 1;
			rc.anchor_offset = 0;
			rc.follow_ctx = NULL;
			rc.follow_offset = 0;
			has_anchors = 1;
		}

		if (!interactive_mode) {
			/* Non-interactive: original behavior */
			if (relative_mode && has_anchors)
				strat = STRAT_REL;
			else
				strat = STRAT_ABS;
		} else if (strat == STRAT_DEFAULT) {
			/* Interactive default resolution */
			if (relative_mode && has_anchors)
				strat = STRAT_REL;
			else if (has_anchors)
				strat = STRAT_REL;
			else
				strat = STRAT_ABS;
		}

		/* Validate strategy, apply fallback chain */
		if (strat == STRAT_REL && !has_anchors)
			strat = STRAT_ABS;
		if (strat == STRAT_RELC) {
			if (!has_anchors)
				strat = STRAT_ABS;
			else if (!(g->ndel == 1 && g->nadd == 1 && g->has_line_diff))
				strat = STRAT_REL;  /* fall back to s// if no ;c data */
		}

		/* Custom interactive path: use edited search pattern */
		int has_custom = g->custom_lines != NULL && g->ncustom > 0;

		/* Helper: emit rel position then a custom edit command (lines array).
		 * Substitute (lines[0] starts s+non-alnum): extra SEP before, exarg escaping.
		 * Otherwise: direct append to position address. */
#define EMIT_REL_EDIT(rlines, rnlines, tline) do { \
		int _mode = has_custom \
			? emit_custom_pos(out, g, &first_ml) \
			: emit_rel_pos(out, &rc, &first_ml); \
		const char *_f = (rlines)[0]; \
		int _is_sub = (_f[0] == 's' && _f[1] && \
		               !(_f[1] >= 'a' && _f[1] <= 'z') && \
		               !(_f[1] >= 'A' && _f[1] <= 'Z') && \
		               !(_f[1] >= '0' && _f[1] <= '9')); \
		if (_is_sub) { \
			EMIT_SEP(out); \
			if (_mode == 0) \
				emit_err_check(out, (tline)); \
			emit_escaped_exarg_only(out, _f); \
			EMIT_SEP(out); \
			emit_err_check(out, (tline)); \
		} else { \
			emit_custom_edit_lines(out, (rlines), (rnlines)); \
			EMIT_SEP(out); \
			if (_mode == 0) \
				emit_err_check(out, (tline)); \
		} \
} while (0)

		/* Dispatch per strategy */
		if (g->del_start && g->nadd) {
			if (strat == STRAT_ABS && g->custom_abs_lines) {
				emit_custom_edit_lines(out, g->custom_abs_lines,
				                       g->custom_abs_nlines);
				EMIT_SEP(out);
			} else if (strat == STRAT_RELC) {
				/* Rel search + horizontal ;c edit */
				if (has_custom) {
					emit_custom_pos(out, g, &first_ml);
					EMIT_SEP(out);
				} else {
					int mode = emit_rel_pos(out, &rc, &first_ml);
					if (mode == 1)
						EMIT_SEP(out);
				}
				if (g->custom_relc_lines && g->custom_relc_nlines > 0) {
					/* custom relc: lines[0] = ".;N;Mc content" */
					emit_custom_edit_lines(out, g->custom_relc_lines,
					                       g->custom_relc_nlines);
				} else {
					if (g->ldc_start == g->ldc_end)
						fprintf(out, ".;%dc", g->ldc_start);
					else
						fprintf(out, ".;%d;%dc", g->ldc_start, g->ldc_end);
					fputc(' ', out);
					emit_escaped_text(out, g->ldc_new_text);
					fputs("\n.\n", out);
				}
				EMIT_SEP(out);
				emit_err_check(out, g->del_start);
			} else if (strat == STRAT_REL) {
				if (g->custom_rel_lines && g->custom_rel_nlines > 0) {
					EMIT_REL_EDIT(g->custom_rel_lines,
					              g->custom_rel_nlines, g->del_start);
				} else if (g->ndel == 1 && g->nadd == 1 &&
				           g->has_line_diff) {
					if (has_custom) {
						emit_custom_pos(out, g, &first_ml);
						EMIT_SEP(out);
						emit_substitute_cmd(out, g->ld_old_text,
						                    g->ld_new_text);
						EMIT_SEP(out);
						emit_err_check(out, g->del_start);
					} else {
						emit_relative_substitute(out, &rc,
						    g->ld_old_text, g->ld_new_text,
						    &first_ml);
					}
				} else {
					if (has_custom) {
						int mode = emit_custom_pos(out, g, &first_ml);
						if (g->ndel == 1)
							fprintf(out, "c ");
						else
							fprintf(out, ",#+%dc ", g->ndel - 1);
						for (int k = 0; k < g->nadd; k++) {
							emit_escaped_text(out, g->add_texts[k]);
							fputc('\n', out);
						}
						fputs(".\n", out);
						EMIT_SEP(out);
						if (mode == 0)
							emit_err_check(out, g->del_start);
					} else {
						emit_relative_change(out, &rc,
						    g->ndel, g->add_texts, g->nadd,
						    &first_ml);
					}
				}
			} else {
				/* STRAT_ABS */
				if (g->ndel == 1 && g->nadd == 1 && g->has_line_diff) {
					int adj = forward ? cum_delta : 0;
					emit_horizontal_change(out, g->del_start + adj,
					    g->ldc_start, g->ldc_end, g->ldc_new_text);
				} else {
					int adj = forward ? cum_delta : 0;
					emit_change(out, g->del_start + adj, g->del_end + adj,
					    g->add_texts, g->nadd);
				}
			}
		} else if (g->del_start) {
			if (strat == STRAT_ABS && g->custom_abs_lines) {
				emit_custom_edit_lines(out, g->custom_abs_lines,
				                       g->custom_abs_nlines);
				EMIT_SEP(out);
			} else if (strat == STRAT_REL) {
				if (g->custom_rel_lines && g->custom_rel_nlines > 0) {
					EMIT_REL_EDIT(g->custom_rel_lines,
					              g->custom_rel_nlines, g->del_start);
				} else if (has_custom) {
					int mode = emit_custom_pos(out, g, &first_ml);
					if (g->ndel == 1)
						fputc('d', out);
					else
						fprintf(out, ",#+%dd", g->ndel - 1);
					EMIT_SEP(out);
					if (mode == 0)
						emit_err_check(out, g->del_start);
				} else {
					emit_relative_delete(out, &rc, g->ndel, &first_ml);
				}
			} else {
				/* STRAT_ABS */
				int adj = forward ? cum_delta : 0;
				emit_delete(out, g->del_start + adj, g->del_end + adj);
			}
		} else if (g->nadd) {
			if (strat == STRAT_ABS && g->custom_abs_lines) {
				emit_custom_edit_lines(out, g->custom_abs_lines,
				                       g->custom_abs_nlines);
				EMIT_SEP(out);
			} else if (strat == STRAT_REL) {
				if (g->custom_rel_lines && g->custom_rel_nlines > 0) {
					EMIT_REL_EDIT(g->custom_rel_lines,
					              g->custom_rel_nlines, g->add_after);
				} else if (has_custom) {
					const char *icmd;
					if (g->custom_offset > 0) {
						g->custom_offset -= 1;
						icmd = "a ";
					} else {
						icmd = "i ";
					}
					int mode = emit_custom_pos(out, g, &first_ml);
					fputs(icmd, out);
					for (int k = 0; k < g->nadd; k++) {
						emit_escaped_text(out, g->add_texts[k]);
						fputc('\n', out);
					}
					fputs(".\n", out);
					EMIT_SEP(out);
					if (mode == 0)
						emit_err_check(out, g->add_after);
				} else {
					/* For pure insert, adjust: search goes to anchor,
					   but insert should be after the anchor line.
					   If offset would go negative, use insert (i)
					   instead of append (a). */
					int use_i = 0;
					if (rc.nanchors > 0) {
						if (rc.anchor_offset > 0)
							rc.anchor_offset -= 1;
						else
							use_i = 1;
					} else if (rc.follow_ctx) {
						if (g->add_after <= 0)
							use_i = 1;
						else
							rc.follow_offset += 1;
					}
					if (use_i) {
						int mode = emit_rel_pos(out, &rc, &first_ml);
						fprintf(out, "i ");
						for (int k = 0; k < g->nadd; k++) {
							emit_escaped_text(out, g->add_texts[k]);
							fputc('\n', out);
						}
						fputs(".\n", out);
						EMIT_SEP(out);
						if (mode == 0)
							emit_err_check(out, rc.target_line);
					} else {
						emit_relative_insert(out, &rc,
						    g->add_texts, g->nadd, &first_ml);
					}
				}
			} else {
				/* STRAT_ABS */
				int adj = forward ? cum_delta : 0;
				emit_insert_after(out, g->add_after + adj,
				    g->add_texts, g->nadd);
			}
		}
#undef EMIT_REL_EDIT
		/* Track cumulative delta for abs mode */
		if (forward) {
			if (g->del_start && g->nadd)
				cum_delta += g->nadd - g->ndel;
			else if (g->del_start)
				cum_delta -= g->ndel;
			else if (g->nadd)
				cum_delta += g->nadd;
		}
		free(g->del_texts);
		free(g->add_texts);
		free(g->all_pre_ctx);
		free(g->post_ctx);
		if (g->custom_lines) {
			for (int k = 0; k < g->ncustom; k++)
				free(g->custom_lines[k]);
			free(g->custom_lines);
		}
		free(g->custom_cmd);
		free(g->ld_old_text);
		free(g->ld_new_text);
		free(g->ldc_new_text);
	}

	fputs("vis 2${SEP}wq\" $VI -e '", out);
	fputs(fp->path, out);
	fputs("'\n", out);
}

static void new_file(const char *path)
{
	files[nfiles].path = xstrdup(path);
	files[nfiles].nops = 0;
	nfiles++;
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
	fprintf(stderr, "Usage: %s [-aridh] [input.patch]\n", prog);
	fprintf(stderr, "Converts unified diff to shell script using nextvi ex commands\n");
	fprintf(stderr, "  -a  Use absolute line numbers\n");
	fprintf(stderr, "  -r  Use relative regex patterns instead of line numbers\n");
	fprintf(stderr, "  -i  Interactive mode: edit search patterns in $EDITOR\n");
	fprintf(stderr, "  -d  Delta mode: re-apply previous customizations from script (-d implies -i)\n");
	fprintf(stderr, "  -h  Show this help\n");
	fprintf(stderr, "Input can be a unified diff or a previously generated patch2vi script\n");
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
		for (j = 1; argv[i][j]; j++) {
			if (argv[i][j] == 'a')
				relative_mode = 0;
			else if (argv[i][j] == 'r')
				relative_mode = 1;
			else if (argv[i][j] == 'i')
				interactive_mode = 1;
			else if (argv[i][j] == 'd') {
				delta_mode = 1;
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

	/* Mark chars that cannot be ex separators (part of ex range syntax) */
	const char *ex_range_chars = " \t0123456789+-.,<>/$';%*#|";
	for (const char *p = ex_range_chars; *p; p++)
		byte_used[(unsigned char)*p] = 1;
	/* Mark chars that cannot be ex separators (part of ex commands) */
	const char *ex_cmd_chars = "@&!?bpaefidgmqrwusxycjtohlv=";
	for (const char *p = ex_cmd_chars; *p; p++)
		byte_used[(unsigned char)*p] = 1;
	/* Mark other problematic chars */
	byte_used[':'] = 1;  /* Default ex separator */
	byte_used['!'] = 1;  /* Shell escape */
	byte_used['"'] = 1;  /* Shell quote */
	byte_used['\\'] = 1; /* Escape char */
	byte_used['`'] = 1;  /* Shell backtick */
	byte_used['\n'] = 1; /* Newline */
	byte_used['\r'] = 1; /* Carriage return */

	if (relative_mode || interactive_mode) {
		const char *err_chars = "FAILline";
		for (const char *p = err_chars; *p; p++)
			byte_used[(unsigned char)*p] = 1;
	}

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
			{
				file_delta_t *cur_fd = NULL;
				grp_delta_t *cur_gd = NULL;
				int in_sect = 0; /* 1=pat 2=abs 3=rel 4=relc */
				while (fgets(line, sizeof(line), in)) {
					chomp(line);
					if (strncmp(line, "=== PATCH2VI PATCH ===", 22) == 0)
						break;
					if (strncmp(line, "=== PATCH2VI DELTA ===", 22) == 0)
						continue;
					if (strncmp(line, "=== END DELTA ===", 17) == 0) {
						cur_fd = NULL; cur_gd = NULL; in_sect = 0;
						continue;
					}
					if (strncmp(line, "=== DELTA ", 10) == 0) {
						in_sect = 0; cur_gd = NULL;
						if (delta_mode) {
							char *p = line + 10;
							char *end = strstr(p, " ===");
							if (end) *end = '\0';
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
					if (!delta_mode || !cur_fd) continue;
					if (strncmp(line, "GROUP ", 6) == 0) {
						in_sect = 0;
						if (cur_fd->ngrps >= cur_fd->gcap) {
							cur_fd->gcap = cur_fd->gcap
								? cur_fd->gcap * 2 : 4;
							cur_fd->grps = realloc(
								cur_fd->grps,
								cur_fd->gcap *
								sizeof(grp_delta_t));
						}
						cur_gd = &cur_fd->grps[cur_fd->ngrps++];
						memset(cur_gd, 0, sizeof(*cur_gd));
						cur_gd->group_idx = atoi(line + 6);
						continue;
					}
					if (!cur_gd) continue;
					if (strncmp(line, "strategy: ", 10) == 0) {
						in_sect = 0;
						const char *v = line + 10;
						if (strcmp(v, "abs") == 0)
							cur_gd->strategy = STRAT_ABS;
						else if (strcmp(v, "rel") == 0)
							cur_gd->strategy = STRAT_REL;
						else if (strcmp(v, "relc") == 0)
							cur_gd->strategy = STRAT_RELC;
						continue;
					}
					if (strncmp(line, "cmd: ", 5) == 0) {
						in_sect = 0;
						free(cur_gd->cmd);
						cur_gd->cmd = xstrdup(line + 5);
						continue;
					}
					if (strcmp(line, "pattern:") == 0)
						{ in_sect = 1; continue; }
					if (strcmp(line, "edit_cmd_abs:") == 0)
						{ in_sect = 2; continue; }
					if (strcmp(line, "edit_cmd_rel:") == 0)
						{ in_sect = 3; continue; }
					if (strcmp(line, "edit_cmd_relc:") == 0)
						{ in_sect = 4; continue; }
					switch (in_sect) {
					case 1:
						arr_append(&cur_gd->pattern,
							   &cur_gd->npattern,
							   &cur_gd->pat_cap, line);
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
					}
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

		/* New file: +++ b/path */
		if (strncmp(line, "+++ ", 4) == 0) {
			const char *path = line + 4;
			/* Skip common prefixes like b/ */
			if (path[0] && path[1] == '/')
				path += 2;
			new_file(path);
			in_hunk = 0;
			continue;
		}

		/* Skip --- line */
		if (strncmp(line, "--- ", 4) == 0)
			continue;

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
		fprintf(stderr, "error: patch uses all possible byte values, cannot find separator\n");
		return 1;
	}

	/* Emit shell script header */
	printf("#!/bin/sh -e\n");
	printf("# Generated by patch2vi from unified diff\n");
	list_unused_bytes(stdout);
	printf("\n# Pass any argument to use patch(1) instead of nextvi ex commands\n");
	printf("if [ -n \"$1\" ]; then\n");
	printf("    sed '1,/^=== PATCH2VI PATCH ===$/d' \"$0\" | patch -p1 --merge=diff3\n");
	printf("    exit $?\n");
	printf("fi\n\n");
	printf("VI=${VI:-vi}\n");
	printf("if ! $VI -? 2>&1 | grep -q 'Nextvi'; then\n");
	printf("    echo \"Error: $VI is not nextvi\" >&2\n");
	printf("    echo \"Set VI environment variable to point to nextvi binary\" >&2\n");
	printf("    exit 1\n");
	printf("fi\n\n");
	printf("SEP=\"$(printf '\\%03o')\"\n", sep);
	printf("# Comment to continue despite errors (errors are still printed)\n");
	printf("QF=\"\\\\${SEP}vis 2\\\\${SEP}q!1\"\n");
	if (relative_mode || interactive_mode) {
		printf("# Uncomment to enter interactive vi on patch failure\n");
		printf("#INTR=\"\\\\${SEP}|sc|\\\\${SEP}vis 2:e $0:%%f>:@Q:q!1\"\n");
		printf("# Uncomment to skip errors (0? = silent nop)\n");
		printf("#DBG=\"0\\?\"\n");
	}

	/* Emit script for each file */
	for (int i = 0; i < nfiles; i++)
		emit_file_script(stdout, &files[i]);

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
		printf("=== END DELTA ===\n");
	}
	printf("=== PATCH2VI PATCH ===\n");
	for (int i = 0; i < nraw; i++)
		fputs(raw_lines[i], stdout);

	return 0;
}
