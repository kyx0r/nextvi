/*
 * NEXTVI Editor
 *
 * Copyright (C) 2015-2019 Ali Gholami Rudi <ali at rudi dot ir>
 * Copyright (C) 2020-2025 Kyryl Melekhin <k dot melekhin at gmail dot com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "vi.h"
#include "conf.c"
#include "ex.c"
#include "lbuf.c"
#include "led.c"
#include "regex.c"
#include "ren.c"
#include "term.c"
#include "uc.c"

int vi_hidch;		/* show hidden chars */
int vi_insmov;		/* moving in insert outside of insertion sbuf */
int vi_lncol;		/* line numbers cursor offset */
static int vi_lnnum;	/* line numbers */
static int vi_mod;	/* screen should be redrawn -
			bit 1: whole screen, bit 2: current line, bit 3: update vi_col) */
static char vi_word_m[] = "\0leEwW";	/* line word navigation */
static char *vi_word = vi_word_m;
static char *_vi_word = vi_word_m;
static int vi_wsel = 1;
static int vi_rshift;			/* row shift for vi_word */
static int vi_arg1, vi_arg2;		/* the first and second arguments */
static char vi_msg[EXLEN+128];		/* current message */
static char vi_charlast[8];		/* the last character searched via f, t, F, or T */
static int vi_charcmd;			/* the character finding command */
static int vi_ybuf;			/* current yank buffer */
static int vi_col;			/* the column requested by | command */
static int vi_scrollud;			/* scroll amount for ^u and ^d */
static int vi_scrolley;			/* scroll amount for ^e and ^y */
static int vi_cndir = 1;		/* ^n direction */
static int vi_status;			/* always show status */
static int vi_tsm;			/* type of the status message */
static int vi_joinmode = 1;		/* 1: insert extra space for pad 0: raw line join */
static int vi_nlword;			/* new line mode for eEwWbB */
static int vi_rln[256];

void *emalloc(size_t size)
{
	void *p;
	if (!(p = malloc(size))) {
		fprintf(stderr, "\nmalloc: out of memory\n");
		exit(EXIT_FAILURE);
	}
	return p;
}

void *erealloc(void *p, size_t size)
{
	if (!(p = realloc(p, size))) {
		fprintf(stderr, "\nrealloc: out of memory\n");
		exit(EXIT_FAILURE);
	}
	return p;
}

static void reverse_in_place(char *str, int len)
{
	char *p1 = str;
	char *p2 = str + len - 1;
	while (p1 < p2) {
		char tmp = *p1;
		*p1++ = *p2;
		*p2-- = tmp;
	}
}

char *itoa(int n, char s[])
{
	int i = 0, sign;
	if ((sign = n) < 0)		/* record sign */
		n = -n;			/* make n positive */
	do {				/* generate digits in reverse order */
		s[i++] = n % 10 + '0';	/* get next digit */
	} while ((n /= 10) > 0);	/* delete it */
	if (sign < 0)
		s[i++] = '-';
	s[i] = '\0';
	reverse_in_place(s, i);
	return &s[i];
}

static void vi_drawmsg(void)
{
	if (vi_msg[0]) {
		syn_blockhl = 0;
		syn_setft("/-");
		preserve(int, xtd, 2)
		led_recrender(vi_msg, xrows, 0, 0, xcols)
		restore(xtd)
		syn_setft(ex_ft);
	}
}

static int vi_nextcol(char *ln, int dir, int *off)
{
	int o = ren_off(ln, ren_next(ln, ren_pos(ln, *off), dir));
	if (*rstate->chrs[o] == '\n')
		return -1;
	*off = o;
	return 0;
}

#define vi_drawnum(func) \
{ \
nrow = xrow; \
noff = xoff; \
for (i = 0, ret = 0;; i++) { \
	l1 = ren_next(c, ren_pos(c, noff), 1)-1-xleft+vi_lncol; \
	if (l1 > xcols || l1 < 0 || ret) \
		break; \
	i = i > 99 ? i % 100 : i; \
	itoa(i%10 ? i%10 : i, snum); \
	tmp[l1] = *snum; \
	ret = func; \
} } \

static void vi_drawrow(int row)
{
	int l1, i, i1, lnnum = vi_lnnum;
	char *c, *s;
	static char ch1[1] = "~";
	static char ch2[1] = "";
	if (*vi_word) {
		int noff, nrow, ret;
		s = lbuf_get(xb, row - vi_rshift);
		c = lbuf_get(xb, xrow);
		if (row == xtop + xrows-1 || !c || *c == '\n')
			vi_rshift = 0;
		if (row != xrow+1 || !c || *c == '\n')
			goto skip;
		char tmp[xcols+3], snum[100];
		memset(tmp, ' ', xcols+1);
		tmp[xcols+1] = '\n';
		tmp[xcols+2] = '\0';
		i1 = isupper((unsigned char)*vi_word);
		if (*vi_word == 'e' || *vi_word == 'E')
			vi_drawnum(lbuf_wordend(xb, i1, 2, &nrow, &noff))
		else if (*vi_word == 'w' || *vi_word == 'W')
			vi_drawnum(lbuf_wordbeg(xb, i1, 2, &nrow, &noff))
		if (*vi_word == 'l') {
			vi_drawnum(vi_nextcol(c, 1, &noff))
			vi_drawnum(vi_nextcol(c, -1, &noff))
		} else
			vi_drawnum(lbuf_wordend(xb, i1, -2, &nrow, &noff))
		tmp[ren_next(c, ren_pos(c, xoff), 1)-1-xleft+vi_lncol] = *vi_word;
		preserve(int, xorder, 0)
		preserve(int, syn_blockhl, 0)
		preserve(int, xtd, dir_context(c) * 2)
		vi_rshift = (row != xtop + xrows-1);
		syn_setft("/#");
		rstate++;
		led_recrender(tmp, row - xtop, 0, 0, xcols)
		rstate--;
		syn_setft(ex_ft);
		restore(xorder)
		restore(syn_blockhl)
		restore(xtd)
		return;
	}
	s = lbuf_get(xb, row);
	skip:
	if (!s)
		s = row ? ch1 : ch2;
	else if (lnnum) {
		char tmp[100], tmp1[100], *p;
		c = tmp, i = 0, i1 = 0;
		if (lnnum == 1 || lnnum & 2) {
			c = itoa(row+1-vi_rshift, tmp);
			*c++ = ' ';
			i = snprintf(0, 0, "%d", xtop+xrows);
		}
		p = c;
		if (lnnum == 1 || lnnum & 4 || lnnum & 8) {
			c = itoa(abs(xrow-row+vi_rshift), c);
			*c++ = ' ';
			i1 = snprintf(0, 0, "%d", xrows);
		}
		*c = '\0';
		l1 = (c - tmp) + (i+i1 - (strlen(tmp) - !!i - !!i1));
		vi_lncol = dir_context(s) < 0 ? 0 : l1;
		memset(c, ' ', l1 - (c - tmp));
		c[l1 - (c - tmp)] = '\0';
		led_crender(s, row - xtop, l1, xleft, xleft + xcols - l1);
		preserve(int, syn_blockhl, 0)
		syn_setft("/##");
		if ((lnnum == 1 || lnnum & 4) && xled && !xleft && vi_lncol) {
			for (i1 = 0; strchr(" \t", *rstate->chrs[ren_off(s, i1)]);)
				i1 = ren_next(s, i1, 1);
			i1 -= (itoa(abs(xrow-row+vi_rshift), tmp1) - tmp1)+1;
			if (i1 >= 0) {
				memset(p, ' ', strlen(p));
				led_prender(tmp1, row - xtop, l1+i1, 0, l1);
			}
		}
		led_prender(tmp, row - xtop, 0, 0, l1);
		syn_setft(ex_ft);
		restore(syn_blockhl)
		return;
	}
	led_crender(s, row - xtop, 0, xleft, xleft + xcols);
}

