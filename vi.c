/*
 * NEXTVI Editor
 *
 * Copyright (C) 2015-2019 Ali Gholami Rudi <ali at rudi dot ir>
 * Copyright (C) 2020-2021 Kyryl Melekhin <k dot melekhin at gmail dot com>
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
static int vi_lnnum;	/* line numbers */
static int vi_mod;	/* screen should be redrawn:
			(1: whole screen, -1: whole screen not updating vi_col, 2: current line) */
static char *vi_word = "\0eEwW";	/* line word navigation */
static int vi_arg1, vi_arg2;		/* the first and second arguments */
static char vi_msg[EXLEN+128];		/* current message */
static char vi_charlast[8];		/* the last character searched via f, t, F, or T */
static int vi_charcmd;			/* the character finding command */
static int vi_ybuf;			/* current yank buffer */
static int vi_col;			/* the column requested by | command */
static int vi_printed;			/* ex_print() calls since the last command */
static int vi_scrollud;			/* scroll amount for ^u and ^d */
static int vi_scrolley;			/* scroll amount for ^e and ^y */
static int vi_soset, vi_so;		/* search offset; 1 in "/kw/1" */
static int vi_cndir = 1;		/* ^n direction */
static int vi_status;			/* always show status */
static int vi_joinmode = 1;		/* 1: insert extra space for pad 0: raw line join */
static char *regs[256];			/* string registers */
static int lnmode[256];

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
		rstate->ren_laststr = NULL;
		preserve(int, xtd, 2)
		led_render(vi_msg, xrows, 0, xcols);
		restore(xtd)
		syn_setft(ex_ft);
	}
}

#define vi_drawwordnum(lbuf_word, skip, dir) \
i = 0; \
nrow = xrow; \
noff = xoff; \
for (int k = nrow; k == nrow; i++) \
{ \
	itoa(i%10 ? i%10 : i, snum); \
	l1 = ren_pos(c, noff) - xleft; \
	if (l1 > xcols || l1 < 0 \
		|| lbuf_word(xb, skip, dir, &nrow, &noff)) \
		break; \
	tmp[l1] = *snum; \
} \

static void vi_drawrow(int row)
{
	int l1, l2, i, lnnum = 0;
	static int movedown;
	char *c;
	char *s = lbuf_get(xb, row-movedown);
	static char ch1[1] = "~";
	static char ch2[1] = "";
	if (!s) {
		s = ch1;
		if (*vi_word && row == xrow+1)
			goto last_row;
		led_print(row ? s : ch2, row - xtop);
		return;
	}
	if (vi_lnnum == 1 || (vi_lnnum == 2 && row != xrow))
	{
		lnnum = 1;
		l1 = *(int*)(s - sizeof(int)) + 2;
		char tmp[l1+100];
		c = itoa(row+1-movedown, tmp);
		l2 = strlen(tmp)+1;
		*c++ = ' ';
		for (i = 0; i < l1; i++)
			if (s[i] != '\t' && s[i] != ' ')
				break;
		if (!s[i])
			i = 0;
		memcpy(c, s, i);
		c = itoa(abs(xrow-row+movedown), tmp+l2+i);
		*c++ = ' ';
		memcpy(c, s+i, l1-i);
		led_reprint(tmp, row - xtop);
	}
	if (*vi_word && row == xrow+1) {
		last_row:;
		int noff = xoff;
		int nrow = xrow;
		c = lbuf_get(xb, xrow);
		if (!c || *c == '\n') {
			led_print(s, row - xtop);
			return;
		}
		char tmp[xcols+3];
		char snum[100];
		memset(tmp, ' ', xcols+1);
		tmp[xcols+1] = '\n';
		tmp[xcols+2] = '\0';
		switch (*vi_word) {
		case 'e':
			vi_drawwordnum(lbuf_wordend, 0, 1)
			vi_drawwordnum(lbuf_wordend, 0, -1)
			break;
		case 'E':
			vi_drawwordnum(lbuf_wordend, 1, 1)
			vi_drawwordnum(lbuf_wordend, 1, -1)
			break;
		case 'w':
			vi_drawwordnum(lbuf_wordbeg, 0, 1)
			break;
		case 'W':
			vi_drawwordnum(lbuf_wordbeg, 1, 1)
			break;
		}
		tmp[ren_pos(c, xoff) - xleft] = *uc_chr(c, xoff) == '\t' ? ' ' : *vi_word;
		preserve(int, xorder, 0)
		preserve(int, syn_blockhl, 0)
		preserve(int, xtd, dir_context(c) * 2)
		movedown = 1;
		syn_setft("/#");
		led_render(tmp, row - xtop, 0, xcols);
		syn_setft(ex_ft);
		restore(xorder)
		restore(syn_blockhl)
		restore(xtd)
	} else if (!lnnum)
		led_print(s, row - xtop);
	if (row+1 == MIN(xtop + xrows, lbuf_len(xb)+movedown))
		movedown = 0;
}

/* redraw the screen */
static void vi_drawagain(void)
{
	syn_scdir(0);
	syn_blockhl = 0;
	for (int i = xtop; i < xtop + xrows; i++)
		vi_drawrow(i);
}

/* update the screen */
static void vi_drawupdate(int otop)
{
	int i = otop - xtop;
	term_pos(0, 0);
	term_room(i);
	syn_scdir(i > 1 || i < -1 ? -1 : i);
	if (i < 0) {
		int n = MIN(-i, xrows);
		for (i = 0; i < n; i++)
			vi_drawrow(xtop + xrows - n + i);
	} else {
		int n = MIN(i, xrows);
		for (i = 0; i < n; i++)
			vi_drawrow(xtop + i);
	}
}

static int vi_buf[128];
static unsigned int vi_buflen;

static int vi_read(void)
{
	return vi_buflen ? vi_buf[--vi_buflen] : term_read();
}

static void vi_back(int c)
{
	if (vi_buflen < sizeof(vi_buf))
		vi_buf[vi_buflen++] = c;
}

static char *vi_char(void)
{
	return led_read(&xkmap, term_read());
}

static void vi_wait(void)
{
	if (vi_printed > 1) {
		strcpy(vi_msg, "[any key to continue] ");
		vi_drawmsg();
		vi_char();
		vi_msg[0] = '\0';
		vi_mod = 1;
	}
	vi_printed = 0;
}

