/*
 * patch2vi - Convert unified diff patches to shell scripts using Nextvi ex commands
 *
 * Usage: patch2vi [input.patch]
 *
 * Uses raw ex mode (:vis 6) with dynamic separator selection via :sc!
 * to avoid conflicts with : % ! characters in patch content.
 *
 * The generated script uses EXINIT with ex commands to apply changes.
 * The user can then modify the script to add regex-based matching for
 * more robust patch application.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static int relative_mode = 0;  /* -r flag: use regex patterns instead of line numbers */

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

/* Count UTF-8 characters in a string */
static int utf8_len(const char *s)
{
	int len = 0;
	for (; *s; s++)
		if ((*s & 0xC0) != 0x80)  /* not a continuation byte */
			len++;
	return len;
}

/* Get byte offset for UTF-8 character position */
static int utf8_byte_offset(const char *s, int charpos)
{
	const char *p = s;
	int chars = 0;
	while (*p && chars < charpos) {
		if ((*p & 0xC0) != 0x80)
			chars++;
		p++;
	}
	return p - s;
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

/*
 * Compare two lines and find the differing portion.
 * Returns 1 if suitable for horizontal edit, 0 otherwise.
 * Sets *start to first differing UTF-8 char position,
 * *old_end to end position in old string,
 * *new_text to the replacement text (allocated).
 */
static int find_line_diff(const char *old, const char *new,
			int *start, int *old_end, char **new_text)
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

	/* Convert byte positions to UTF-8 character positions */
	*start = utf8_char_offset(old, old_diff_start);
	*old_end = utf8_char_offset(old, old_diff_end);

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

/* Find an unused byte to use as separator (prefer printable) */
static int find_unused_byte(void)
{
	/* First try printable non-special chars (avoid ex range chars) */
	const char *preferred = "@&~=[]{}";
	for (const char *p = preferred; *p; p++) {
		if (!byte_used[(unsigned char)*p])
			return *p;
	}
	/* Then try any printable */
	for (int c = '!'; c <= '~'; c++) {
		if (!byte_used[c])
			return c;
	}
	/* Then try high bytes */
	for (int c = 128; c < 256; c++) {
		if (!byte_used[c])
			return c;
	}
	/* Then try low control chars (except 0, \n, \r, \t) */
	for (int c = 1; c < 32; c++) {
		if (c != '\n' && c != '\r' && c != '\t' && !byte_used[c])
			return c;
	}
	return -1;  /* All bytes used - very unlikely */
}

/* Parse a hunk header: @@ -old_start,old_count +new_start,new_count @@ */
static int parse_hunk_header(const char *line, int *old_start, int *old_count,
				 int *new_start, int *new_count)
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
	while (*p && *p != '+')
		p++;
	if (*p != '+')
		return 0;
	p++;
	*new_start = atoi(p);
	*new_count = 1;
	while (*p && *p != ',' && *p != ' ')
		p++;
	if (*p == ',') {
		p++;
		*new_count = atoi(p);
	}
	return 1;
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

/* Emit ex commands for inserting text after line N */
static void emit_insert_after(FILE *out, int line, char **texts, int ntexts, int sep)
{
	if (ntexts == 0)
		return;

	fprintf(out, "%da ", line);
	for (int i = 0; i < ntexts; i++) {
		emit_escaped_line(out, texts[i]);
		fputc('\n', out);
	}
	fprintf(out, ".\n%c", sep);
}

/* Emit ex commands for deleting lines from N to M inclusive */
static void emit_delete(FILE *out, int from, int to, int sep)
{
	if (from == to)
		fprintf(out, "%dd%c", from, sep);
	else
		fprintf(out, "%d,%dd%c", from, to, sep);
}

/* Emit ex command for horizontal (character-level) edit */
static void emit_horizontal_change(FILE *out, int line, int char_start, int char_end,
					const char *new_text, int sep)
{
	if (char_start == char_end)
		fprintf(out, "%d;%dc ", line, char_start);
	else
		fprintf(out, "%d;%d;%dc ", line, char_start, char_end);
	emit_escaped_line(out, new_text);
	fprintf(out, "\n.\n%c", sep);
}

/* Emit ex commands for changing lines (delete and insert) */
static void emit_change(FILE *out, int from, int to, char **texts, int ntexts, int sep)
{
	if (ntexts == 0) {
		emit_delete(out, from, to, sep);
		return;
	}

	if (from == to)
		fprintf(out, "%dc ", from);
	else
		fprintf(out, "%d,%dc ", from, to);

	for (int i = 0; i < ntexts; i++) {
		emit_escaped_line(out, texts[i]);
		fputc('\n', out);
	}
	fprintf(out, ".\n%c", sep);
}

/*
 * Relative mode emit functions - use regex patterns instead of line numbers.
 *
 * Forward processing: groups are processed top-to-bottom.
 * Each >anchor> search starts from current cursor position (xrow)
 * and finds the next occurrence forward. After each edit, the cursor
 * advances, so subsequent searches naturally find the correct occurrence.
 *
 * Multi-line context: when 2+ context lines are available, uses f> command
 * for multiline search (newlines become regular characters in the search).
 *
 * Conditional wrapping: each edit is wrapped in ? so if anchor doesn't
 * match, the edit is skipped rather than corrupting the wrong location.
 */

typedef struct {
	char **anchors;
	int nanchors;
	int anchor_start_line;
	char *follow_ctx;
	int follow_offset;
	int anchor_offset;   /* lines from last anchor to first change */
} rel_ctx_t;

/* Write a regex-escaped string with shell double-quote escaping.
 * escape_regex handles regex metacharacters, then emit_escaped_line
 * handles shell special chars (\, $, `, ") so backslashes survive
 * the shell's double-quote processing. */
static void emit_escaped_regex(FILE *out, const char *s)
{
	char *escaped = escape_regex(s);
	emit_escaped_line(out, escaped);
	free(escaped);
}

/* Emit forward single-line search position (no leading 1<sep>) */
static void emit_fwd_pos(FILE *out, const char *anchor, int offset, int sep)
{
	fputc('>', out);
	emit_escaped_regex(out, anchor);
	fputc('>', out);
	if (offset > 0)
		fprintf(out, "+%d", offset);
	else if (offset < 0)
		fprintf(out, "%d", offset);
}

/* Emit multiline f>/f+ position using 2+ context lines.
 * first=1 uses f> (search from current pos), first=0 uses f+ (skip past current) */
static void emit_multiline_pos(FILE *out, char **anchors, int nanchors,
				int offset, int sep, int first)
{
	fprintf(out, "%%;0f%c ", first ? '>' : '+');
	for (int i = 0; i < nanchors; i++) {
		emit_escaped_regex(out, anchors[i]);
		if (i < nanchors - 1)
			fputc('\n', out);  /* literal newline between context lines */
	}
	fprintf(out, "%c", sep);
	/* After f>/f+, cursor is at match position; use .+offset for target */
	fprintf(out, ".+%d", offset);
}

/* Emit following context search with negative offset */
static void emit_follow_pos(FILE *out, const char *follow, int offset, int sep)
{
	fputc('>', out);
	emit_escaped_regex(out, follow);
	fputc('>', out);
	if (offset > 0)
		fprintf(out, "-%d", offset);
	/* offset==0 means the follow ctx IS at the change line - unusual but handle it */
}

/*
 * Emit the search/positioning part for a relative group.
 * Returns: 0 = single-command positioning (pos is part of final cmd),
 *          1 = two-command positioning (f>/f+ then .+offset as separate cmd)
 *
 * For single-line: emits ">anchor>+offset" (caller appends action)
 * For multiline:   emits "%;0f>ctx1\nctx2<sep>.+offset" (first) or f+ (subsequent)
 * For follow ctx:  emits ">follow>-offset" (caller appends action)
 *
 * *first_ml: pointer to flag, 1 for first multiline search (uses f>),
 *            cleared to 0 after first use (subsequent use f+)
 */
static int emit_rel_pos(FILE *out, rel_ctx_t *rc, int sep, int *first_ml)
{
	if (rc->nanchors >= 2) {
		/* Multiline: f>/f+ search, then .+offset as position for next command */
		int offset = rc->nanchors + rc->anchor_offset - 1;
		emit_multiline_pos(out, rc->anchors, rc->nanchors, offset, sep, *first_ml);
		*first_ml = 0;
		return 1;
	}
	if (rc->nanchors == 1 && rc->anchors[0] && rc->anchors[0][0]) {
		emit_fwd_pos(out, rc->anchors[0], rc->anchor_offset, sep);
		return 0;
	}
	if (rc->follow_ctx && rc->follow_ctx[0]) {
		emit_follow_pos(out, rc->follow_ctx, rc->follow_offset, sep);
		return 0;
	}
	return -1;  /* no anchor available */
}

/* Emit the conditional search part for ? wrapping */
static void emit_cond_search(FILE *out, rel_ctx_t *rc, int sep, int first)
{
	if (rc->nanchors >= 2) {
		fprintf(out, "%%;0f%c ", first ? '>' : '+');
		for (int i = 0; i < rc->nanchors; i++) {
			emit_escaped_regex(out, rc->anchors[i]);
			if (i < rc->nanchors - 1)
				fputc('\n', out);
		}
	} else if (rc->nanchors == 1 && rc->anchors[0] && rc->anchors[0][0]) {
		fputc('>', out);
		emit_escaped_regex(out, rc->anchors[0]);
		fputc('>', out);
	} else if (rc->follow_ctx && rc->follow_ctx[0]) {
		fputc('>', out);
		emit_escaped_regex(out, rc->follow_ctx);
		fputc('>', out);
	}
}

/* Emit delete using relative pattern */
static void emit_relative_delete(FILE *out, rel_ctx_t *rc, int count, int sep,
				  int *first_ml)
{
	int mode = emit_rel_pos(out, rc, sep, first_ml);
	if (mode == 1) {
		/* multiline: pos is ".+N", append delete action */
		if (count == 1)
			fprintf(out, "d%c", sep);
		else
			fprintf(out, ",#+%dd%c", count - 1, sep);
	} else {
		/* single-line: pos already emitted, append delete */
		if (count == 1)
			fprintf(out, "d%c", sep);
		else
			fprintf(out, ",#+%dd%c", count - 1, sep);
	}
}

/* Emit insert using relative pattern */
static void emit_relative_insert(FILE *out, rel_ctx_t *rc,
				  char **texts, int ntexts, int sep, int *first_ml)
{
	if (ntexts == 0)
		return;

	int mode = emit_rel_pos(out, rc, sep, first_ml);
	if (mode == 1) {
		/* After multiline f>, we have .+offset as position */
		fprintf(out, "a ");
	} else {
		fprintf(out, "a ");
	}
	for (int i = 0; i < ntexts; i++) {
		emit_escaped_line(out, texts[i]);
		fputc('\n', out);
	}
	fprintf(out, ".\n%c", sep);
}

/* Emit change using relative pattern */
static void emit_relative_change(FILE *out, rel_ctx_t *rc,
				  int del_count, char **texts, int ntexts, int sep,
				  int *first_ml)
{
	if (ntexts == 0) {
		emit_relative_delete(out, rc, del_count, sep, first_ml);
		return;
	}

	int mode = emit_rel_pos(out, rc, sep, first_ml);
	if (mode == 1) {
		/* multiline pos emitted .+offset, append change */
		if (del_count == 1)
			fprintf(out, "c ");
		else
			fprintf(out, ",#+%dc ", del_count - 1);
	} else {
		if (del_count == 1)
			fprintf(out, "c ");
		else
			fprintf(out, ",#+%dc ", del_count - 1);
	}

	for (int i = 0; i < ntexts; i++) {
		emit_escaped_line(out, texts[i]);
		fputc('\n', out);
	}
	fprintf(out, ".\n%c", sep);
}

/* Emit horizontal change using relative pattern */
static void emit_relative_horizontal(FILE *out, rel_ctx_t *rc,
				      int char_start, int char_end,
				      const char *new_text, int sep, int *first_ml)
{
	emit_rel_pos(out, rc, sep, first_ml);
	if (char_start == char_end)
		fprintf(out, ";%dc ", char_start);
	else
		fprintf(out, ";%d;%dc ", char_start, char_end);
	emit_escaped_line(out, new_text);
	fprintf(out, "\n.\n%c", sep);
}

/* Process operations for one file and emit script */
static void emit_file_script(FILE *out, file_patch_t *fp, int sep)
{
	if (fp->nops == 0)
		return;

	fprintf(out, "\n# Patch: %s\n", fp->path);
	fprintf(out, "EXINIT=\"rcm:|sc! %c|vis 6%c", sep, sep);

	/*
	 * Strategy: process operations in groups.
	 * A group is a contiguous sequence of deletes/adds.
	 * We process from end to start to avoid line number shifts.
	 */

	typedef struct {
		int del_start, del_end;  /* 0 if no deletes */
		char **add_texts;
		char **del_texts;        /* deleted line contents */
		int ndel;
		int nadd;
		int add_after;  /* line to add after (for pure adds) */
		/* For relative mode: */
		char *anchor;            /* last preceding context line text */
		int anchor_offset;       /* lines from anchor to first change */
		char *anchors[3];        /* up to 3 consecutive preceding context lines */
		int nanchors;            /* count of anchor lines */
		int anchor_start_line;   /* line number of first anchor line */
		char *follow_ctx;        /* first following context line */
		int follow_offset;       /* lines from first change to follow_ctx */
	} group_t;

	group_t groups[MAX_OPS];
	int ngroups = 0;
	int i = 0;

	while (i < fp->nops) {
		group_t *g = &groups[ngroups];
		g->del_start = g->del_end = 0;
		g->add_texts = NULL;
		g->del_texts = NULL;
		g->ndel = 0;
		g->nadd = 0;
		g->add_after = 0;
		g->anchor = NULL;
		g->anchor_offset = 0;
		g->nanchors = 0;
		g->anchor_start_line = 0;
		g->follow_ctx = NULL;
		g->follow_offset = 0;

		/* Skip context lines, collecting up to 3 consecutive for relative mode */
		char *last_ctx = NULL;
		int last_ctx_line = 0;
		char *ctx_ring[3] = {NULL, NULL, NULL};
		int ctx_line_ring[3] = {0, 0, 0};
		int ctx_count = 0;
		while (i < fp->nops && fp->ops[i].type == 'c') {
			last_ctx = fp->ops[i].text;
			last_ctx_line = fp->ops[i].oline;
			/* Shift ring buffer */
			ctx_ring[0] = ctx_ring[1];
			ctx_line_ring[0] = ctx_line_ring[1];
			ctx_ring[1] = ctx_ring[2];
			ctx_line_ring[1] = ctx_line_ring[2];
			ctx_ring[2] = fp->ops[i].text;
			ctx_line_ring[2] = fp->ops[i].oline;
			ctx_count++;
			i++;
		}
		if (i >= fp->nops)
			break;

		/* Store anchor info for relative mode */
		g->anchor = last_ctx;
		if (last_ctx) {
			int first_change_line = fp->ops[i].oline;
			g->anchor_offset = first_change_line - last_ctx_line;
		}
		/* Store multi-line anchors (up to 3 consecutive context lines before change) */
		if (ctx_count >= 3) {
			g->anchors[0] = ctx_ring[0];
			g->anchors[1] = ctx_ring[1];
			g->anchors[2] = ctx_ring[2];
			g->nanchors = 3;
			g->anchor_start_line = ctx_line_ring[0];
		} else if (ctx_count == 2) {
			g->anchors[0] = ctx_ring[1];
			g->anchors[1] = ctx_ring[2];
			g->nanchors = 2;
			g->anchor_start_line = ctx_line_ring[1];
		} else if (ctx_count == 1) {
			g->anchors[0] = ctx_ring[2];
			g->nanchors = 1;
			g->anchor_start_line = ctx_line_ring[2];
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

		/* Peek at following context for fallback when no preceding context */
		if (g->nanchors == 0 && i < fp->nops && fp->ops[i].type == 'c') {
			g->follow_ctx = fp->ops[i].text;
			/* Distance from first change to following context */
			int first_change_line = g->del_start ? g->del_start : g->add_after + 1;
			g->follow_offset = fp->ops[i].oline - first_change_line;
		}

		if (g->del_start || g->nadd)
			ngroups++;
	}

	/*
	 * Emit groups.
	 * Absolute mode: reverse order (bottom-to-top) to preserve line numbers.
	 * Relative mode: forward order (top-to-bottom) with content-based search.
	 *   Each >anchor> search starts from cursor position which advances
	 *   after each edit, so subsequent searches find the correct occurrence.
	 */
	int gi_start = relative_mode ? 0 : ngroups - 1;
	int gi_end = relative_mode ? ngroups : -1;
	int gi_step = relative_mode ? 1 : -1;

	int first_ml = 1;  /* first multiline search uses f>, subsequent use f+ */

	if (relative_mode)
		fprintf(out, "1%c", sep);  /* Position cursor at line 1 */

	for (int gi = gi_start; gi != gi_end; gi += gi_step) {
		group_t *g = &groups[gi];

		/* Build rel_ctx_t for relative mode */
		rel_ctx_t rc;
		int has_rel = 0;
		if (relative_mode) {
			rc.anchors = g->anchors;
			rc.nanchors = g->nanchors;
			rc.anchor_start_line = g->anchor_start_line;
			rc.follow_ctx = g->follow_ctx;
			rc.follow_offset = g->follow_offset;
			rc.anchor_offset = g->anchor_offset;
			/* Check if we have any usable anchor */
			if (rc.nanchors > 0 || (rc.follow_ctx && rc.follow_ctx[0]))
				has_rel = 1;
			/* Fallback: use first deleted line as anchor */
			if (!has_rel && g->ndel > 0 && g->del_texts[0] && g->del_texts[0][0]) {
				rc.anchors = g->del_texts;
				rc.nanchors = 1;
				rc.anchor_offset = 0;
				rc.follow_ctx = NULL;
				rc.follow_offset = 0;
				has_rel = 1;
			}
		}

		if (g->del_start && g->nadd) {
			/* Try horizontal edit for single-line changes */
			if (g->ndel == 1 && g->nadd == 1 &&
			    g->del_texts[0] && g->add_texts[0]) {
				int start, old_end;
				char *new_text;
				if (find_line_diff(g->del_texts[0], g->add_texts[0],
				                   &start, &old_end, &new_text)) {
					if (has_rel)
						emit_relative_horizontal(out, &rc,
						    start, old_end, new_text, sep, &first_ml);
					else
						emit_horizontal_change(out, g->del_start, start,
						    old_end, new_text, sep);
					free(new_text);
				} else {
					if (has_rel)
						emit_relative_change(out, &rc,
						    g->ndel, g->add_texts, g->nadd, sep, &first_ml);
					else
						emit_change(out, g->del_start, g->del_end,
						    g->add_texts, g->nadd, sep);
				}
			} else {
				if (has_rel)
					emit_relative_change(out, &rc,
					    g->ndel, g->add_texts, g->nadd, sep, &first_ml);
				else
					emit_change(out, g->del_start, g->del_end,
					    g->add_texts, g->nadd, sep);
			}
		} else if (g->del_start) {
			if (has_rel)
				emit_relative_delete(out, &rc, g->ndel, sep, &first_ml);
			else
				emit_delete(out, g->del_start, g->del_end, sep);
		} else if (g->nadd) {
			if (has_rel) {
				/* For pure insert, adjust: search goes to anchor,
				   but insert should be after the anchor line */
				if (rc.nanchors > 0) {
					rc.anchor_offset -= 1;
				} else if (rc.follow_ctx) {
					rc.follow_offset += 1;
				}
				emit_relative_insert(out, &rc,
				    g->add_texts, g->nadd, sep, &first_ml);
			} else
				emit_insert_after(out, g->add_after, g->add_texts, g->nadd, sep);
		}
		free(g->del_texts);
		free(g->add_texts);
	}

	fprintf(out, "vis 4%cwq\" $VI -e '%s'\n", sep, fp->path);
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
	fprintf(stderr, "Usage: %s [-r] [input.patch]\n", prog);
	fprintf(stderr, "Converts unified diff to shell script using nextvi ex commands\n");
	fprintf(stderr, "  -r  Use relative regex patterns instead of line numbers\n");
	exit(1);
}

int main(int argc, char **argv)
{
	char line[MAX_LINE];
	int in_hunk = 0;
	int old_line = 0;
	const char *input_file = NULL;

	/* Initialize byte tracking */
	memset(byte_used, 0, sizeof(byte_used));
	/* Mark chars that cannot be ex separators (part of ex range syntax) */
	const char *ex_range_chars = " \t0123456789+-.,<>/$';%*#|";
	for (const char *p = ex_range_chars; *p; p++)
		byte_used[(unsigned char)*p] = 1;
	/* Mark other problematic chars */
	byte_used[':'] = 1;  /* Default ex separator */
	byte_used['!'] = 1;  /* Shell escape */
	byte_used['"'] = 1;  /* Shell quote */
	byte_used['\\'] = 1; /* Escape char */
	byte_used['`'] = 1;  /* Shell backtick */
	byte_used['\n'] = 1; /* Newline */
	byte_used['\r'] = 1; /* Carriage return */

	/* Parse arguments */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
		} else if (strcmp(argv[i], "-r") == 0) {
			relative_mode = 1;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			usage(argv[0]);
		} else {
			input_file = argv[i];
		}
	}

	FILE *in = stdin;
	if (input_file) {
		in = fopen(input_file, "r");
		if (!in) {
			perror(input_file);
			return 1;
		}
	}

	while (fgets(line, sizeof(line), in)) {
		chomp(line);

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
		int os, oc, ns, nc;
		if (parse_hunk_header(line, &os, &oc, &ns, &nc)) {
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
	printf("#!/bin/sh\n");
	printf("# Generated by patch2vi from unified diff\n");
	printf("# Separator: ");
	if (sep >= 32 && sep < 127)
		printf("'%c'\n", sep);
	else
		printf("0x%02x\n", sep);
	printf("set -e\n");
	printf("\n# Path to nextvi (adjust as needed)\n");
	printf("VI=${VI:-vi}\n");
	printf("\n# Verify that VI is nextvi\n");
	printf("if ! $VI -? 2>&1 | grep -q 'Nextvi'; then\n");
	printf("    echo \"Error: $VI is not nextvi\" >&2\n");
	printf("    echo \"Set VI environment variable to point to nextvi\" >&2\n");
	printf("    exit 1\n");
	printf("fi\n");

	/* Emit script for each file */
	for (int i = 0; i < nfiles; i++)
		emit_file_script(stdout, &files[i], sep);

	return 0;
}