/* redraw the screen */
static void vi_drawagain(int i)
{
	syn_scdir(0);
	syn_blockhl = 0;
	for (; i < xtop + xrows; i++)
		vi_drawrow(i);
}

/* update the screen */
static void vi_drawupdate(int i)
{
	int n;
	term_pos(0, 0);
	term_room(i);
	syn_scdir(i);
	if (i < 0) {
		n = MIN(-i, xrows);
		for (i = 0; i < n; i++)
			vi_drawrow(xtop + xrows - n + i);
	} else {
		n = MIN(i, xrows);
		for (i = n-1; i >= 0; i--)
			vi_drawrow(xtop + i);
	}
}

static void vi_wait(void)
{
	if (xmpt > 1) {
		strcpy(vi_msg, "[any key to continue] ");
		vi_drawmsg();
		term_read();
		vi_msg[0] = '\0';
		vi_mod |= 1;
	}
	xmpt = xmpt > 0 ? 0 : xmpt;
}

static char *vi_prompt(char *msg, char *insert, int *kmap, int *mlen)
{
	char *s;
	term_pos(xrows, led_pos(msg, 0));
	vi_lncol = 0;
	syn_setft("/-");
	s = led_prompt(msg, "", insert, kmap);
	syn_setft(ex_ft);
	vi_mod |= 1;
	*mlen = s ? strlen(msg) : 0;
	if (!s)
		return NULL;
	strncpy(vi_msg, s, sizeof(vi_msg) - 1);
	return s;
}

static char *vi_enprompt(char *msg, char *insert, int *mlen)
{
	int kmap = 0;
	return vi_prompt(msg, insert, &kmap, mlen);
}

/* read an ex input line */
char *ex_read(char *msg)
{
	int c;
	if (!(xvis & 2)) {
		int oleft = xleft;
		syn_blockhl = 0;
		syn_setft("/-");
		char *s = led_prompt(msg, "", NULL, &xkmap);
		xleft = oleft;
		if (s && (!msg || strcmp(s, msg)))
			term_chr('\n');
		syn_setft(ex_ft);
		return s;
	}
	sbuf_smake(sb, 128)
	while ((c = term_read()) != EOF && c != '\n')
		sbuf_chr(sb, c)
	if (c == EOF) {
		free(sb->s);
		return NULL;
	}
	sbufn_sret(sb)
}

/* print an ex output line */
void ex_cprint(char *line, int r, int c, int ln)
{
	syn_blockhl = 0;
	if (!(xvis & 4)) {
		if (xmpt == 1)
			term_chr('\n');
		if (isupper(xpr) && xmpt == 1 && !strchr(line, '\n'))
			vi_regputraw(xpr, "\n", 1, 1);
		term_pos(xrows, led_pos(vi_msg, 0));
		snprintf(vi_msg+c, sizeof(vi_msg)-c, "%s", line);
	}
	if (xpr)
		vi_regputraw(xpr, line, !!strchr(line, '\n'), 1);
	if (xvis & 2) {
		term_write(line, dstrlen(line, '\n'))
		term_write("\n", 1)
		return;
	}
	syn_setft("/-");
	led_recrender(line, r, c, xleft, xleft + xcols - c)
	syn_setft(ex_ft);
	if (ln && (xvis & 4 || xmpt > 0)) {
		term_chr('\n');
		if (isupper(xpr) && !strchr(line, '\n'))
			vi_regputraw(xpr, "\n", 1, 1);
	}
	xmpt += !(xvis & 4) && xmpt >= 0 && (ln || xmpt);
}

static int vi_yankbuf(void)
{
	int c = term_read();
	if (c == '"')
		return term_read();
	term_dec()
	return 0;
}

static int vi_prefix(void)
{
	int n = 0;
	int c = term_read();
	if (c >= '1' && c <= '9') {
		while (c >= '0' && c <= '9') {
			n = n * 10 + c - '0';
			c = term_read();
		}
	}
	return n;
}

static int vi_digit(void)
{
	int c = term_read();
	if (c >= '0' && c <= '9')
		return c - '0';
	return -1;
}

static int vi_off2col(struct lbuf *lb, int row, int off)
{
	char *ln = lbuf_get(lb, row);
	return ln ? ren_pos(ln, off) : 0;
}

static int vi_col2off(struct lbuf *lb, int row, int col)
{
	char *ln = lbuf_get(lb, row);
	if (!ln)
		return 0;
	ren_state *r = ren_position(ln);
	if (col >= r->cmax)
		return r->col[r->cmax - !!r->cmax];
	return r->col[col];
}

static int vi_findchar(struct lbuf *lb, char *cs, int cmd, int n, int *row, int *off)
{
	if (vi_charlast != cs)
		strcpy(vi_charlast, cs);
	vi_charcmd = cmd;
	return lbuf_findchar(lb, cs, cmd, n, row, off);
}

static int vi_search(int cmd, int cnt, int *row, int *off, int msg)
{
	int i, dir;
	if (cmd == '/' || cmd == '?') {
		char sign[4] = {cmd};
		char *kw = vi_prompt(sign, NULL, &xkmap, &i);
		if (!kw)
			return 1;
		ex_krsset(kw + i, cmd == '/' ? +2 : -2);
		free(kw);
	} else if (msg)
		ex_krsset(xregs['/'], xkwddir);
	if (!lbuf_len(xb) || (!xkwddir || !xkwdrs))
		return 1;
	dir = cmd == 'N' ? -xkwddir : xkwddir;
	for (i = 0; i < cnt; i++) {
		if (lbuf_search(xb, xkwdrs, dir, row, off,
				lbuf_len(xb), msg ? dir : -1)) {
			snprintf(vi_msg, msg, "\"%s\" not found %d/%d",
					xregs['/'], i, cnt);
			return 1;
		}
	}
	return 0;
}

/* read a line motion */
static int vi_motionln(int *row, int cmd)
{
	int cnt = (vi_arg1 ? vi_arg1 : 1) * (vi_arg2 ? vi_arg2 : 1);
	int c = term_read();
	int mark, mark_row, mark_off;
	switch (c) {
	case '\n':
	case '+':
	case 'j':
		*row = MIN(*row + cnt, lbuf_len(xb) - 1);
		break;
	case 'k':
	case '-':
		*row = MAX(*row - cnt, 0);
		break;
	case '\'':
		if ((mark = term_read()) <= 0)
			return -1;
		if (lbuf_jump(xb, mark, &mark_row, &mark_off))
			return -1;
		*row = mark_row;
		break;
	case 'G':
		*row = (vi_arg1 || vi_arg2) ? cnt - 1 : lbuf_len(xb) - 1;
		break;
	case 'H':
		*row = MIN(xtop + cnt - 1, lbuf_len(xb) - 1);
		break;
	case 'L':
		*row = MIN(xtop + xrows - 1 - cnt + 1, lbuf_len(xb) - 1);
		break;
	case 'M':
		*row = MIN(xtop + xrows / 2, lbuf_len(xb) - 1);
		break;
	default:
		if (c == cmd) {
			*row = MIN(*row + cnt - 1, lbuf_len(xb) - 1);
			break;
		}
		if (c == '%' && (vi_arg1 || vi_arg2)) {
			if (cnt > 100)
				return -1;
			*row = lbuf_len(xb) * cnt / 100;
			break;
		}
		term_dec()
		return 0;
	}
	if (*row < 0)
		*row = 0;
	return c;
}

static char *vi_curword(struct lbuf *lb, int row, int off, int n)
{
	char *ln = lbuf_get(lb, row);
	char *beg, *end;
	if (!ln || !n)
		return NULL;
	beg = uc_chr(ln, ren_noeol(ln, off));
	end = beg;
	while (beg > ln && uc_kind(uc_beg(ln, beg - 1)) == 1)
		beg = uc_beg(ln, beg - 1);
	for (int i = n; uc_len(end) && i > 0; end++, i--)
		while (uc_len(end) && uc_kind(end) == 1)
			end += uc_len(end);
	if (beg >= --end)
		return NULL;
	sbuf_smake(sb, (end - beg)+64)
	if (n > 1) {
		for (; beg != end; beg++) {
			if (*beg == xsep)
				sbuf_chr(sb, '\\')
			if (strchr("!#%{}[]().?\\^$|*/+", *beg))
				sbuf_chr(sb, '\\')
			sbuf_chr(sb, *beg)
		}
	} else {
		sbuf_str(sb, "\\<")
		sbuf_mem(sb, beg, end - beg)
		sbuf_str(sb, "\\>")
	}
	sbufn_sret(sb)
}

