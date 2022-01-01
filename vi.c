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
#include <time.h>
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

int vi_lnnum;		/* line numbers */
int vi_hidch;		/* show hidden chars */
int vi_insmov;		/* moving in insert outside of insertion sbuf */
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
	if (vi_msg[0])
	{
		int oleft = xleft;
		xleft = 0;
		syn_blockhl = 0;
		syn_setft("/-");
		led_printmsg(vi_msg, xrows);
		syn_setft(ex_filetype);
		xleft = oleft;
	}
}

#define vi_drawwordnum(lbuf_word, skip, dir, tmp, nrow, noff) \
{ int i = 0; \
char snum[100]; \
int _nrow = nrow; \
int _noff = noff; \
int l1 = ren_off(tmp, xleft); \
int l2 = ren_off(tmp, xleft+xcols); \
for (int k = _nrow; k == _nrow && _noff >= l1 && _noff <= l2; i++) \
{ \
	if (tmp[_noff] == ' ') { \
		c = itoa(i%10 ? i%10 : i, snum); \
		tmp[_noff] = *snum; \
	} \
	if (lbuf_word(xb, skip, dir, &_nrow, &_noff)) \
		break; \
} } \

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
		l1 = strlen(s)+1;
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
		l1 = strlen(c)+1;
		char tmp[l1];
		memcpy(tmp, c, l1);
		for (i = 0; i < l1-1; i++)
			if (tmp[i] != '\t' && tmp[i] != '\n')
				tmp[i] = ' ';
		if (tmp[noff] == ' ')
			tmp[noff] = *vi_word;
		switch (*vi_word)
		{
		case 'e':
			vi_drawwordnum(lbuf_wordend, 0, 1, tmp, nrow, noff)
			vi_drawwordnum(lbuf_wordend, 0, -1, tmp, nrow, noff)
			break;
		case 'E':
			vi_drawwordnum(lbuf_wordend, 1, 1, tmp, nrow, noff)
			vi_drawwordnum(lbuf_wordend, 1, -1, tmp, nrow, noff)
			break;
		case 'w':
			vi_drawwordnum(lbuf_wordbeg, 0, 1, tmp, nrow, noff)
			break;
		case 'W':
			vi_drawwordnum(lbuf_wordbeg, 1, 1, tmp, nrow, noff)
			break;
		}
		movedown = 1;
		i = syn_blockhl;
		syn_blockhl = 0;
		syn_setft("/#");
		led_print(tmp, row - xtop);
		syn_setft(ex_filetype);
		syn_blockhl = i;
	} else if (!lnnum)
		led_print(s, row - xtop);
	if (row+1 == MIN(xtop + xrows, lbuf_len(xb)+movedown))
		movedown = 0;
}

/* redraw the screen */
static void vi_drawagain()
{
	term_record = 1;
	syn_scdir(0);
	syn_blockhl = 0;
	for (int i = xtop; i < xtop + xrows; i++)
		vi_drawrow(i);
	vi_drawmsg();
	term_commit();
}

/* update the screen */
static void vi_drawupdate(int otop)
{
	int i = otop - xtop;
	term_record = 1;
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
	vi_drawmsg();
	term_commit();
}

/* update the screen by removing lines r1 to r2 before an input command */
static void vi_drawrm(int r1, int r2, int newln)
{
	r1 = MIN(MAX(r1, xtop), xtop + xrows);
	r2 = MIN(MAX(r2, xtop), xtop + xrows);
	term_pos(r1 - xtop, 0);
	term_room(r1 - r2 + newln);
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
	int l2 = strlen(msg);
	memcpy(vi_msg, msg, l2+1);
	term_pos(xrows, led_pos(msg, 0));
	term_kill();
	syn_setft("/-");
	s = led_prompt(msg, "", insert, kmap);
	syn_setft(ex_filetype);
	vi_mod = 1;
	if (!s)
		return NULL;
	r = uc_dup(s + l2);
	strcpy(vi_msg+l2, r);
	free(s);
	return r;
}