static char *vi_prompt(char *msg, char *insert, int *kmap)
{
	char *r, *s;
	term_pos(xrows, led_pos(msg, 0));
	syn_setft("/-");
	s = led_prompt(msg, "", insert, kmap);
	syn_setft(ex_ft);
	vi_mod = 1;
	if (!s)
		return NULL;
	unsigned int mlen = strlen(msg);
	r = uc_dup(s + mlen);
	strncpy(vi_msg, msg, sizeof(vi_msg) - 1);
	mlen = MIN(mlen, sizeof(vi_msg) - 1);
	strncpy(vi_msg + mlen, r, sizeof(vi_msg) - mlen - 1);
	free(s);
	return r;
}

/* read an ex input line */
char *ex_read(char *msg)
{
	sbuf *sb;
	int c;
	if (term_sbuf && xled) {
		int oleft = xleft;
		syn_blockhl = 0;
		syn_setft("/-");
		char *s = led_prompt(msg, "", NULL, &xkmap);
		xleft = oleft;
		if (s)
			term_chr('\n');
		syn_setft(ex_ft);
		return s;
	}
	sbuf_make(sb, 128)
	while ((c = term_read()) != EOF && c != '\n')
		sbuf_chr(sb, c)
	if (c == EOF) {
		sbuf_free(sb)
		return NULL;
	}
	sbufn_done(sb)
}

/* show an ex message */
void ex_show(char *msg)
{
	if (!(xvis & 4)) {
		snprintf(vi_msg, sizeof(vi_msg), "%s", msg);
	} else if (term_sbuf) {
		syn_setft("/-");
		led_reprint(msg, -1);
		term_chr('\n');
		syn_setft(ex_ft);
	} else {
		write(1, msg, dstrlen(msg, '\n'));
		write(1, "\n", 1);
	}
}

/* print an ex output line */
void ex_print(char *line)
{
	syn_blockhl = 0;
	if (!(xvis & 4)) {
		vi_printed += line ? 1 : 2;
		if (line) {
			snprintf(vi_msg, sizeof(vi_msg), "%s", line);
			syn_setft("/-");
			led_reprint(line, -1);
			syn_setft(ex_ft);
		}
		term_chr('\n');
	} else if (line)
		ex_show(line);
}

static int vi_yankbuf(void)
{
	int c = vi_read();
	if (c == '"')
		return vi_read();
	vi_back(c);
	return 0;
}

static int vi_off2col(struct lbuf *lb, int row, int off)
{
	char *ln = lbuf_get(lb, row);
	return ln ? ren_pos(ln, off) : 0;
}

static int vi_prefix(void)
{
	int n = 0;
	int c = vi_read();
	if ((c >= '1' && c <= '9')) {
		while (isdigit(c)) {
			n = n * 10 + c - '0';
			c = vi_read();
		}
	}
	vi_back(c);
	return n;
}

static int vi_digit(void)
{
	int c = vi_read();
	if ((c >= '0' && c <= '9')) {
		if (isdigit(c))
			return c - '0';
	}
	return -1;
}

static int vi_col2off(struct lbuf *lb, int row, int col)
{
	char *ln = lbuf_get(lb, row);
	return ln ? ren_off(ln, col) : 0;
}

static int vi_nextoff(struct lbuf *lb, int dir, int *row, int *off)
{
	int o = *off + dir;
	if (o < 0 || !lbuf_get(lb, *row) || o >= uc_slen(lbuf_get(lb, *row)))
		return 1;
	*off = o;
	return 0;
}

static int vi_nextcol(struct lbuf *lb, int dir, int *row, int *off)
{
	char *ln = lbuf_get(lb, *row);
	int col = ln ? ren_pos(ln, *off) : 0;
	int o = ln ? ren_next(ln, col, dir) : -1;
	if (o < 0)
		return -1;
	*off = ren_off(ln, o);
	return 0;
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
	int i, dir, len = 0;
	if (cmd == '/' || cmd == '?') {
		char sign[4] = {cmd};
		sbuf *sb;
		char *kw = vi_prompt(sign, 0, &xkmap);
		char *re;
		if (!kw)
			return 1;
		sbuf_make(sb, 1024)
		sbuf_chr(sb, cmd)
		sbufn_str(sb, kw)
		free(kw);
		kw = sb->s;
		if ((re = re_read(&kw))) {
			ex_krsset(re, cmd == '/' ? +2 : -2);
			while (isspace(*kw))
				kw++;
			vi_soset = !!kw[0];
			vi_so = atoi(kw);
			free(re);
		}
		sbuf_free(sb)
	} else if (msg)
		ex_krsset(regs['/'], xkwddir);
	if (!lbuf_len(xb) || (!xkwddir || !xkwdrs))
		return 1;
	dir = cmd == 'N' ? -xkwddir : xkwddir;
	for (i = 0; i < cnt; i++) {
		if (lbuf_search(xb, xkwdrs, dir, row, off, &len)) {
			snprintf(vi_msg, msg, "\"%s\" not found %d/%d",
					regs['/'], i, cnt);
			break;
		}
		if (i + 1 < cnt && cmd == '/')
			*off += len;
	}
	if (i && vi_soset) {
		*off = -1;
		if (*row + vi_so < 0 || *row + vi_so >= lbuf_len(xb))
			i = 0;
		else
			*row += vi_so;
	}
	return !i;
}

/* read a line motion */
static int vi_motionln(int *row, int cmd)
{
	int cnt = (vi_arg1 ? vi_arg1 : 1) * (vi_arg2 ? vi_arg2 : 1);
	int c = vi_read();
	int mark, mark_row, mark_off;
	switch (c) {
	case '\n':
	case '+':
		*row = MIN(*row + cnt, lbuf_len(xb) - 1);
		break;
	case '-':
		*row = MAX(*row - cnt, 0);
		break;
	case '_':
		*row = MIN(*row + cnt - 1, lbuf_len(xb) - 1);
		break;
	case '\'':
		if ((mark = vi_read()) <= 0)
			return -1;
		if (lbuf_jump(xb, mark, &mark_row, &mark_off))
			return -1;
		*row = mark_row;
		break;
	case 'j':
		*row = MIN(*row + cnt, lbuf_len(xb) - 1);
		break;
	case 'k':
		*row = MAX(*row - cnt, 0);
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
			*row = MAX(0, lbuf_len(xb) - 1) * cnt / 100;
			break;
		}
		vi_back(c);
		return 0;
	}
	if (*row < 0)
		*row = 0;
	return c;
}