void vi_regputraw(unsigned char c, const char *s, int ln, int append)
{
	char *pre = append && xregs[tolower(c)] ? xregs[tolower(c)] : "";
	int len1 = strlen(pre), len2 = strlen(s);
	char *buf = emalloc(len1 + len2 + 4);
	memcpy(buf, pre, len1);
	memcpy(buf+len1, s, len2);
	memset(buf+len1+len2, '\0', 4);
	free(xregs[tolower(c)]);
	xregs[tolower(c)] = buf;
	vi_rln[tolower(c)] = ln;
}

void vi_regput(int c, const char *s, int ln)
{
	int i;
	char *i_s;
	if (ln || strchr(s, '\n')) {
		for (i = 8; i > 0; i--)
			if ((i_s = xregs['0'+i]))
				vi_regputraw('0' + i + 1, i_s, vi_rln['0'+i], 0);
		vi_regputraw('1', s, ln, 0);
	} else if (xregs[c])
		vi_regputraw('0', xregs[c], ln, 0);
	vi_regputraw(c, s, ln, isupper(c));
}

rset *fsincl;
char *fs_exdir;
static int fspos;
static int fsdir;

void dir_calc(char *path)
{
	struct dirent *dirp;
	struct stat statbuf;
	int i = 0, ret;
	char *cpath, *ptrs[1024];
	int plen[1024];
	DIR *dp, *sdp, *dps[1024];
	unsigned int pathlen = strlen(path), len;
	cpath = emalloc(pathlen + 1024);
	strcpy(cpath, path);
	if (!(dp = opendir(cpath)))
		return;
	sbuf_smake(sb, 1024)
	temp_pos(1, -1, 0, 0);
	fspos = 0;
	for (;;) {
		while ((dirp = readdir(dp))) {
			len = strlen(dirp->d_name)+1;
			if (strcmp(dirp->d_name, ".") == 0 ||
				strcmp(dirp->d_name, "..") == 0 ||
				len > 1023)
				continue;
			cpath[pathlen] = '/';
			memcpy(&cpath[pathlen+1], dirp->d_name, len);
			ret = lstat(cpath, &statbuf);
			if (ret >= 0 && S_ISDIR(statbuf.st_mode)) {
				if (!(sdp = opendir(cpath)) || i >= LEN(ptrs))
					break;
				dps[i] = sdp;
				ptrs[i] = cpath;
				cpath = emalloc(pathlen + 1024);
				memcpy(cpath, ptrs[i], pathlen + len);
				plen[i++] = pathlen + len;
			} else if (ret >= 0 && S_ISREG(statbuf.st_mode))
				if (!fsincl || rset_find(fsincl, cpath, NULL, 0) >= 0) {
					sbuf_mem(sb, cpath, (int)(pathlen + len))
					sbuf_chr(sb, '\n')
				}
		}
		closedir(dp);
		free(cpath);
		if (i > 0) {
			dp = dps[--i];
			pathlen = plen[i];
			cpath = ptrs[i];
		} else
			break;
	}
	sbuf_null(sb)
	if (sb->s_n > 1)
		temp_write(1, sb->s);
	free(sb->s);
}

#define fssearch() \
len = lbuf_s(path)->len; \
path[len] = '\0'; \
ret = ex_edit(path, len); \
path[len] = '\n'; \
if (ret && xrow) { \
	*row = xrow; *off = xoff; /* short circuit */ \
	if (!vi_search('n', cnt, row, off, 0)) \
		return 1; \
	++*off; \
} else { \
	*row = 0; *off = 0; \
} \
if (!vi_search(*row ? 'N' : 'n', cnt, row, off, 0)) \
	return 1; \

static int fs_search(int cnt, int *row, int *off)
{
	char *path;
	int again = 0, ret, len;
	wrap:
	while (fspos < lbuf_len(tempbufs[1].lb)) {
		path = tempbufs[1].lb->ln[fspos++];
		fssearch()
	}
	if (fspos == lbuf_len(tempbufs[1].lb) && !again) {
		fspos = 0;
		again = 1;
		goto wrap;
	}
	return 0;
}

static int fs_searchback(int cnt, int *row, int *off)
{
	char *path;
	int ret, len;
	while (--fspos >= 0) {
		path = tempbufs[1].lb->ln[fspos];
		fssearch()
	}
	return 0;
}

static void vc_status(int type)
{
	int cp, l, col, buf;
	char cbuf[8] = "", *c;
	col = vi_off2col(xb, xrow, xoff);
	col = ren_cursor(lbuf_get(xb, xrow), col) + 1;
	if (type && lbuf_get(xb, xrow)) {
		c = rstate->chrs[xoff];
		uc_code(cp, c, l)
		memcpy(cbuf, c, l);
		snprintf(vi_msg, sizeof(vi_msg), "<%s> %08x %dL %dW S%d O%d C%d",
			cbuf, cp, l, rstate->wid[xoff], (int)(c - lbuf_get(xb, xrow)),
			xoff, col);
		return;
	}
	buf = istempbuf(ex_buf) ? tempbufs - ex_buf - 1 : ex_buf - bufs;
	snprintf(vi_msg, sizeof(vi_msg),
		"\"%s\"%s%dL %d%% L%d C%d B%d",
		ex_path[0] ? ex_path : "unnamed",
		lbuf_modified(xb) ? "* " : " ", lbuf_len(xb),
		xrow * 100 / MAX(1, lbuf_len(xb)-1), xrow+1, col, buf);
}