/* read an ex input line */
char *ex_read(char *msg)
{
	sbuf *sb;
	int c;
	if (xled) {
		int oleft = xleft;
		syn_blockhl = 0;
		syn_setft("/-");
		char *s = led_prompt(msg, "", NULL, &xkmap);
		xleft = oleft;
		if (s)
			term_chr('\n');
		syn_setft(ex_filetype);
		return s;
	}
	sbuf_make(sb, xcols)
	while ((c = getchar()) != EOF && c != '\n')
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
	if (xvis) {
		vi_printed = 1;
		snprintf(vi_msg, sizeof(vi_msg), "%s", msg);
	} else if (xled) {
		syn_setft("/-");
		led_reprint(msg, -1);
		term_chr('\n');
		syn_setft(ex_filetype);
	} else {
		printf("%s", msg);
	}
}

/* print an ex output line */
void ex_print(char *line)
{
	syn_blockhl = 0;
	if (xvis) {
		vi_printed += line ? 1 : 2;
		if (line) {
			snprintf(vi_msg, sizeof(vi_msg), "%s", line);
			syn_setft("/-");
			led_reprint(line, -1);
			syn_setft(ex_filetype);
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
	rset *rs;
	int r = *row;
	int o = *off;
	int len = 0;
	int i, dir;
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
			ex_krsset(re[0] ? re : NULL, cmd == '/' ? +1 : -1);
			while (isspace(*kw))
				kw++;
			vi_soset = !!kw[0];
			vi_so = atoi(kw);
			free(re);
		}
		sbuf_free(sb)
	}
	if (!lbuf_len(xb) || ex_krs(&rs, &dir))
		return 1;
	dir = cmd == 'N' ? -dir : dir;
	o = *off;
	for (i = 0; i < cnt; i++) {
		if (lbuf_search(xb, rs, dir, &r, &o, &len)) {
			snprintf(vi_msg, msg, "\"%s\" not found %d/%d",
					regs['/'], i, cnt);
			break;
		}
		if (i + 1 < cnt && cmd == '/')
			o += len;
	}
	if (i) {
		*row = r;
		*off = o;
		if (vi_soset) {
			*off = -1;
			if (*row + vi_so < 0 || *row + vi_so >= lbuf_len(xb))
				i = 0;
			else
				*row += vi_so;
		}
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
	case '\033':	/* Arrow keys */
		c = vi_read();
		if (c == '[') {
			c = vi_read();
			switch (c) {
			case 'A':	/* ↑ */
				*row = MAX(*row - cnt, 0);
				c = 'k';
				break;
			case 'B':	/* ↓ */
				*row = MIN(*row + cnt, lbuf_len(xb) - 1);
				c = 'j';
				break;
			default:	/* Not a line motion so we put back all the arrow characters */
				vi_back(c);
				vi_back('[');
				vi_back('\033');
				return 0;
			}
		} else {	/* Not a 033[X command so we put back the characters in the buffer */
			vi_back(c);
			vi_back('\033');
		}
		break;
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

static char *vi_curword(struct lbuf *lb, int row, int off, int n)
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
			if (strchr("{}[]().?\\^$|*/+", *beg))
				sbuf_str(sb, "\\")
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

static void vi_regputraw(int c, const char *s, int ln)
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
	}
	vi_regputraw(c, s, ln);
}

static void vi_regdone(void)
{
	for (int i = 0; i < LEN(regs); i++)
		free(regs[i]);
}

static void vi_regprint()
{
	for (int i = 0; i < LEN(regs); i++)
	{
		if (regs[i])
		{
			int len = strlen(regs[i])+3;
			char buf[len];
			snprintf(buf, len, "%c %s", i, regs[i]);
			ex_print(buf);
		}
	}
}

rset *fsincl;
char *fslink;
char *fs_exdir;
int fstlen;
int fspos;
int fscount;