static char *vi_curword(struct lbuf *lb, int row, int off, int n, int x)
{
	sbuf *sb;
	char *ln = lbuf_get(lb, row);
	char *beg, *end;
	if (!ln || !n)
		return NULL;
	beg = uc_chr(ln, ren_noeol(ln, off));
	end = beg;
	while (beg > ln && uc_kind(uc_beg(ln, beg - 1)) == 1)
		beg = uc_beg(ln, beg - 1);
	for (int i = n; *end && i > 0; end++, i--) {
		while (*end && uc_kind(end) == 1)
			end = uc_next(end);
	}
	if (beg >= --end)
		return NULL;
	sbuf_make(sb, (end - beg)+64)
	if (n > 1) {
		for (; beg != end; beg++) {
			if (strchr("!#%{}[]().?\\^$|*/+", *beg))
				sbuf_str(sb, *beg == x ? "\\\\" : "\\")
			sbuf_chr(sb, *beg)
		}
		sbufn_done(sb)
	}
	sbuf_str(sb, "\\<")
	sbuf_mem(sb, beg, end - beg)
	sbuf_str(sb, "\\>")
	sbufn_done(sb)
}

char *vi_regget(int c, int *ln)
{
	*ln = lnmode[c];
	return regs[c];
}

static void vi_regputraw(unsigned char c, const char *s, int ln)
{
	char *pre = isupper(c) && regs[tolower(c)] ? regs[tolower(c)] : "";
	char *buf = malloc(strlen(pre) + strlen(s) + 1);
	strcpy(buf, pre);
	strcat(buf, s);
	free(regs[tolower(c)]);
	regs[tolower(c)] = buf;
	lnmode[tolower(c)] = ln;
}

void vi_regput(int c, const char *s, int ln)
{
	int i, i_ln;
	char *i_s;
	if (ln || strchr(s, '\n')) {
		for (i = 8; i > 0; i--)
			if ((i_s = vi_regget('0' + i, &i_ln)))
				vi_regputraw('0' + i + 1, i_s, i_ln);
		vi_regputraw('1', s, ln);
	} else if (vi_regget(c, &i))
		vi_regputraw('0', vi_regget(c, &i), ln);
	vi_regputraw(c, s, ln);
}

static void vi_regprint(void)
{
	term_pos(xrows, led_pos(vi_msg, 0));
	xleft = (xcols / 2) * vi_arg1;
	for (int i = 1; i < LEN(regs); i++) {
		if (regs[i]) {
			char buf[xcols * 5 + 3];
			snprintf(buf, xcols * 5 + 3, "%c %s", i, regs[i]);
			ex_print(xleft ? regs[i] : buf);
		}
	}
}

rset *fsincl;
char *fs_exdir;
static int fspos;
static int fsdir;

static char *file_calc(char *path)
{
	struct dirent *dp;
	struct stat statbuf;
	int len;
	DIR *dir = opendir(path);
	int pathlen = strlen(path);
	if (!dir)
		return path + pathlen;
	while ((dp = readdir(dir)) != NULL) {
		len = strlen(dp->d_name)+1;
		path[pathlen] = '/';
		memcpy(&path[pathlen+1], dp->d_name, len);
		if (fsincl && rset_find(fsincl, path, 0, NULL, 0) < 0)
			continue;
		if (lstat(path, &statbuf) >= 0 && S_ISREG(statbuf.st_mode))
			temp_write(1, path);
	}
	closedir(dir);
	path[pathlen] = '/';
	path[pathlen+1] = '\0';
	return path + pathlen + 1;
}

void dir_calc(char *path)
{
	struct dirent *dirp;
	struct stat statbuf;
	char *ptr;
	int i = 0;
	char cur_dir[4096];
	char *ptrs[1024];
	DIR *dps[1024];
	DIR *dp;
	strcpy(cur_dir, path);
	temp_pos(1, -1, 0, 0);
	fspos = 0;
	goto start;
	while (i > 0) {
		while ((dirp = readdir(dp)) != NULL) {
			if (strcmp(dirp->d_name, ".") == 0 ||
				strcmp(dirp->d_name, "..") == 0)
				continue;
			strcpy(ptr, dirp->d_name);
			if (lstat(cur_dir, &statbuf) >= 0 &&
					S_ISDIR(statbuf.st_mode)) {
				start:
				ptr = file_calc(cur_dir);
				if ((dp = opendir(cur_dir)) == NULL)
					return;
				dps[++i] = dp;
				ptrs[i] = ptr;
			}
		}
		closedir(dp);
		dp = dps[--i];
		ptr = ptrs[i];
	}
}

#define fssearch() \
len = *(int*)(path - sizeof(int)); \
path[len] = '\0'; \
ret = ex_edit(path, len); \
path[len] = '\n'; \
if (ret && xrow) { \
	*row = xrow; *off = xoff-1; /* short circuit */ \
	if (!vi_search('n', cnt, row, off, 0)) \
		return 1; \
	*off += 2; \
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
		path = lbuf_get(tempbufs[1].lb, fspos++);
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
		path = lbuf_get(tempbufs[1].lb, fspos);
		fssearch()
	}
	return 0;
}