/* read a motion */
static int vi_motion(int *row, int *off)
{
	static char ca_dir;
	static sbuf *savepath[10];
	static int srow[10], soff[10], lkwdcnt;
	int cnt = (vi_arg1 ? vi_arg1 : 1) * (vi_arg2 ? vi_arg2 : 1);
	int dir, mark, mark_row, mark_off;
	char *cs;
	int mv, i;

	if ((mv = vi_motionln(row, 0))) {
		*off = -1;
		return mv;
	}
	mv = term_read();
	switch (mv) {
	case ',':
		cnt = -cnt;
	case ';':
		if (!vi_charlast[0])
			return -1;
		if (vi_findchar(xb, vi_charlast, vi_charcmd, cnt, row, off))
			return -1;
		break;
	case 'h':
	case 'l':
		if (!(cs = lbuf_get(xb, *row)))
			return -1;
		dir = dir_context(cs);
		dir = mv == 'h' ? -dir : dir;
		for (i = 0; i < cnt; i++)
			if (vi_nextcol(cs, dir, off))
				break;
		break;
	case ' ':
	case 127:
	case TK_CTL('h'):
		dir = mv == ' ' ? +1 : -1;
		if (!lbuf_get(xb, *row))
			break;
		mark = ren_position(lbuf_get(xb, *row))->n;
		for (i = 0; i < cnt; i++, *off += dir)
			if (*off + dir < 0 || *off + dir >= mark)
				break;
		break;
	case 'f':
	case 'F':
	case 't':
	case 'T':
		if (!(cs = led_read(&xkmap, term_read())))
			return -1;
		if (vi_findchar(xb, cs, mv, cnt, row, off))
			return -1;
		break;
	case 'b':
	case 'B':
		mark = mv == 'B';
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, mark, -(vi_nlword+1), row, off))
				break;
		break;
	case 'e':
	case 'E':
		mark = mv == 'E';
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, mark, vi_nlword+1, row, off))
				break;
		break;
	case 'w':
	case 'W':
		mark = mv == 'W';
		for (i = 0; i < cnt; i++)
			if (lbuf_wordbeg(xb, mark, vi_nlword+1, row, off))
				break;
		break;
	case '{':
	case '}':
	case '[':
	case ']':
		dir = mv == '{' || mv == '[' ? 1 : -1;
		mark = mv == '[' || mv == ']' ? '\n' : '{';
		for (i = 0; i < cnt; i++)
			if (lbuf_sectionbeg(xb, dir, row, off, mark))
				break;
		break;
	case TK_CTL(']'):	/* note: this is also ^5 as per ascii */
	case TK_CTL('p'):
		#define open_saved(n) \
		if (savepath[n]) { \
			*row = srow[n]; *off = soff[n]; \
			ex_edit(savepath[n]->s, savepath[n]->s_n); \
		} \

		if (vi_arg1 && (cs = vi_curword(xb, *row, *off, cnt))) {
			ex_krsset(cs, +1);
			free(cs);
		}
		struct buf* tmpex_buf = istempbuf(ex_buf) ? ex_pbuf : ex_buf;
		if (mv == TK_CTL(']')) {
			if (vi_arg1 || lkwdcnt != xkwdcnt)
				term_exec("", 1, '&')
			lkwdcnt = xkwdcnt;
			fspos += fsdir < 0 ? 1 : 0;
			fspos = MIN(fspos, lbuf_len(tempbufs[1].lb));
			fs_search(1, row, off);
			fsdir = 1;
		} else {
			fspos -= fsdir > 0 ? 1 : 0;
			if (!fs_searchback(1, row, off)) {
				open_saved(0)
				fsdir = 0;
			} else
				fsdir = -1;
			fspos = MAX(fspos, 0);
		}
		if (tmpex_buf != ex_buf)
			ex_pbuf = tmpex_buf;
		bsync_ret:
		for (i = xbufcur-1; i >= 0 && bufs[i].mtime == -1; i--)
			ex_bufpostfix(&bufs[i], 1);
		syn_setft(ex_ft);
		vc_status(0);
		return 'n';
	case TK_CTL('t'):
		if (vi_arg1 % 2 == 0 && vi_arg1 < LEN(srow) * 2) {
			vi_arg1 /= 2;
			if (!savepath[vi_arg1])
				sbuf_make(savepath[vi_arg1], 128)
			sbuf_cut(savepath[vi_arg1], 0)
			sbufn_str(savepath[vi_arg1], ex_path)
			srow[vi_arg1] = *row; soff[vi_arg1] = *off;
			break;
		}
		open_saved(vi_arg1 / 2)
		goto bsync_ret;
	case '0':
		*off = 0;
		break;
	case '^':
		*off = lbuf_indents(xb, *row);
		break;
	case '$':
		*off = lbuf_eol(xb, *row);
		break;
	case '|':
		vi_col = cnt - 1;
		break;
	case '/':
	case '?':
	case 'n':
	case 'N':
		if (vi_search(mv, cnt, row, off, sizeof(vi_msg)))
			return -1;
		break;
	case TK_CTL('a'):
		if (!(cs = vi_curword(xb, *row, *off, cnt)))
			return -1;
		ex_krsset(cs, +1);
		free(cs);
		if (vi_search(ca_dir ? 'N' : 'n', 1, row, off, sizeof(vi_msg))) {
			ca_dir = !ca_dir;
			if (vi_search(ca_dir ? 'N' : 'n', 1, row, off, sizeof(vi_msg)))
				return -1;
		}
		break;
	case '`':
		if ((mark = term_read()) <= 0)
			return -1;
		if (lbuf_jump(xb, mark, &mark_row, &mark_off))
			return -1;
		*row = mark_row;
		*off = mark_off;
		break;
	case '%':
		if (lbuf_pair(xb, row, off))
			return -1;
		break;
	default:
		return 0;
	}
	return mv;
}

static void swap(int *a, int *b)
{
	int t = *a;
	*a = *b;
	*b = t;
}

static char *lbuf_region(struct lbuf *lb, int r1, int o1, int r2, int o2)
{
	char *s1, *s2, *s3;
	if (r1 == r2)
		return uc_sub(lbuf_get(lb, r1), o1, o2);
	sbuf_smake(sb, 1024)
	s1 = uc_chr(lbuf_get(lb, r1), o1);
	s3 = uc_sub(lbuf_get(lb, r2), 0, o2);
	s2 = lbuf_cp(lb, r1 + 1, r2);
	sbuf_str(sb, s1)
	sbuf_str(sb, s2)
	sbuf_str(sb, s3)
	free(s2);
	free(s3);
	sbufn_sret(sb)
}