static void file_calc(char *path, char *basepath)
{
	struct dirent *dp;
	struct stat statbuf;
	int len, _len, len1;
	DIR *dir = opendir(basepath);
	int pathlen = strlen(path);
	if (!dir)
		return;
	while ((dp = readdir(dir)) != NULL)
	{
		len1 = strlen(dp->d_name)+1;
		path[pathlen] = '/';
		memcpy(&path[pathlen+1], dp->d_name, len1);
		if (fsincl)
			if (rset_find(fsincl, path, 0, NULL, 0) < 0)
				continue;
		if (lstat(path, &statbuf) >= 0 && S_ISREG(statbuf.st_mode))
		{
			len = pathlen + len1 + 1;
			_len = len + sizeof(int);
			fslink = realloc(fslink, _len+fstlen);
			*(int*)((char*)fslink+fstlen) = len;
			memcpy(fslink+fstlen+sizeof(int), path, len);
			fstlen += _len;
			fscount++;
		}
	}
	path[pathlen] = 0;
	closedir(dir);
}

void dir_calc(char *cur_dir)
{
	struct dirent *dirp;
	struct stat statbuf;
	char *ptr;
	DIR *dp;
	file_calc(cur_dir, cur_dir);
	ptr = cur_dir + strlen(cur_dir);
	*ptr++ = '/';
	*ptr = 0;
	if ((dp = opendir(cur_dir)) == NULL)
		return;
	while ((dirp = readdir(dp)) != NULL)
	{
		if (strcmp(dirp->d_name, ".") == 0 ||
			strcmp(dirp->d_name, "..") == 0)
			continue;
		strcpy(ptr, dirp->d_name);
		if (lstat(cur_dir, &statbuf) >= 0 &&
			S_ISDIR(statbuf.st_mode))
			dir_calc(cur_dir);
	}
	closedir(dp);
}

static int fs_search(int cnt, int *row, int *off)
{
	char *path;
	int again = 0;
	redo:
	for (;fspos < fstlen;)
	{
		path = &fslink[fspos+sizeof(int)];
		fspos += *(int*)((char*)fslink+fspos) + sizeof(int);
		if (ex_edit(path))
			{*row = xrow; *off = xoff-1;}
		else
			{*row = 0; *off = 0;}
		if (!vi_search('n', cnt, row, off, 0))
			return 1;
	}
	if (fspos == fstlen && !again) {
		fspos = 0;
		again = 1;
		goto redo;
	}
	return 0;
}

static int fs_searchback(int cnt, int *row, int *off)
{
	char *path;
	int tlen = 0;
	int count = fscount;
	char *paths[count];
	for (; tlen < fspos;) {
		path = &fslink[tlen+sizeof(int)];
		tlen += *(int*)((char*)fslink+tlen) + sizeof(int);
		paths[--count] = path;
	}
	for (int i = count; i < fscount; i++)
	{
		path = paths[i];
		fspos -= *(int*)((char*)path-sizeof(int))+sizeof(int);
		if (ex_edit(path))
			{*row = xrow; *off = xoff-1;}
		else
			{*row = 0; *off = 0;}
		if (!vi_search('n', cnt, row, off, 0))
			return 1;
	}
	return 0;
}