/* read a motion */
static int vi_motion(int *row, int *off)
{
	static char ca_dir;
	static sbuf *savepath[10];
	static int srow[10], soff[10], lkwdcnt;
	int cnt = (vi_arg1 ? vi_arg1 : 1) * (vi_arg2 ? vi_arg2 : 1);
	char *ln = lbuf_get(xb, *row);
	int dir = dir_context(ln ? ln : "");
	int mark, mark_row, mark_off;
	char *cs;
	int mv, i;

	if ((mv = vi_motionln(row, 0))) {
		*off = -1;
		return mv;
	}
	mv = vi_read();
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
		dir = -dir;
	case 'l':
		for (i = 0; i < cnt; i++)
			if (vi_nextcol(xb, dir, row, off))
				break;
		break;
	case 'f':
	case 'F':
	case 't':
	case 'T':
		if (!(cs = vi_char()))
			return -1;
		if (vi_findchar(xb, cs, mv, cnt, row, off))
			return -1;
		break;
	case 'B':
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, 1, -1, row, off))
				break;
		break;
	case 'E':
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, 1, +1, row, off))
				break;
		break;
	case 'b':
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, 0, -1, row, off))
				break;
		break;
	case 'e':
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, 0, +1, row, off))
				break;
		break;
	case 'W':
		for (i = 0; i < cnt; i++)
			if (lbuf_wordbeg(xb, 1, +1, row, off))
				break;
		break;
	case 'w':
		for (i = 0; i < cnt; i++)
			if (lbuf_wordbeg(xb, 0, +1, row, off))
				break;
		break;
	case '{':
		for (i = 0; i < cnt; i++)
			if (lbuf_paragraphbeg(xb, -1, row, off))
				break;
		break;
	case '}':
		for (i = 0; i < cnt; i++)
			if (lbuf_paragraphbeg(xb, +1, row, off))
				break;
		break;
	case '[':
		if (vi_read() != '[')
			return -1;
		for (i = 0; i < cnt; i++)
			if (lbuf_sectionbeg(xb, -1, row, off))
				break;
		break;
	case ']':
		if (vi_read() != ']')
			return -1;
		for (i = 0; i < cnt; i++)
			if (lbuf_sectionbeg(xb, +1, row, off))
				break;
		break;
	case TK_CTL(']'):	/* note: this is also ^5 as per ascii */
	case TK_CTL('p'):
		#define open_saved(n) \
		if (savepath[n]) { \
			*row = srow[n]; *off = soff[n]; \
			ex_edit(savepath[n]->s, savepath[n]->s_n); \
		} \

		if (vi_arg1 && (cs = vi_curword(xb, *row, *off, cnt, 0))) {
			ex_krsset(cs, +1);
			free(cs);
		}
		struct buf* tmpex_buf = istempbuf(ex_buf) ? ex_pbuf : ex_buf;
		if (mv == TK_CTL(']')) {
			if (vi_arg1 || lkwdcnt != xkwdcnt)
				term_exec("qq", 3, /*nop*/, /*nop*/)
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
		return '\\';
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
		*off = vi_col2off(xb, *row, cnt - 1);
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
		if (!(cs = vi_curword(xb, *row, *off, cnt, 0)))
			return -1;
		ex_krsset(cs, +1);
		free(cs);
		if (vi_search(ca_dir ? 'N' : 'n', 1, row, off, sizeof(vi_msg))) {
			ca_dir = !ca_dir;
			if (vi_search(ca_dir ? 'N' : 'n', 1, row, off, sizeof(vi_msg)))
				return -1;
		}
		break;
	case ' ':
		for (i = 0; i < cnt; i++)
			if (vi_nextoff(xb, +1, row, off))
				break;
		break;
	case 127:
	case TK_CTL('h'):
		for (i = 0; i < cnt; i++)
			if (vi_nextoff(xb, -1, row, off))
				break;
		break;
	case '`':
		if ((mark = vi_read()) <= 0)
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
	case '\\':
		temp_switch(1);
		*row = xrow; *off = xoff;
		break;
	default:
		vi_back(mv);
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
	sbuf *sb;
	char *s1, *s2, *s3;
	if (r1 == r2)
		return uc_sub(lbuf_get(lb, r1), o1, o2);
	sbuf_make(sb, 1024)
	s1 = uc_sub(lbuf_get(lb, r1), o1, -1);
	s3 = uc_sub(lbuf_get(lb, r2), 0, o2);
	s2 = lbuf_cp(lb, r1 + 1, r2);
	sbuf_str(sb, s1)
	sbuf_str(sb, s2)
	sbuf_str(sb, s3)
	free(s1);
	free(s2);
	free(s3);
	sbufn_done(sb)
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
	post = lnmode ? uc_dup("\n") : uc_sub(lbuf_get(xb, r2), o2, -1);
	if (!lnmode) {
		sbuf *sb; sbuf_make(sb, 1024)
		sbuf_str(sb, pref)
		sbufn_str(sb, post)
		lbuf_edit(xb, sb->s, r1, r2 + 1);
		sbuf_free(sb)
	} else
		lbuf_edit(xb, NULL, r1, r2 + 1);
	xrow = r1;
	xoff = lnmode ? lbuf_indents(xb, xrow) : o1;
	free(pref);
	free(post);
}

static void vi_splitln(int row, int linepos, int nextln)
{
	char *s, *part, *buf;
	int slen, crow;
	int c = !vi_arg1 ? 1 : vi_arg1;
	vi_mod = 1;
	for (int i = 0; i < c; i++) {
		crow = row + i;
		s = lbuf_get(xb, crow);
		if (!s)
			return;
		slen = uc_slen(s);
		if (slen > linepos) {
			part = uc_sub(s, linepos, slen);
			buf = uc_sub(s, 0, linepos);
			lbuf_edit(xb, buf, crow, crow+1);
			lbuf_edit(xb, part, crow+1, crow+1);
			free(part);
			free(buf);
			if (nextln)
				c++;
		}
	}
}

static int charcount(char *text, int tlen, char *post)
{
	int plen = strlen(post);
	char *nl = text;
	if (tlen < plen)
		return 0;
	for (int i = 0; i < tlen - plen; i++)
		if (text[i] == '\n')
			nl = text + i + 1;
	return uc_slen(nl) - uc_slen(post);
}

static char *vi_input(char *pref, char *post, int row)
{
	sbuf *rep = led_input(pref, post, &xkmap, row);
	xoff = *rep->s ? charcount(rep->s, rep->s_n, post) : xoff;
	sbufn_done(rep)
}

static char *vi_indents(char *ln)
{
	sbuf *sb; sbuf_make(sb, 256)
	while (xai && ln && (*ln == ' ' || *ln == '\t'))
		sbuf_chr(sb, *ln++)
	sbufn_done(sb)
}