static void vi_yank(int r1, int o1, int r2, int o2, int lnmode)
{
	char *region;
	region = lbuf_region(xb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	vi_regput(vi_ybuf, region, lnmode);
	free(region);
	xrow = r1;
	xoff = lnmode ? xoff : o1;
}

static void vi_delete(int r1, int o1, int r2, int o2, int lnmode)
{
	char *pref, *post;
	char *region;
	region = lbuf_region(xb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	vi_regput(vi_ybuf, region, lnmode);
	free(region);
	pref = lnmode ? uc_dup("") : uc_sub(lbuf_get(xb, r1), 0, o1);
	post = lnmode ? "\n" : uc_chr(lbuf_get(xb, r2), o2);
	if (!lnmode) {
		sbuf_smake(sb, 1024)
		sbuf_str(sb, pref)
		sbufn_str(sb, post)
		lbuf_edit(xb, sb->s, r1, r2 + 1);
		free(sb->s);
	} else
		lbuf_edit(xb, NULL, r1, r2 + 1);
	xrow = r1;
	xoff = lnmode ? lbuf_indents(xb, xrow) : o1;
	free(pref);
}

static void vi_splitln(int row, int linepos, int newln)
{
	char *s, *part, *buf;
	s = lbuf_get(xb, row);
	if (!s)
		return;
	part = uc_sub(s, linepos, -1);
	buf = uc_sub(s, 0, linepos);
	if (newln || *part != '\n') {
		lbuf_edit(xb, buf, row, row+1);
		lbuf_edit(xb, part, row+1, row+1);
		vi_mod |= 1;
	}
	free(part);
	free(buf);
}

static char *vi_indents(char *ln, int *l)
{
	sbuf_smake(sb, 256)
	while (xai && ln && (*ln == ' ' || *ln == '\t'))
		sbuf_chr(sb, *ln++)
	*l = sb->s_n;
	sbufn_sret(sb)
}

static void vi_change(int r1, int o1, int r2, int o2, int lnmode)
{
	char *region, *pref, *post, *_post;
	char *ln = lbuf_get(xb, r1);
	int l1, l2 = 1;
	region = lbuf_region(xb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	vi_regput(vi_ybuf, region, lnmode);
	free(region);
	pref = lnmode ? vi_indents(ln, &l1) : uc_subl(ln, 0, o1, &l1);
	post = _post = lnmode ? uc_dup("\n") : uc_subl(lbuf_get(xb, r2), o2, -1, &l2);
	term_pos(r1 - xtop < 0 ? 0 : r1 - xtop, 0);
	term_room(r1 < xtop ? xtop - xrow : r1 - r2);
	xrow = r1;
	if (r1 < xtop)
		xtop = r1;
	sbuf *rep = led_input(pref, &post, r1 - (r1 - r2), 0);
	sbufn_str(rep, post)
	int tlen = lnmode || !ln ? -1 : lbuf_s(ln)->len+1;
	if (rep->s_n != tlen || memcmp(&ln[l1], &rep->s[l1], tlen - l2 - l1))
		lbuf_edit(xb, rep->s, r1, r2 + 1);
	sbuf_free(rep)
	free(pref);
	free(_post);
}

static void vi_case(int r1, int o1, int r2, int o2, int lnmode, int cmd)
{
	char *pref, *post;
	char *region, *s;
	region = lbuf_region(xb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	s = region;
	while (uc_len(s)) {
		int c = (unsigned char) s[0];
		if (c <= 0x7f) {
			if (cmd == 'u')
				s[0] = tolower(c);
			if (cmd == 'U')
				s[0] = toupper(c);
			if (cmd == '~')
				s[0] = islower(c) ? toupper(c) : tolower(c);
		}
		s += uc_len(s);
	}
	pref = lnmode ? uc_dup("") : uc_sub(lbuf_get(xb, r1), 0, o1);
	post = lnmode ? "\n" : uc_chr(lbuf_get(xb, r2), o2);
	if (!lnmode) {
		sbuf_smake(sb, 256)
		sbuf_str(sb, pref)
		sbuf_str(sb, region)
		sbufn_str(sb, post)
		lbuf_edit(xb, sb->s, r1, r2 + 1);
		free(sb->s);
	} else
		lbuf_edit(xb, region, r1, r2 + 1);
	xrow = r2;
	xoff = lnmode ? lbuf_indents(xb, r2) : o2;
	free(region);
	free(pref);
}

static void vi_pipe(int r1, int r2)
{
	char region[100];
	char *p = itoa(r1+1, region);
	int mlen;
	*p++ = ',';
	p = itoa(r2+1, p);
	*p++ = '!';
	*p = '\0';
	char *cmd = vi_enprompt(":", region, &mlen);
	if (cmd)
		ex_command(cmd + mlen)
	free(cmd);
}

static void vi_shift(int r1, int r2, int dir)
{
	sbuf_smake(sb, 1024)
	char *ln;
	int i;
	for (i = r1; i <= r2; i++) {
		if (!(ln = lbuf_get(xb, i)))
			continue;
		if (dir < 0)
			ln = ln[0] == ' ' || ln[0] == '\t' ? ln + 1 : ln;
		else if (ln[0] != '\n' || r1 == r2)
			sbuf_chr(sb, '\t')
		sbufn_str(sb, ln)
		lbuf_edit(xb, sb->s, i, i + 1);
		sbuf_cut(sb, 0)
	}
	xoff = lbuf_indents(xb, r1);
	free(sb->s);
}

static void vc_motion(int cmd)
{
	int r1 = xrow, r2 = xrow;	/* region rows */
	int o1 = xoff, o2;		/* visual region columns */
	int lnmode = 0;			/* line-based region */
	int mv;
	vi_arg2 = vi_prefix();
	term_dec()
	o1 = ren_noeol(lbuf_get(xb, r1), o1);
	o2 = o1;
	if ((mv = vi_motionln(&r2, cmd)))
		o2 = -1;
	else if (!(mv = vi_motion(&r2, &o2)))
		return;
	if (mv < 0)
		return;
	lnmode = o2 < 0;
	if (lnmode) {
		o1 = 0;
		o2 = lbuf_eol(xb, r2);
	}
	if (r1 > r2) {
		swap(&r1, &r2);
		swap(&o1, &o2);
	}
	if (r1 == r2 && o1 > o2)
		swap(&o1, &o2);
	o1 = ren_noeol(lbuf_get(xb, r1), o1);
	if (!lnmode && strchr("fFtTeE%", mv))
		if (o2 < lbuf_eol(xb, r2))
			o2 = ren_noeol(lbuf_get(xb, r2), o2) + 1;
	if (cmd == 'y') {
		vi_yank(r1, o1, r2, o2, lnmode);
		return;
	}
	if (cmd == 'd')
		vi_delete(r1, o1, r2, o2, lnmode);
	if (cmd == 'c')
		vi_change(r1, o1, r2, o2, lnmode);
	if (cmd == '~' || cmd == 'u' || cmd == 'U')
		vi_case(r1, o1, r2, o2, lnmode, cmd);
	if (cmd == '!')
		vi_pipe(r1, r2);
	if (cmd == '>' || cmd == '<')
		vi_shift(r1, r2, cmd == '>' ? +1 : -1);
	if (cmd == TK_CTL('w'))
		for (int lc = r1; lc <= r2; lc++) {
			xoff = 2;
			while (xoff > 1)
				vi_shift(lc, lc, -1);
			vi_shift(lc, lc, -1);
		}
	vi_mod |= (r1 != r2 || (lnmode && cmd == 'd')) ? 1 : 2;
}

static void vc_insert(int cmd)
{
	char *pref, *post, *_post;
	char *ln = lbuf_get(xb, xrow);
	int row, cmdo, l1;
	if (cmd == 'I')
		xoff = lbuf_indents(xb, xrow);
	else if (cmd == 'A')
		xoff = lbuf_eol(xb, xrow);
	else if (cmd == 'o') {
		xrow++;
		if (xrow - xtop == xrows) {
			xtop++;
			vi_drawagain(xtop);
		}
	}
	xoff = ren_noeol(ln, xoff);
	row = xrow;
	if (cmd == 'a' || cmd == 'A')
		xoff++;
	if (ln && ln[0] == '\n')
		xoff = 0;
	cmdo = cmd == 'o' || cmd == 'O';
	pref = ln && !cmdo ? uc_subl(ln, 0, xoff, &l1) : vi_indents(ln, &l1);
	post = _post = ln && !cmdo ? uc_sub(ln, xoff, -1) : uc_dup("\n");
	term_pos(row - xtop, 0);
	term_room(cmdo);
	sbuf *rep = led_input(pref, &post, row, cmdo);
	if (rep->s_n != l1 || cmdo || !ln || memcmp(ln, rep->s, l1)) {
		sbufn_str(rep, post)
		if (cmdo && !lbuf_len(xb))
			lbuf_edit(xb, "\n", 0, 0);
		lbuf_edit(xb, rep->s, row, row + !cmdo);
	}
	sbuf_free(rep)
	free(pref);
	free(_post);
}

static int vc_put(int cmd)
{
	int cnt = MAX(1, vi_arg1);
	char *buf = xregs[vi_ybuf];
	int i;
	if (!buf)
		snprintf(vi_msg, sizeof(vi_msg), "yank buffer empty");
	if (!buf || !buf[0])
		return 0;
	sbuf_smake(sb, 1024)
	if (vi_rln[vi_ybuf]) {
		for (i = 0; i < cnt; i++)
			sbufn_str(sb, buf)
		if (!lbuf_len(xb))
			lbuf_edit(xb, "\n", 0, 0);
		if (cmd == 'p')
			xrow++;
		lbuf_edit(xb, sb->s, xrow, xrow);
		xoff = lbuf_indents(xb, xrow);
		free(sb->s);
		return 1;
	}
	char *ln = xrow < lbuf_len(xb) ? lbuf_get(xb, xrow) : "\n";
	int off = ren_noeol(ln, xoff) + (ln[0] != '\n' && cmd == 'p');
	char *s = uc_sub(ln, 0, off);
	sbuf_str(sb, s)
	free(s);
	for (i = 0; i < cnt; i++)
		sbuf_str(sb, buf)
	sbufn_str(sb, uc_chr(ln, off))
	lbuf_edit(xb, sb->s, xrow, xrow + 1);
	xoff = off + uc_slen(buf) * cnt - 1;
	free(sb->s);
	return 1;
}

static void vc_join(int spc, int cnt)
{
	int beg = xrow;
	int end = xrow + cnt;
	if (!lbuf_get(xb, beg) || !lbuf_get(xb, end - 1))
		return;
	sbuf_smake(sb, 1024)
	xoff = 0;
	for (int i = beg; i < end; i++) {
		char *ln = lbuf_get(xb, i);
		char *lnend = ln + lbuf_s(ln)->len;
		if (i > beg) {
			while (ln[0] == ' ' || ln[0] == '\t')
				ln++;
			if (spc && sb->s_n && *ln != ')' &&
					sb->s[sb->s_n-1] != ' ') {
				sbuf_chr(sb, ' ')
				xoff++;
			}
		}
		xoff += (i+1 == end) ? 0 : uc_slen(ln) - 1;
		sbuf_mem(sb, ln, lnend - ln)
	}
	sbufn_chr(sb, '\n')
	lbuf_edit(xb, sb->s, beg, end);
	free(sb->s);
	vi_mod |= 1;
}

static void vi_scrollforward(int cnt)
{
	xtop = MIN(lbuf_len(xb) - 1, xtop + cnt);
	xrow = MAX(xrow, xtop);
}

static void vi_scrollbackward(int cnt)
{
	xtop = MAX(0, xtop - cnt);
	xrow = MIN(xrow, xtop + xrows - 1);
}

static int vc_replace(void)
{
	int cnt = MAX(1, vi_arg1);
	char *cs = led_read(&xkmap, term_read());
	char *ln = lbuf_get(xb, xrow);
	char *pref, *post;
	char *s;
	int off, i;
	if (!ln || !cs)
		return 0;
	off = ren_noeol(ln, xoff);
	s = uc_chr(ln, off);
	for (i = 0; uc_len(s) && *s != '\n' && i < cnt; i++)
		s += uc_len(s);
	if (i < cnt)
		return 0;
	pref = uc_sub(ln, 0, off);
	post = uc_chr(ln, off + cnt);
	sbuf_smake(sb, 1024)
	sbuf_str(sb, pref)
	for (i = 0; i < cnt; i++)
		sbuf_str(sb, cs)
	sbufn_str(sb, post)
	lbuf_edit(xb, sb->s, xrow, xrow + 1);
	if (cs[0] == '\n') {
		xrow += cnt;
		xoff = 0;
	} else
		xoff = off + cnt - 1;
	free(sb->s);
	free(pref);
	return cs[0] == '\n' ? 1 : 2;
}

static char rep_cmd[sizeof(icmd)];	/* the last command */
static int rep_len;

static void vc_repeat(void)
{
	for (int i = 0; i < MAX(1, vi_arg1); i++)
		term_push(rep_cmd, rep_len);
}

static void vc_execute(int cmd)
{
	static int exec_buf = -1;
	int c = term_read(), i, n = MAX(1, vi_arg1);
	char *buf = NULL;
	if (TK_INT(c))
		return;
	if (c == cmd)
		c = exec_buf;
	exec_buf = c;
	if (exec_buf >= 0)
		buf = xregs[exec_buf];
	if (!buf) {
		snprintf(vi_msg, sizeof(vi_msg), "exec buffer empty");
		return;
	}
	for (i = 0; i < n; i++)
		term_exec(buf, strlen(buf), cmd)
}

static void vi_argcmd(int arg, char cmd)
{
	char str[100];
	char *cs = itoa(arg, str);
	*cs = cmd;
	term_push(str, cs - str + 1);
}

void vi(int init)
{
	char *ln, *cs;
	int mv, n, k, c;
	xgrec++;
	if (init) {
		xtop = MAX(0, xrow - xrows / 2);
		vi_col = vi_off2col(xb, xrow, xoff);
		vi_drawagain(xtop);
		vi_drawmsg();
		term_pos(xrow - xtop, led_pos(lbuf_get(xb, xrow), vi_col));
	}
	while (!xquit) {
		int nrow = xrow;
		int noff = xoff;
		int ooff = noff;
		int otop = xtop;
		int oleft = xleft;
		int orow = xrow;
		icmd_pos = 0;
		vi_arg2 = 0;
		vi_mod = 0;
		vi_ybuf = vi_yankbuf();
		vi_arg1 = vi_prefix();
		term_dec()
		if (vi_lnnum == 1) {
			vi_lnnum = 0;
			vi_lncol = 0;
			vi_mod |= 1;
		}
		if (vi_msg[0]) {
			vi_msg[0] = '\0';
			if (!vi_status)
				vi_drawrow(otop + xrows - 1);
		}
		if (led_attsb)
			sbuf_cut(led_attsb, 0)
		if (!vi_ybuf)
			vi_ybuf = vi_yankbuf();
		mv = vi_motion(&nrow, &noff);
		if (mv > 0) {
			if (strchr("|jk", mv)) {
				noff = vi_col2off(xb, nrow, vi_col);
			} else {
				noff = noff < 0 ? lbuf_indents(xb, nrow) : noff;
				vi_mod |= 4;
			}
			if ((xrow != nrow || ooff != noff) &&
					strchr("%'`GHML/?{}[]", mv))
				lbuf_mark(xb, '`', xrow, ooff);
			xrow = nrow;
			xoff = noff;
			switch (mv) {
			case 1: /* ^a */
				if (xrow < otop + xrows - 1)
					break;
			case '/':
			case '?':
			case 'n':
			case 'N':
				xtop = MAX(0, xrow - xrows / 2);
				vi_mod |= 1;
				break;
			}
		} else if (mv == 0) {
			char *cmd;
			term_dec()
			lbuf_mark(xb, '*', xrow, xoff);
			re_motion:
			c = term_read();
			switch (c) {
			case TK_CTL('b'):
				vi_scrollbackward(MAX(1, vi_arg1) * (xrows - 1));
				xoff = lbuf_indents(xb, xrow);
				vi_mod |= 4;
				break;
			case TK_CTL('f'):
				vi_scrollforward(MAX(1, vi_arg1) * (xrows - 1));
				xoff = lbuf_indents(xb, xrow);
				vi_mod |= 4;
				break;
			case TK_CTL('e'):
				vi_scrolley = vi_arg1 ? vi_arg1 : vi_scrolley;
				vi_scrollforward(MAX(1, vi_scrolley));
				xoff = vi_col2off(xb, xrow, vi_col);
				break;
			case TK_CTL('y'):
				vi_scrolley = vi_arg1 ? vi_arg1 : vi_scrolley;
				vi_scrollbackward(MAX(1, vi_scrolley));
				xoff = vi_col2off(xb, xrow, vi_col);
				break;
			case TK_CTL('u'):
				if (xrow == 0)
					break;
				if (vi_arg1)
					vi_scrollud = vi_arg1;
				n = vi_scrollud ? vi_scrollud : xrows / 2;
				xrow = MAX(0, xrow - n);
				if (xtop > 0)
					xtop = MAX(0, xtop - n);
				xoff = lbuf_indents(xb, xrow);
				vi_mod |= 4;
				break;
			case TK_CTL('d'):
				if (xrow == lbuf_len(xb) - 1)
					break;
				if (vi_arg1)
					vi_scrollud = vi_arg1;
				n = vi_scrollud ? vi_scrollud : xrows / 2;
				xrow = MIN(MAX(0, lbuf_len(xb) - 1), xrow + n);
				if (xtop < lbuf_len(xb) - xrows)
					xtop = MIN(lbuf_len(xb) - xrows, xtop + n);
				xoff = lbuf_indents(xb, xrow);
				vi_mod |= 4;
				break;
			case TK_CTL('i'): {
				if (!(ln = lbuf_get(xb, xrow)))
					break;
				ln += xoff;
				char buf[strlen(ln)+4];
				strcpy(buf, ":e ");
				strcpy(buf+3, ln);
				term_push(buf, strlen(ln)+3);
				break; }
			case TK_CTL('n'):
				vi_cndir = vi_arg1 ? -vi_cndir : vi_cndir;
				vi_arg1 = ex_buf - bufs + vi_cndir;
			case TK_CTL('_'): /* note: this is also ^7 per ascii */
				if (vi_arg1 > 0)
					goto switchbuf;
				xleft = 0;
				ex_exec("b");
				vi_arg1 = vi_digit();
				if (vi_arg1 > -1 && vi_arg1 < xbufcur) {
					switchbuf:
					bufs_switchwft(vi_arg1 < xbufcur ? vi_arg1 : 0)
					vc_status(0);
				}
				vi_mod |= 1;
				xmpt = xmpt >= 0 ? 0 : xmpt;
				break;
			case 'u':
				undo:
				if (vi_arg1 >= 0 && !lbuf_undo(xb)) {
					lbuf_jump(xb, '*', &xrow, &xoff);
					vi_mod |= 1;
					vi_arg1--;
					goto undo;
				} else if (!vi_arg1)
					snprintf(vi_msg, sizeof(vi_msg), "undo failed");
				break;
			case TK_CTL('r'):
				redo:
				if (vi_arg1 >= 0 && !lbuf_redo(xb)) {
					lbuf_jump(xb, '*', &xrow, &xoff);
					vi_mod |= 1;
					vi_arg1--;
					goto redo;
				} else if (!vi_arg1)
					snprintf(vi_msg, sizeof(vi_msg), "redo failed");
				break;
			case TK_CTL('g'):
				vi_tsm = 0;
				status:
				if (vi_arg1) {
					vi_status = vi_arg1 % 2 ? xrows - vi_arg1 : 0;
					xrows += vi_status ? 0 : vi_arg1 - 1;
				}
				vc_status(vi_tsm);
				break;
			case TK_CTL('^'):
				bufs_switchwft(ex_pbuf - bufs)
				vc_status(0);
				vi_mod |= 1;
				break;
			case TK_CTL('k'):;
				static struct lbuf *writexb;
				if ((k = ex_exec("w")) && xb == writexb)
					k = ex_exec("se nompt:w!");
				writexb = k ? xb : NULL;
				break;
			case '#':
				if (vi_lnnum & vi_arg1)
					vi_lnnum = vi_lnnum & ~vi_arg1;
				else
					vi_lnnum = vi_arg1 ? vi_lnnum | vi_arg1 : !vi_lnnum;
				vi_lncol = 0;
				vi_mod |= 1;
				break;
			case 'v':
				vi_mod |= 2;
				k = term_read();
				switch (k) {
				case '.':
					while (vi_arg1) {
						term_push("j", 1);
						term_push(rep_cmd, rep_len);
						if (strchr("iIoOaAsScC", rep_cmd[0])) {
							/* go to the left to restore
							previous position of what
							was inserted. */
							term_push("0", 1);
							if (noff)
								vi_argcmd(noff, 'l');
						}
						vi_arg1--;
					}
					break;
				case 'a':
					xai = !xai;
					char aistr[] = "autoindent  ";
					aistr[11] = xai + '0';
					snprintf(vi_msg, sizeof(vi_msg), "%s", aistr);
					break;
				case 'j':
					vi_joinmode = !vi_joinmode;
					break;
				case 'w':
					vi_nlword = !vi_nlword;
					break;
				case 'o':
					ex_command("%s/\x0d//g:%s/[ \t]+$//g")
					vi_mod |= 1;
					break;
				case 'I':;
				case 'i':;
					char restr[100] = "%s/^\t/";
					vi_arg1 = MIN(vi_arg1 ? vi_arg1 : xtabspc, 80);
					if (k == 'I') {
						cmd = restr+6;
						while (vi_arg1--)
							*cmd++ = ' ';
						strcpy(cmd, "/g");
					} else {
						strcpy(restr, "%s/^ {");
						strcpy(itoa(vi_arg1, restr+6), "}/\t/g");
					}
					ln = vi_enprompt(":", restr, &n);
					goto do_excmd;
				case 'b':
				case 'v':
					term_push(k == 'v' ? ":\x01" : ":\x02", 2); /* ^a : ^b */
					break;
				case ';':
					ln = vi_enprompt(":", "!", &n);
					goto do_excmd;
				case '/':
					cs = vi_curword(xb, xrow, xoff, vi_arg1);
					ln = vi_prompt("v/ xkwd:", cs, &xkmap, &n);
					ex_krsset(ln + n, +1);
					free(ln);
					free(cs);
					break;
				case 't': {
					strcpy(vi_msg, "arg2:(0|#)");
					vi_drawmsg();
					cs = vi_curword(xb, xrow, xoff, vi_prefix());
					char buf[cs ? strlen(cs)+30 : 30];
					strcpy(buf, ".,.+");
					char *buf1 = itoa(vi_arg1, buf+4);
					strcat(buf1, "s/");
					if (cs) {
						strcat(buf1, cs);
						strcat(buf1, "/");
						free(cs);
					}
					ln = vi_enprompt(":", buf, &n);
					goto do_excmd; }
				case 'r': {
					cs = vi_curword(xb, xrow, xoff, vi_arg1);
					char buf[cs ? strlen(cs)+30 : 30];
					strcpy(buf, "%s/");
					if (cs) {
						strcat(buf, cs);
						strcat(buf, "/");
						free(cs);
					}
					ln = vi_enprompt(":", buf, &n);
					goto do_excmd; }
				default:
					term_dec()
				}
				break;
			case 'V':
				vi_hidch = !vi_hidch;
				vi_mod |= 1;
				break;
			case TK_CTL('v'):
				vi_arg1 = (vi_wsel % 5) + !!*vi_word;
			case TK_CTL('c'):
				if (vi_arg1 && vi_arg1 <= 5) {
					vi_wsel = vi_arg1;
					vi_word = _vi_word + vi_arg1;
				} else
					vi_word = _vi_word + (!*vi_word * vi_wsel);
				vi_mod |= 1;
				break;
			case ':':
				ln = vi_enprompt(":", 0, &n);
				do_excmd:
				if (ln && ln[n])
					ex_command(ln + n)
				free(ln);
				break;
			case 'q':
				if (term_read() == 'q')
					xquit = texec == '&' ? -1 : 1;
				continue;
			case 'c':
			case 'd':
				k = term_read();
				if (k == 'i') {
					k = term_read();
					switch(k) {
					case ')':
						term_push("F(ldt)", 6);
						break;
					case '(':
						term_push("f(ldt)", 6);
						break;
					case '"':
						term_push("f\"ldt\"", 6);
						break;
					}
					if (c == 'c')
						term_back('i');
					break;
				}
				term_dec()
			case 'y':
			case '!':
			case '>':
			case '<':
			case TK_CTL('w'):
				vc_motion(c);
				if (c == 'c')
					goto ins;
				break;
			case 'I':
			case 'i':
			case 'a':
			case 'A':
			case 'o':
			case 'O':
				vc_insert(c);
				ins:
				vi_mod |= !xpac && xrow == orow ? 8 : 1;
				if (vi_insmov == 127) {
					if (xrow && !(xoff > 0 && lbuf_eol(xb, xrow))) {
						xrow--;
						vc_join(0, 2);
					} else if (xoff)
						vi_delete(xrow, xoff - 1, xrow, xoff, 0);
					term_back(xoff != lbuf_eol(xb, xrow) ? 'i' : 'a');
					break;
				}
				if (c != 'A' && c != 'C')
					xoff--;
				break;
			case 'J':
				vc_join(vi_joinmode, vi_arg1 <= 1 ? 2 : vi_arg1);
				break;
			case 'K':
				vi_splitln(xrow, xoff+1, !vi_arg1);
				break;
			case TK_CTL('z'):
			case TK_CTL('l'):
				if (c == TK_CTL('z')) {
					term_pos(xrows, 0);
					term_suspend();
				} else {
					term_done();
					term_init();
				}
				vi_status = vi_status ? xrows - 1: vi_status;
				vi_mod |= 1;
				break;
			case 'm':
				if ((k = term_read()) > 0 && islower(k))
					lbuf_mark(xb, k, xrow, xoff);
				break;
			case 'p':
			case 'P':
				vi_mod |= vc_put(c);
				break;
			case 'z':
				k = term_read();
				switch (k) {
				case 'z':
					xquit = 2 * (texec == '&' ? -1 : 1);
					term_push("\n", 1);
					break;
				case '\n':
					xtop = vi_arg1 ? vi_arg1 : xrow;
					break;
				case '.':
					n = vi_arg1 ? vi_arg1 : xrow;
					xtop = MAX(0, n - xrows / 2);
					break;
				case '-':
					n = vi_arg1 ? vi_arg1 : xrow;
					xtop = MAX(0, n - xrows + 1);
					break;
				case 'l':
				case 'r':
					xtd = k == 'r' ? -1 : +1;
					break;
				case 'L':
				case 'R':
					xtd = k == 'R' ? -2 : +2;
					break;
				case 'e':
				case 'f':
					xkmap = k == 'e' ? 0 : xkmap_alt;
					break;
				case '1':
				case '2':
					xkmap_alt = k - '0';
					break;
				}
				rstate->s = NULL;
				vi_mod |= 1;
				break;
			case 'g':
				k = term_read();
				if (k == 'g')
					term_push("1G", 2);
				else if (k == 'a') {
					vi_tsm = 1;
					goto status;
				} else if (k == 'w') {
					char cmd[100] = "se noled:& ";
					n = vi_arg1 ? vi_arg1 - 1 : 79;
					k = xled;
					strcpy(itoa(n, cmd+10), "|");
					while (1) {
						ex_exec(cmd);
						ex_exec("se grp=2:f/[^ \t]*[^ \t]?(.):& 1K:se nogrp");
						if (vi_col < n)
							break;
						ex_exec("+1");
					}
					if (k) {
						ex_exec("se led");
						vi_mod |= 1;
					}
				} else if (k == 'q') {
					char cmd[100] = "se noled:g/./& ";
					strcpy(itoa(vi_arg1, cmd+14), "gw:se led");
					ex_command(cmd)
				} else if (k == '~' || k == 'u' || k == 'U') {
					vc_motion(k);
					goto rep;
				}
				break;
			case 'x':
				term_push("d ", 2);
				goto motion;
			case 'X':
				term_push("d", 2);
				goto motion;
			case 'D':
				term_push("d$", 2);
				goto motion;
			case 'Y':
				term_push("yy", 2);
				goto motion;
			case '~':
				term_push("g~ ", 3);
				goto motion;
			case 'C':
				term_push("c$", 2);
				goto motion;
			case 's':
				term_push("c ", 2);
				goto motion;
			case 'S':
				term_push("cc", 2);
				motion:
				icmd_pos--;
				goto re_motion;
			case 'r':
				vi_mod |= vc_replace();
				break;
			case 'R':
				ex_exec("reg");
				break;
			case 'Z':
				if (term_read() == 'Z')
					ex_exec("x");
				break;
			case '.':
				vc_repeat();
				break;
			case '@':
			case '&':
				vc_execute(c);
				break;
			case '\\':
				ex_exec("b-2");
				if (vi_arg1 && xb == tempbufs[1].lb)
					ex_exec("1,$d:fd:b-2");
				vc_status(0);
				vi_mod |= 1;
				break;
			default:
				continue;
			}
			if (strchr("!<>AIJKOPRacdiopry", c)) {
				rep:
				memcpy(rep_cmd, icmd, icmd_pos);
				rep_len = icmd_pos;
			}
		}
		if (xrow < 0 || xrow >= lbuf_len(xb))
			xrow = lbuf_len(xb) ? lbuf_len(xb) - 1 : 0;
		if (xtop > xrow)
			xtop = xtop - xrows / 2 > xrow ?
					MAX(0, xrow - xrows / 2) : xrow;
		if (xtop + xrows <= xrow)
			xtop = xtop + xrows + xrows / 2 <= xrow ?
					xrow - xrows / 2 : xrow - xrows + 1;
		ln = lbuf_get(xb, xrow);
		xoff = ren_noeol(ln, xoff);
		if (ln && !rstate->wid[xoff]) {
			n = xoff;
			do {
				if (!xoff)
					n = ooff+1;
				xoff += n > ooff ? 1 : -1;
			} while (!rstate->wid[xoff] && xoff < rstate->n);
		}
		if (vi_mod)
			vi_col = vi_off2col(xb, xrow, xoff);
		if (vi_col >= xleft + xcols)
			xleft = vi_col - xcols / 2;
		if (vi_col < xleft)
			xleft = vi_col < xcols ? 0 : vi_col - xcols / 2;
		n = led_pos(ln, ren_cursor(ln, vi_col));
		vi_wait();
		if (xhlw) {
			static char *word;
			if ((cs = vi_curword(xb, xrow, xoff, xhlw))) {
				if (!word || strcmp(word, cs)) {
					syn_addhl(cs, 1, 1);
					free(word);
					word = cs;
					vi_mod |= 1;
				}
			}
		}
		if (xhlp && (k = syn_findhl(3)) >= 0) {
			int row = xrow, off = xoff;
			int row1 = xrow, off1 = xoff;
			led_att la;
			if (!led_attsb)
				sbuf_make(led_attsb, sizeof(la) * 2)
			if (!lbuf_pair(xb, &row, &off)) {
				row1 = row; off1 = off;
				if (!lbuf_pair(xb, &row, &off)) {
					la.s = ln;
					la.off = off;
					la.att = hls[k].att[0];
					sbuf_mem(led_attsb, &la, (int)sizeof(la))
					la.s = lbuf_get(xb, row1);
					la.off = off1;
					sbuf_mem(led_attsb, &la, (int)sizeof(la))
					vi_mod |= row1 == row && orow == xrow ? 2 : 1;
				}
			}
		}
		syn_reloadft();
		term_record = 1;
		if (vi_mod & 1 || xleft != oleft
				|| (vi_lnnum && orow != xrow && !(vi_lnnum == 2))
				|| (*vi_word && orow != xrow))
			vi_drawagain(xtop);
		else if (*vi_word && ooff != xoff && xrow+1 < xtop + xrows) {
			vi_drawrow(xrow+1);
			vi_rshift = 0;
		} else if (xtop != otop)
			vi_drawupdate(otop - xtop);
		if (xhll) {
			syn_blockhl = 0;
			if (xrow != orow && orow >= xtop && orow < xtop + xrows)
				if (!(vi_mod & 1) && !*vi_word)
					vi_drawrow(orow);
			syn_addhl("^.+", 2, 1);
			syn_reloadft();
			vi_drawrow(xrow);
			syn_addhl(NULL, 2, 1);
			syn_reloadft();
		} else if (vi_mod & 2 && !(vi_mod & 1)) {
			syn_blockhl = 0;
			vi_drawrow(xrow);
		}
		if (vi_status && !vi_msg[0]) {
			xrows = vi_status != xrows ? vi_status : xrows;
			vc_status(vi_tsm);
			vi_drawmsg();
			vi_msg[0] = '\0';
		} else
			vi_drawmsg();
		term_pos(xrow - xtop, n + vi_lncol);
		term_commit();
		lbuf_modified(xb);
	}
	xgrec--;
}

static void sighandler(int signo)
{
	if (!(xvis & 4) && signo == SIGWINCH)
		term_exec("", 1, '&')
}

static int setup_signals(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sighandler;
	if (sigaction(SIGWINCH, &sa, NULL))
		return 0;
	return 1;
}

int main(int argc, char *argv[])
{
	int i, j;
	if (!setup_signals())
		return EXIT_FAILURE;
	dir_init();
	syn_init();
	temp_open(0, "/hist/", "/");
	temp_open(1, "/fm/", "/fm");
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (argv[i][1] == '-' && !argv[i][2]) {
			i++;
			break;
		}
		for (j = 1; argv[i][j]; j++) {
			if (argv[i][j] == 's')
				xvis |= 2|4;
			else if (argv[i][j] == 'e')
				xvis |= 4;
			else if (argv[i][j] == 'm')
				xvis |= 8;
			else if (argv[i][j] == 'v')
				xvis &= ~4;
			else {
				fprintf(stderr, "Unknown option: -%c\n", argv[i][j]);
				fprintf(stderr, "Usage: %s [-emsv] [file ...]\n", argv[0]);
				return EXIT_FAILURE;
			}
		}
	}
	ibuf = emalloc(ibuf_sz);
	term_init();
	ex_init(argv + i, argc - i);
	if (xvis & 4)
		ex();
	else
		vi(1);
	term_done();
	if (xvis & 4)
		return EXIT_SUCCESS;
	if (abs(xquit) == 2) {
		term_pos(xrows - !vi_status, 0);
		term_kill();
	} else
		term_clean();
	return EXIT_SUCCESS;
}