/* read a motion */
static int vi_motion(int *row, int *off)
{
	static char sdirection;
	static sbuf *savepath;
	static int _row, _off, srow, soff;
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
	case '\033':	/* Arrow keys */
		mv = vi_read();
		if (mv == '[') {
			mv = vi_read();
			switch (mv) {
			case 'D':    /* ← */
				dir = -dir;
			case 'C':    /* → */
				for (i = 0; i < cnt; i++)
					if (vi_nextcol(xb, dir, row, off))
						break;
				break;
			default:	/* Not a motion managed by this function so we put back all the arrow characters */
				vi_back(mv);
				vi_back('[');
				vi_back('\033');
				return 0;
			}
		} else {	/* Not a 033[X command so we put back the characters in the buffer */
			vi_back(mv);
			vi_back('\033');
		}
		break;
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
	case TK_CTL(']'): /* note: this is also ^5 as per ascii */
		cs = vi_curword(xb, *row, *off, cnt);
		if (!fslink)
			mdir_calc(fs_exdir ? fs_exdir : ".")
		_row = *row; _off = *off;
		if (vi_arg1 && cs)
			ex_krsset(cs, +1);
		if (!fs_search(1, row, off)) {
			*row = _row; *off = _off;
		}
		free(cs);
		break;
	case TK_CTL('p'):
		if (!fslink)
			return -1;
		cs = vi_curword(xb, *row, *off, cnt);
		if (vi_arg1 && cs)
			ex_krsset(cs, +1);
		if (!fs_searchback(1, row, off)) {
			if (savepath) {
				*row = srow; *off = soff;
				ex_edit(savepath->s);
			}
		}
		free(cs);
		break;
	case TK_CTL('t'):
		if (!savepath)
			sbuf_make(savepath, 1024)
		sbuf_cut(savepath, 0)
		sbufn_str(savepath, ex_path)
		srow = *row; soff = *off;
		break;
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
		if (!(cs = vi_curword(xb, *row, *off, cnt)))
			return -1;
		ex_krsset(cs, +1);
		free(cs);
		if (vi_search(sdirection ? 'N' : 'n', 1, row, off, sizeof(vi_msg)))
		{
			sdirection = !sdirection;
			if (vi_search(sdirection ? 'N' : 'n', 1, row, off, sizeof(vi_msg)))
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
		if (!fslink || !strcmp(ex_path, "/fm/")) {
			ec_setdir(NULL, NULL, NULL);
			mdir_calc(fs_exdir ? fs_exdir : ".")
			temp_done(1);
		}
		temp_open(1, "/fm/", "/fm");
		cs = temp_get(1, 1);
		if (!cs) {
			temp_done(1);
			temp_open(1, "/fm/", "/fm");
			for (i = 0; i < fstlen;)
			{
				cs = &fslink[i+sizeof(int)];
				i += *(int*)((char*)fslink+i) + sizeof(int);
				temp_write(1, cs);
			}
			if (!strcmp(ex_path, "/fm/"))
				break;
		}
		temp_switch(1);
		vi(1);
		temp_switch(1);
		xquit = 0;
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
	char *s, *part;
	int len, crow, bytelen;
	int l, c = !vi_arg1 ? 1 : vi_arg1;
	vi_mod = 1;
	for (int i = 0; i < c; i++)
	{
		crow = row + i;
		s = lbuf_get(xb, crow);
		if (!s)
			return;
		len = uc_slen(s);
		bytelen = 0;
		for (int z = 0; z < linepos; z++)
			{ uc_len(l, (s+bytelen)) bytelen += l; }
		if (len > linepos)
		{
			part = uc_sub(s, linepos, len);
			char buf[bytelen+2];
			memcpy(buf, s, bytelen);
			buf[bytelen] = '\n';
			buf[bytelen+1] = 0;
			lbuf_edit(xb, buf, crow, crow+1);
			lbuf_edit(xb, part, crow+1, crow+1);
			free(part);
			if (nextln)
				c++;
		}
	}
}

static int charcount(char *text, char *post)
{
	int tlen = strlen(text);
	int plen = strlen(post);
	char *nl = text;
	int i;
	if (tlen < plen)
		return 0;
	for (i = 0; i < tlen - plen; i++)
		if (text[i] == '\n')
			nl = text + i + 1;
	return uc_slen(nl) - uc_slen(post);
}

static char *vi_input(char *pref, char *post, int row)
{
	char *rep = led_input(pref, post, &xkmap, row);
	int off = *rep ? charcount(rep, post) - 1 : xoff;
	xoff = off < 0 ? 0 : off;
	return rep;
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
	vi_drawrm(r1, r2, 0);
	rep = vi_input(pref, post, -1);
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
	char *text, *rep;
	int kmap = 0;
	char *cmd = vi_prompt("!", 0, &kmap);
	if (!cmd)
		return;
	text = lbuf_cp(xb, r1, r2 + 1);
	rep = cmd_pipe(cmd, text, 1, 1);
	if (rep)
		lbuf_edit(xb, rep, r1, r2 + 1);
	free(cmd);
	free(text);
	free(rep);
}

static void vi_shift(int r1, int r2, int dir)
{
	sbuf *sb; sbuf_make(sb, 1024)
	char *ln;
	int i;
	for (i = r1; i <= r2; i++) {
		if (!(ln = lbuf_get(xb, i)))
			continue;
		if (dir > 0)
			sbuf_chr(sb, '\t')
		else
			ln = ln[0] == ' ' || ln[0] == '\t' ? ln + 1 : ln;
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
	return 0;
}

static void vc_insert(int cmd)
{
	char *pref, *post;
	char *ln = lbuf_get(xb, xrow);
	int row, off;
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
	off = xoff;
	row = xrow;
	if (cmd == 'a' || cmd == 'A')
		off++;
	if (ln && ln[0] == '\n')
		off = 0;
	cmdo = cmd == 'o' || cmd == 'O';
	pref = ln && !cmdo ? uc_sub(ln, 0, off) : vi_indents(ln);
	post = ln && !cmdo ? uc_sub(ln, off, -1) : uc_dup("\n");
	vi_drawrm(row, row, cmdo);
	rep = vi_input(pref, post, row - cmdo);
	if (*rep)
		lbuf_edit(xb, rep, row, row + !cmdo);
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

static void vc_join(int spc)
{
	sbuf *sb;
	int cnt = vi_arg1 <= 1 ? 2 : vi_arg1;
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
	snprintf(vi_msg, sizeof(vi_msg),
		"\"%s\"%c %d lines %d%% L%d C%d B%ld",
		ex_path[0] ? ex_path : "unnamed",
		lbuf_modified(xb) ? '*' : ' ', lbuf_len(xb),
		xrow * 100 / (lbuf_len(xb)+1), xrow+1,
		ren_cursor(lbuf_get(xb, xrow), col) + 1,
		abs((int)(ex_buf - bufs)) > xbufcur ? (long)-1 : ex_buf - bufs);
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
			vi_mod = 1;
		if (vi_lnnum == 1)
			vi_lnnum = 0;
		if (vi_msg[0]) {
			vi_msg[0] = '\0';
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
			switch (mv)
			{
			case TK_CTL(']'):
			case TK_CTL('p'):
				for (n = 0; n < xbufcur; n++)
					ex_bufpostfix(n);
				syn_setft(ex_filetype);
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
				char buf[strlen(ln)+3];
				buf[0] = ':';
				buf[1] = 'e';
				buf[2] = ' ';
				strcpy(buf+3, ln);
				term_push(buf, strlen(ln)+3);
				if (!strcmp(ex_path, "/fm/"))
					xquit = 1;
				vi_mod = 1;
				break; }
			case TK_CTL('n'):
				vi_cndir = vi_arg1 ? -vi_cndir : vi_cndir;
				vi_arg1 = ex_buf - bufs + vi_cndir;
			case TK_CTL('_'): /* note: this is also ^7 per ascii */
				if (vi_arg1 > 0)
					goto switchbuf;
				term_pos(xrows, led_pos(vi_msg, 0));
				term_kill();
				ex_command("b");
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
				vc_status();
				break;
			case TK_CTL('^'):
				ex_command("e #");
				if (!vi_printed)
					vc_status();
				vi_mod = 1;
				break;
			case TK_CTL('k'):
				ex_command("w");
				break;
			case '#':
				vi_lnnum = 1;
				vi_mod = 1;
				break;
			case 'v':
				vi_mod = 2;
				k = vi_read();
				switch (k) {
				case 'h':
					ex_command(".s/\\./->/");
					break;
				case 'g':
					ex_command(".s/->/\\./");
					break;
				case '.':
					while (vi_arg1)
					{
						term_push("j", 1);
						term_push(rep_cmd, rep_len);
						switch (rep_cmd[0])
						{
						case 'i':
						case 'I':
						case 'o':
						case 'O':
						case 'a':
						case 'A':
						case 's':
						case 'S':
						case 'c':
						case 'C':
							/*
							go to the left to restore
							previous position of what
							was inserted.
							*/
							term_push("0", 1);
							if (noff)
								vi_argcmd(noff, 'l');
							break;
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
				case 'o':
					ex_command("%s/\x0d//g");
					ex_command("%s/[ \t]+$//g");
					vi_mod = 1;
					break;
				case 'i':
					ln = vi_prompt(":", "%s/^ {8}/\t/g", &kmap);
					goto do_excmd;
				case 'b':
					term_push("\x02", 1); /* ^b */
					goto prompt;
				case 'v':
					term_push("\x16", 1); /* ^v */
					goto prompt;
				case '#':
					vi_lnnum = 2;
					vi_mod = 1;
					break;
				case ';':
					ln = vi_prompt(":", "!", &kmap);
					goto do_excmd;
				case '/':
					cs = vi_curword(xb, xrow, xoff, vi_arg1);
					ln = vi_prompt("(search) kwd:", cs, &kmap);
					ex_krsset(ln, +1);
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
					ln = vi_prompt(":", buf, &kmap);
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
				prompt:
				ln = vi_prompt(":", 0, &kmap);
				do_excmd:
				if (ln && ln[0])
					ex_command(ln);
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
				goto ins;
			case 'I':
			case 'i':
			case 'a':
			case 'A':
			case 'o':
			case 'O':
				vc_insert(c);
				vi_mod = xrow == orow ? 2 : 1;
				ins:
				switch (vi_insmov)
				{
				case 127:;
					k = lbuf_eol(xb, xrow);
					if (!xoff && xrow && k != 1) {
						xoff = lbuf_eol(xb, --xrow);
						vc_join(0);
						term_push(lbuf_eol(xb, xrow) > xoff ? "i" : "a", 1);
						break;
					}
					if (c == 'a' || c == 's' || c == 'c')
						xoff++;
					char push[2];
					push[0] = xoff >= k-1 ? 'x' : 'X';
					push[1] = xoff >= k-1 ? 'A' : 'i';
					term_push(push, 2);
					break;
				}
				break;
			case 'J':
				vc_join(1);
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
				vi_back(TK_CTL('h'));
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
				term_pos(xrows, led_pos(vi_msg, 0));
				term_kill();
				vi_regprint();
				vi_wait();
				break;
			case 'Z':
				k = vi_read();
				if (k == 'Z')
					ex_command("x");
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
			if ((cs = vi_curword(xb, xrow, xoff, 1))) {
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
		if (abs(vi_mod) == 1 || xleft != oleft)
			vi_drawagain();
		else if (xtop != otop)
			vi_drawupdate(otop);
		if (xhll) {
			if (xrow != orow && orow >= xtop && orow < xtop + xrows)
				if (!vi_mod)
					vi_drawrow(orow);
			syn_addhl("^.+", 2, 1);
			syn_reloadft();
			vi_drawrow(xrow);
			syn_addhl(NULL, 2, 1);
			syn_reloadft();
			syn_blockhl = 0;
		} else if (vi_mod == 2)
			vi_drawrow(xrow);
		vi_drawmsg();
		term_pos(xrow - xtop, n);
		lbuf_modified(xb);
	}
}

static void sighandler()
{
	vi_back(TK_CTL('l'));
}

static int setup_signals(void) {
	struct sigaction sa = {0};
	sa.sa_handler = sighandler;
	if (sigaction(SIGCONT, &sa, NULL) ||
			sigaction(SIGWINCH, &sa, NULL))
		return 0;
	return 1;
}

int main(int argc, char *argv[])
{
	int i;
	char *prog = strchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	xvis = strcmp("ex", prog) && strcmp("neatex", prog);
	if (!setup_signals())
		return 1;
	dir_init();
	syn_init();
	temp_open(0, "/hist/", "/");
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (argv[i][1] == 's')
			xled = 0;
		if (argv[i][1] == 'e')
			xvis = 0;
		if (argv[i][1] == 'v')
			xvis = 1;
	}
	if (xled || xvis)
		term_init();
	if (!ex_init(argv + i)) {
		if (xvis)
			vi(1);
		else
			ex();
		ex_done();
	}
	if (xled || xvis)
		term_done();
	term_clean();
	vi_regdone();
	syn_done();
	dir_done();
	led_done();
	ren_done();
	temp_done(1);
	temp_done(0);
	free(fslink);
	return 0;
}