static void vi_change(int r1, int o1, int r2, int o2, int lnmode)
{
	char *region, *rep, *pref, *post;
	region = lbuf_region(xb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	vi_regput(vi_ybuf, region, lnmode);
	free(region);
	pref = lnmode ? vi_indents(lbuf_get(xb, r1)) : uc_sub(lbuf_get(xb, r1), 0, o1);
	post = lnmode ? uc_dup("\n") : uc_sub(lbuf_get(xb, r2), o2, -1);
	term_pos(r1 - xtop < 0 ? 0 : r1 - xtop, 0);
	term_room(r1 < xtop ? xtop - xrow : r1 - r2);
	xrow = r1;
	if (r1 < xtop)
		xtop = r1;
	rep = vi_input(pref, post, r1 - r2 ? r1 - (r1 - r2) : -1);
	if (*rep)
		lbuf_edit(xb, rep, r1, r2 + 1);
	free(rep);
	free(pref);
	free(post);
}

static void vi_case(int r1, int o1, int r2, int o2, int lnmode, int cmd)
{
	char *pref, *post;
	char *region, *s;
	region = lbuf_region(xb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	s = region;
	while (*s) {
		int c = (unsigned char) s[0];
		if (c <= 0x7f) {
			if (cmd == 'u')
				s[0] = tolower(c);
			if (cmd == 'U')
				s[0] = toupper(c);
			if (cmd == '~')
				s[0] = islower(c) ? toupper(c) : tolower(c);
		}
		s = uc_next(s);
	}
	pref = lnmode ? uc_dup("") : uc_sub(lbuf_get(xb, r1), 0, o1);
	post = lnmode ? uc_dup("\n") : uc_sub(lbuf_get(xb, r2), o2, -1);
	if (!lnmode) {
		sbuf *sb; sbuf_make(sb, 256)
		sbuf_str(sb, pref)
		sbuf_str(sb, region)
		sbufn_str(sb, post)
		lbuf_edit(xb, sb->s, r1, r2 + 1);
		sbuf_free(sb)
	} else
		lbuf_edit(xb, region, r1, r2 + 1);
	xrow = r2;
	xoff = lnmode ? lbuf_indents(xb, r2) : o2;
	free(region);
	free(pref);
	free(post);
}

static void vi_pipe(int r1, int r2)
{
	int kmap = 0;
	char region[100];
	char *p = itoa(r1+1, region);
	*p++ = ',';
	p = itoa(r2+1, p);
	*p++ = '!';
	*p = '\0';
	char *cmd = vi_prompt(":", region, &kmap);
	if (!cmd)
		return;
	ex_command(cmd)
	free(cmd);
}

static void vi_shift(int r1, int r2, int dir)
{
	sbuf *sb; sbuf_make(sb, 1024)
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
	sbuf_free(sb)
}

static int vc_motion(int cmd)
{
	int r1 = xrow, r2 = xrow;	/* region rows */
	int o1 = xoff, o2;		/* visual region columns */
	int lnmode = 0;			/* line-based region */
	int mv;
	vi_arg2 = vi_prefix();
	if (vi_arg2 < 0)
		return 1;
	o1 = ren_noeol(lbuf_get(xb, r1), o1);
	o2 = o1;
	if ((mv = vi_motionln(&r2, cmd))) {
		o2 = -1;
	} else if (!(mv = vi_motion(&r2, &o2))) {
		vi_read();
		return 1;
	}
	if (mv < 0)
		return 1;
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
	if (cmd == 'y')
		vi_yank(r1, o1, r2, o2, lnmode);
	if (cmd == 'd')
		vi_delete(r1, o1, r2, o2, lnmode);
	if (cmd == 'c')
		vi_change(r1, o1, r2, o2, lnmode);
	if (cmd == '~' || cmd == 'u' || cmd == 'U')
		vi_case(r1, o1, r2, o2, lnmode, cmd);
	if (cmd == '!') {
		if (mv == '{' || mv == '}')
			if (lbuf_get(xb, r2) && *lbuf_get(xb, r2) == '\n' && r1 < r2)
				r2--;
		vi_pipe(r1, r2);
	}
	if (cmd == '>' || cmd == '<')
		vi_shift(r1, r2, cmd == '>' ? +1 : -1);
	if (cmd == TK_CTL('w'))
		for (int lc = r1; lc <= r2; lc++) {
			xoff = 2;
			while (xoff > 1)
				vi_shift(lc, lc, -1);
			vi_shift(lc, lc, -1);
		}
	return 0;
}

static void vc_insert(int cmd)
{
	char *pref, *post;
	char *ln = lbuf_get(xb, xrow);
	int row;
	char *rep;
	int cmdo;
	if (cmd == 'I')
		xoff = lbuf_indents(xb, xrow);
	else if (cmd == 'A')
		xoff = lbuf_eol(xb, xrow);
	else if (cmd == 'o') {
		xrow++;
		if (xrow - xtop == xrows) {
			xtop++;
			vi_drawagain();
		}
	}
	xoff = ren_noeol(ln, xoff);
	row = xrow;
	if (cmd == 'a' || cmd == 'A')
		xoff++;
	if (ln && ln[0] == '\n')
		xoff = 0;
	cmdo = cmd == 'o' || cmd == 'O';
	pref = ln && !cmdo ? uc_sub(ln, 0, xoff) : vi_indents(ln);
	post = ln && !cmdo ? uc_sub(ln, xoff, -1) : uc_dup("\n");
	term_pos(row - xtop, 0);
	term_room(cmdo);
	rep = vi_input(pref, post, row - cmdo);
	if (*rep) {
		if (cmdo && !lbuf_len(xb))
			lbuf_edit(xb, "\n", 0, 0);
		lbuf_edit(xb, rep, row, row + !cmdo);
	}
	free(rep);
	free(pref);
	free(post);
}

static int vc_put(int cmd)
{
	int cnt = MAX(1, vi_arg1);
	int lnmode;
	char *buf = vi_regget(vi_ybuf, &lnmode);
	int i;
	if (!buf)
		snprintf(vi_msg, sizeof(vi_msg), "yank buffer empty");
	if (!buf || !buf[0])
		return 1;
	sbuf *sb; sbuf_make(sb, 1024)
	if (lnmode) {
		for (i = 0; i < cnt; i++)
			sbufn_str(sb, buf)
		if (!lbuf_len(xb))
			lbuf_edit(xb, "\n", 0, 0);
		if (cmd == 'p')
			xrow++;
		lbuf_edit(xb, sb->s, xrow, xrow);
		xoff = lbuf_indents(xb, xrow);
		sbuf_free(sb)
		return 0;
	}
	char *ln = xrow < lbuf_len(xb) ? lbuf_get(xb, xrow) : "\n";
	int off = ren_noeol(ln, xoff) + (ln[0] != '\n' && cmd == 'p');
	char *s = uc_sub(ln, 0, off);
	sbuf_str(sb, s)
	free(s);
	for (i = 0; i < cnt; i++)
		sbuf_str(sb, buf)
	s = uc_sub(ln, off, -1);
	sbufn_str(sb, s)
	free(s);
	lbuf_edit(xb, sb->s, xrow, xrow + 1);
	xoff = off + uc_slen(buf) * cnt - 1;
	sbuf_free(sb)
	return 0;
}

static int join_spaces(char *prev, char *next)
{
	int prevlen = strlen(prev);
	if (!prev[0])
		return 0;
	if (prev[prevlen - 1] == ' ' || next[0] == ')')
		return 0;
	return prev[prevlen - 1] == '.' ? 2 : 1;
}

static void vc_join(int spc, int cnt)
{
	sbuf *sb;
	int beg = xrow;
	int end = xrow + cnt;
	int off = 0, i;
	if (!lbuf_get(xb, beg) || !lbuf_get(xb, end - 1))
		return;
	sbufn_make(sb, 1024)
	for (i = beg; i < end; i++) {
		char *ln = lbuf_get(xb, i);
		char *lnend = strchr(ln, '\n');
		int spaces;
		if (i > beg)
			while (ln[0] == ' ' || ln[0] == '\t')
				ln++;
		spaces = i > beg && spc ? join_spaces(sb->s, ln) : 0;
		off = uc_slen(sb->s);
		while (spaces--)
			sbuf_chr(sb, ' ')
		sbufn_mem(sb, ln, lnend - ln)
	}
	sbufn_chr(sb, '\n')
	lbuf_edit(xb, sb->s, beg, end);
	xoff = off;
	sbuf_free(sb)
	vi_mod = 1;
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

static void vc_status(void)
{
	int col = vi_off2col(xb, xrow, xoff);
	long buf = ex_buf - bufs;
	snprintf(vi_msg, sizeof(vi_msg),
		"\"%s\"%c %d lines %d%% L%d C%d B%ld",
		ex_path[0] ? ex_path : "unnamed",
		lbuf_modified(xb) ? '*' : ' ', lbuf_len(xb),
		xrow * 100 / (lbuf_len(xb)+1), xrow+1,
		ren_cursor(lbuf_get(xb, xrow), col) + 1,
		buf >= xbufcur || buf < 0 ? tempbufs - ex_buf - 1 : buf);
}

static void vc_charinfo(void)
{
	char *c = uc_chr(lbuf_get(xb, xrow), xoff);
	if (c) {
		char cbuf[8] = "";
		int l; uc_len(l, c)
		memcpy(cbuf, c, l);
		uc_code(l, c)
		snprintf(vi_msg, sizeof(vi_msg), "<%s> %04x", cbuf, l);
	}
}

static int vc_replace(void)
{
	int cnt = MAX(1, vi_arg1);
	char *cs = vi_char();
	char *ln = lbuf_get(xb, xrow);
	sbuf *sb;
	char *pref, *post;
	char *s;
	int off, i;
	if (!ln || !cs)
		return 1;
	off = ren_noeol(ln, xoff);
	s = uc_chr(ln, off);
	for (i = 0; s[0] != '\n' && i < cnt; i++)
		s = uc_next(s);
	if (i < cnt)
		return 1;
	pref = uc_sub(ln, 0, off);
	post = uc_sub(ln, off + cnt, -1);
	sbuf_make(sb, 1024)
	sbuf_str(sb, pref)
	for (i = 0; i < cnt; i++)
		sbuf_str(sb, cs)
	sbufn_str(sb, post)
	lbuf_edit(xb, sb->s, xrow, xrow + 1);
	off += cnt - 1;
	xoff = off;
	sbuf_free(sb)
	free(pref);
	free(post);
	return 0;
}

static char rep_cmd[4096];	/* the last command */
static int rep_len;

static void vc_repeat(void)
{
	for (int i = 0; i < MAX(1, vi_arg1); i++)
		term_push(rep_cmd, rep_len);
}

static void vc_execute(void)
{
	static int exec_buf = -1;
	int lnmode;
	int c = vi_read();
	char *buf = NULL;
	int i;
	if (TK_INT(c))
		return;
	if (c == '@')
		c = exec_buf;
	exec_buf = c;
	if (exec_buf >= 0)
		buf = vi_regget(exec_buf, &lnmode);
	if (buf)
		for (i = 0; i < MAX(1, vi_arg1); i++)
			term_push(buf, strlen(buf));
	else
		snprintf(vi_msg, sizeof(vi_msg), "exec buffer empty");
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
	int mark, kmap = 0;
	char *ln, *cs;
	if (init) {
		xtop = MAX(0, xrow - xrows / 2);
		vi_col = vi_off2col(xb, xrow, xoff);
		vi_drawagain();
		vi_drawmsg();
		term_pos(xrow - xtop, led_pos(lbuf_get(xb, xrow), vi_col));
	}
	while (!xquit) {
		int nrow = xrow;
		int noff = ren_noeol(lbuf_get(xb, xrow), xoff);
		int otop = xtop;
		int oleft = xleft;
		int orow = xrow;
		int mv, n;
		term_cmd(&n);
		vi_arg2 = 0;
		vi_mod = 0;
		vi_ybuf = vi_yankbuf();
		vi_arg1 = vi_prefix();
		if (*vi_word || vi_lnnum)
			vi_mod = -1;
		if (vi_lnnum == 1)
			vi_lnnum = 0;
		if (vi_msg[0]) {
			vi_msg[0] = '\0';
			if (vi_status)
				vc_status();
			vi_drawrow(otop + xrows - 1);
		}
		if (!vi_ybuf)
			vi_ybuf = vi_yankbuf();
		mv = vi_motion(&nrow, &noff);
		if (mv > 0) {
			if (strchr("\'`GHML/?{}[]nN", mv) ||
					(mv == '%' && noff < 0)) {
				lbuf_mark(xb, '\'', xrow, xoff);
				lbuf_mark(xb, '`', xrow, xoff);
			}
			xrow = nrow;
			if (noff < 0 && !strchr("jk", mv))
				noff = lbuf_indents(xb, xrow);
			else if (strchr("jk", mv))
				noff = vi_col2off(xb, xrow, vi_col);
			xoff = ren_noeol(lbuf_get(xb, xrow), noff);
			if (!strchr("|jk", mv))
				vi_col = vi_off2col(xb, xrow, xoff);
			switch (mv) {
			case '\\':
				vc_status();
			case 1: /* ^a */
			case '/':
			case '?':
			case 'n':
			case 'N':
				if (mv == 1 && xrow < otop + xrows - 1)
					break;
				xtop = MAX(0, xrow - xrows / 2);
				vi_mod = 1;
				break;
			}
		} else if (mv == 0) {
			char *cmd;
			int c = vi_read();
			int k = 0;
			if (c <= 0)
				continue;
			lbuf_mark(xb, '*', xrow, xoff);
			switch (c) {
			case TK_CTL('b'):
				vi_scrollbackward(MAX(1, vi_arg1) * (xrows - 1));
				xoff = lbuf_indents(xb, xrow);
				vi_mod = 1;
				break;
			case TK_CTL('f'):
				vi_scrollforward(MAX(1, vi_arg1) * (xrows - 1));
				xoff = lbuf_indents(xb, xrow);
				vi_mod = 1;
				break;
			case TK_CTL('e'):
				vi_scrolley = vi_arg1 ? vi_arg1 : vi_scrolley;
				vi_scrollforward(MAX(1, vi_scrolley));
				xoff = vi_col2off(xb, xrow, vi_col);
				if (vi_scrolley > 1 || vi_mod)
					vi_mod = -1;
				break;
			case TK_CTL('y'):
				vi_scrolley = vi_arg1 ? vi_arg1 : vi_scrolley;
				vi_scrollbackward(MAX(1, vi_scrolley));
				xoff = vi_col2off(xb, xrow, vi_col);
				if (vi_scrolley > 1 || vi_mod)
					vi_mod = -1;
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
				vi_mod = 1;
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
				vi_mod = 1;
				break;
			case TK_CTL('z'):
				term_pos(xrows, 0);
				term_suspend();
				vi_mod = 1;
				break;
			case TK_CTL('i'): {
				if (!(ln = lbuf_get(xb, xrow)))
					break;
				ln += xoff;
				char buf[strlen(ln)+4];
				buf[0] = ':';
				buf[1] = 'e';
				buf[2] = ' ';
				strcpy(buf+3, ln);
				term_push(buf, strlen(ln)+3);
				break; }
			case TK_CTL('n'):
				vi_cndir = vi_arg1 ? -vi_cndir : vi_cndir;
				vi_arg1 = ex_buf - bufs + vi_cndir;
			case TK_CTL('_'): /* note: this is also ^7 per ascii */
				if (vi_arg1 > 0)
					goto switchbuf;
				term_pos(xrows, led_pos(vi_msg, 0));
				xleft = 0;
				ex_exec("b");
				vi_arg1 = vi_digit();
				if (vi_arg1 > -1 && vi_arg1 < xbufcur) {
					switchbuf:
					bufs_switchwft(vi_arg1 < xbufcur ? vi_arg1 : 0)
					vc_status();
				}
				vi_mod = 1;
				vi_printed = 0;
				break;
			case 'u':
				undo:
				if (vi_arg1 >= 0 && !lbuf_undo(xb)) {
					lbuf_jump(xb, '*', &xrow, &xoff);
					vi_mod = 1;
					vi_arg1--;
					goto undo;
				} else if (!vi_arg1)
					snprintf(vi_msg, sizeof(vi_msg), "undo failed");
				break;
			case TK_CTL('r'):
				redo:
				if (vi_arg1 >= 0 && !lbuf_redo(xb)) {
					lbuf_jump(xb, '*', &xrow, &xoff);
					vi_mod = 1;
					vi_arg1--;
					goto redo;
				} else if (!vi_arg1)
					snprintf(vi_msg, sizeof(vi_msg), "redo failed");
				break;
			case TK_CTL('g'):
				vi_status = vi_arg1;
				vc_status();
				break;
			case TK_CTL('^'):
				bufs_switchwft(ex_pbuf - bufs)
				vc_status();
				vi_mod = 1;
				break;
			case TK_CTL('k'):
				ex_exec("w");
				break;
			case '#':
				vi_lnnum = !vi_arg1 ? 1 : 2;
				vi_mod = 1;
				break;
			case 'v':
				vi_mod = 2;
				k = vi_read();
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
				case 'o':
					ex_command("%s/\x0d//g|%s/[ \t]+$//g")
					vi_mod = 1;
					break;
				case 'i':
					ln = vi_prompt(":", "%s/^ {8}/\t/g", &kmap);
					goto do_excmd;
				case 'b':
				case 'v':
					vi_back(':');
					term_push(k == 'v' ? "\x01" : "\x02", 1); /* ^a : ^b */
					break;
				case ';':
					ln = vi_prompt(":", "!", &kmap);
					goto do_excmd;
				case '/':
					cs = vi_curword(xb, xrow, xoff, vi_arg1, 0);
					ln = vi_prompt("v/ xkwd:", cs, &kmap);
					ex_krsset(ln, +1);
					free(ln);
					free(cs);
					break;
				case 't': {
					strcpy(vi_msg, "arg2:(0|#)");
					vi_drawmsg();
					cs = vi_curword(xb, xrow, xoff, vi_prefix(), '|');
					char buf[cs ? strlen(cs)+30 : 30];
					strcpy(buf, ".,.+");
					char *buf1 = itoa(vi_arg1, buf+4);
					strcat(buf1, "s/");
					if (cs) {
						strcat(buf1, cs);
						strcat(buf1, "/");
						free(cs);
					}
					ln = vi_prompt(":", buf, &kmap);
					goto do_excmd; }
				case 'r': {
					cs = vi_curword(xb, xrow, xoff, vi_arg1, '|');
					char buf[cs ? strlen(cs)+30 : 30];
					strcpy(buf, "%s/");
					if (cs) {
						strcat(buf, cs);
						strcat(buf, "/");
						free(cs);
					}
					ln = vi_prompt(":", buf, &kmap);
					goto do_excmd; }
				default:
					vi_back(k);
				}
				break;
			case 'V':
				vi_hidch = !vi_hidch;
				vi_mod = 1;
				break;
			case TK_CTL('v'):
				vi_word++;
				if (!*vi_word)
					vi_word -= 5;
				if (vi_arg1)
					while (*vi_word) vi_word--;
				vi_mod = 1;
				break;
			case ':':
				ln = vi_prompt(":", 0, &kmap);
				do_excmd:
				if (ln && ln[0])
					ex_command(ln)
				free(ln);
				if (xquit)
					continue;
				break;
			case 'q':
				k = vi_read();
				if (k == 'q')
					xquit = 1;
				continue;
			case 'c':
			case 'd':
				k = vi_read();
				if (k == 'i') {
					k = vi_read();
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
						term_push("i", 1);
					break;
				}
				vi_back(k);
			case 'y':
			case '!':
			case '>':
			case '<':
			case TK_CTL('w'):
				if (!vc_motion(c))
					vi_mod = 1;
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
				vi_mod = !xpac && xrow == orow ? 3 : 1;
				ins:
				if (vi_insmov == 127) {
					vi_mod = vi_mod == 3 ? 2 : 1;
					if (xrow && !(xoff > 0 && lbuf_eol(xb, xrow))) {
						xoff = lbuf_eol(xb, --xrow);
						vc_join(0, 2);
					} else if (xoff)
						vi_delete(xrow, xoff - 1, xrow, xoff, 0);
					vi_back(xoff != lbuf_eol(xb, xrow) ? 'i' : 'a');
					break;
				}
				if (c != 'A' && c != 'C')
					xoff--;
				xoff = xoff < 0 ? 0 : xoff;
				break;
			case 'J':
				vc_join(vi_joinmode, vi_arg1 <= 1 ? 2 : vi_arg1);
				break;
			case 'K':
				vi_splitln(xrow, xoff+1, 0);
				break;
			case TK_CTL('l'):
				term_done();
				term_init();
				vi_mod = 1;
				break;
			case 'm':
				if ((mark = vi_read()) > 0 && islower(mark))
					lbuf_mark(xb, mark, xrow, xoff);
				break;
			case 'p':
			case 'P':
				if (!vc_put(c))
					vi_mod = 1;
				break;
			case 'z':
				k = vi_read();
				switch (k) {
				case 'z':
					xquit = 2;
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
				vi_mod = 1;
				break;
			case 'g':
				k = vi_read();
				if (k == 'g')
					term_push("1G", 2);
				else if (k == 'a')
					vc_charinfo();
				else if (k == 'w')
					if (*uc_chr(lbuf_get(xb, xrow), xoff+1) != '\n')
						term_push("080|gwbhKj", 10);
					else
						ibuf_cnt = 0;
				else if (k == 'q')
					vi_splitln(xrow, 80, 1);
				else if (k == '~' || k == 'u' || k == 'U')
					if (!vc_motion(k))
						vi_mod = 2;
				break;
			case 'x':
				vi_back(' ');
				if (!vc_motion('d'))
					vi_mod = 2;
				break;
			case 'X':
				vi_back(127);
				if (!vc_motion('d'))
					vi_mod = 2;
				break;
			case 'C':
				vi_back('$');
				if (!vc_motion('c'))
					vi_mod = 1;
				goto ins;
			case 'D':
				vi_back('$');
				if (!vc_motion('d'))
					vi_mod = 2;
				break;
			case 'r':
				if (!vc_replace())
					vi_mod = 2;
				break;
			case 's':
				vi_back(' ');
				if (!vc_motion('c'))
					vi_mod = 1;
				goto ins;
			case 'S':
				vi_back('c');
				if (!vc_motion('c'))
					vi_mod = 1;
				goto ins;
			case 'Y':
				vi_back('y');
				vc_motion('y');
				break;
			case 'R':
				vi_mod = 1;
				vi_regprint();
				break;
			case 'Z':
				k = vi_read();
				if (k == 'Z')
					ex_exec("x");
				break;
			case '~':
				vi_back(' ');
				if (!vc_motion('~'))
					vi_mod = 2;
				break;
			case '.':
				vc_repeat();
				break;
			case '@':
				vc_execute();
				break;
			default:
				continue;
			}
			cmd = term_cmd(&n);
			if (strchr("!<>ACDIJKOPRSXYacdioprsxy~", c) ||
					(c == 'g' && strchr("uU~", k))) {
				if ((unsigned int)n < sizeof(rep_cmd)) {
					memcpy(rep_cmd, cmd, n);
					rep_len = n;
				}
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
		xoff = ren_noeol(lbuf_get(xb, xrow), xoff);
		if (vi_mod > 0)
			vi_col = vi_off2col(xb, xrow, xoff);
		if (vi_col >= xleft + xcols)
			xleft = vi_col - xcols / 2;
		if (vi_col < xleft)
			xleft = vi_col < xcols ? 0 : vi_col - xcols / 2;
		n = led_pos(lbuf_get(xb, xrow), ren_cursor(lbuf_get(xb, xrow), vi_col));
		vi_wait();
		if (xhlw) {
			static char *word;
			if ((cs = vi_curword(xb, xrow, xoff, xhlw, 0))) {
				if (!word || strcmp(word, cs)) {
					syn_addhl(cs, 1, 1);
					free(word);
					word = cs;
					vi_mod = 1;
				}
			}
		}
		if (xhlp && (cs = lbuf_get(xb, xrow))) {
			char pairs[] = "{([})]{([})]";
			cs = uc_chr(cs, xoff);
			int start = uc_off(cs, strcspn(cs, "{([})]"));
			int ch = dstrlen(pairs, *uc_chr(cs, start));
			if (ch < 9) {
				static sbuf *sb;
				char buf[100];
				int pos = xleft ? ren_off(lbuf_get(xb, xrow), xleft) : 0;
				start += xoff - pos;
				int row = xrow, off = start + pos;
				if (!lbuf_pair(xb, &row, &off)) {
					off -= pos;
					if (sb)
						sbuf_free(sb)
					sbuf_make(sb, 128)
					if (off && row == xrow && off - start - 1 < 0)
					{
						ch += 3;
						swap(&start, &off);
					}
					if (start) {
						sbuf_str(sb, "^.{")
						itoa(start, buf);
						sbuf_str(sb, buf)
						sbuf_str(sb, "}(\\{)")
					} else
						sbuf_str(sb, "^(\\{)")
					sb->s[sb->s_n-2] = pairs[ch];
					if (off && row == xrow) {
						sbuf_str(sb, ".{")
						itoa(abs(off - start - 1), buf);
						sbuf_str(sb, buf)
						sbuf_str(sb, "}(\\})")
					} else if (off) {
						sbuf_str(sb, "|^.{")
						itoa(off, buf);
						sbuf_str(sb, buf)
						sbuf_str(sb, "}(\\})")
					} else
						sbuf_str(sb, "|^(\\})")
					sb->s[sb->s_n-2] = pairs[ch+3];
					sbuf_null(sb)
					syn_addhl(sb->s, 3, 1);
					vi_mod = 1;
				}
			}
		}
		syn_reloadft();
		term_record = 1;
		if (abs(vi_mod) == 1 || xleft != oleft)
			vi_drawagain();
		else if (xtop != otop)
			vi_drawupdate(otop);
		if (xhll) {
			syn_blockhl = 0;
			if (xrow != orow && orow >= xtop && orow < xtop + xrows)
				if (!vi_mod)
					vi_drawrow(orow);
			syn_addhl("^.+", 2, 1);
			syn_reloadft();
			vi_drawrow(xrow);
			syn_addhl(NULL, 2, 1);
			syn_reloadft();
		} else if (vi_mod == 2) {
			syn_blockhl = 0;
			vi_drawrow(xrow);
		}
		vi_drawmsg();
		term_pos(xrow - xtop, n);
		term_commit();
		lbuf_modified(xb);
	}
}

static void sighandler(int signo)
{
	vi_back(TK_CTL('l'));
}

static int setup_signals(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sighandler;
	if (sigaction(SIGCONT, &sa, NULL) ||
			sigaction(SIGWINCH, &sa, NULL))
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
				xvis |= 2;
			else if (argv[i][j] == 'e')
				xvis |= 4;
			else if (argv[i][j] == 'v')
				xvis = 0;
			else {
				fprintf(stderr, "Unknown option: -%c\n", argv[i][j]);
				fprintf(stderr, "Usage: %s [-esv] [file ...]\n", argv[0]);
				return EXIT_FAILURE;
			}
		}
	}
	term_init();
	ex_init(argv + i, argc - i);
	if (xvis & 4)
		ex();
	else
		vi(1);
	term_done();
	if (xvis & 4)
		return EXIT_SUCCESS;
	if (xquit == 2) {
		term_pos(xrows - 1, 0);
		term_kill();
	} else
		term_clean();
	return EXIT_SUCCESS;
}
