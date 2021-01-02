/* rendering strings */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vi.h"

/* specify the screen position of the characters in s */
int *ren_position(char *s, char ***chrs, int *n, int *noff)
{
	int i; 
	chrs[0] = uc_chop(s, n);
	int nn = *n;
	int *off, *pos;
	int cpos = 0;
	int size = (nn + 1) * sizeof(pos[0]);
	pos = malloc(size*2);
	off = pos + nn+1;
	for (i = 0; i < nn; i++)
		pos[i] = i;
	if (xorder)
		dir_reorder(s, pos, chrs[0], nn);
	for (i = 0; i < nn; i++)
		off[pos[i]] = i;
	if (noff)
	{
		int _cpos, notab = 0, done = 0;
		for (i = 0; i < nn; i++) {
			pos[off[i]] = cpos;
			_cpos = ren_cwid(chrs[0][off[i]], cpos);
			if (chrs[0][off[i]][0] == '\t')
				notab++;
			else
				notab += _cpos;
			if (!done && *noff == off[i])
			{
				*noff = notab;
				done = 1;
			}
			cpos += _cpos;
		}
	} else {
		for (i = 0; i < nn; i++) {
			pos[off[i]] = cpos;
			cpos += ren_cwid(chrs[0][off[i]], cpos);
		}
	}
	pos[nn] = cpos;
	return pos;
}

/* find the next character after visual position p; if cur, start from p itself */
static int pos_next(int *pos, int n, int p, int cur)
{
	int i, ret = -1;
	for (i = 0; i < n; i++)
		if (pos[i] - !cur >= p && (ret < 0 || pos[i] < pos[ret]))
			ret = i;
	return ret >= 0 ? pos[ret] : -1;
}

/* find the previous character after visual position p; if cur, start from p itself */
static int pos_prev(int *pos, int n, int p, int cur)
{
	int i, ret = -1;
	for (i = 0; i < n; i++)
		if (pos[i] + !cur <= p && (ret < 0 || pos[i] > pos[ret]))
			ret = i;
	return ret >= 0 ? pos[ret] : -1;
}

/* convert character offset to visual position */
int ren_pos(char *s, int off)
{
	int n;
	char **c;
	int *pos = ren_position(s, &c, &n, 0);
	int ret = off < n ? pos[off] : 0;
	free(pos);
	free(c);
	return ret;
}

/* convert visual position to character offset */
int ren_off(char *s, int p)
{
	int off = -1;
	int n;
	char **c;
	int *pos = ren_position(s, &c, &n, 0);
	int i;
	p = pos_prev(pos, n, p, 1);
	for (i = 0; i < n; i++)
		if (pos[i] == p)
			off = i;
	free(pos);
	free(c);
	return off >= 0 ? off : 0;
}

/* adjust cursor position */
int ren_cursor(char *s, int p)
{
	int n, next;
	int *pos;
	char **c;
	if (!s)
		return 0;
	pos = ren_position(s, &c, &n, 0);
	p = pos_prev(pos, n, p, 1);
	if (uc_code(uc_chr(s, ren_off(s, p))) == '\n')
		p = pos_prev(pos, n, p, 0);
	next = pos_next(pos, n, p, 0);
	p = (next >= 0 ? next : pos[n]) - 1;
	free(pos);
	free(c);
	return p >= 0 ? p : 0;
}

/* return an offset before EOL */
int ren_noeol(char *s, int o)
{
	int n = s ? uc_slen(s) : 0;
	if (o >= n)
		o = MAX(0, n - 1);
	return o > 0 && uc_chr(s, o)[0] == '\n' ? o - 1 : o;
}

/* the position of the next character */
int ren_next(char *s, int p, int dir)
{
	int n;
	char **c;
	int *pos = ren_position(s, &c, &n, 0);
	p = pos_prev(pos, n, p, 1);
	if (dir >= 0)
		p = pos_next(pos, n, p, 0);
	else
		p = pos_prev(pos, n, p, 0);
	free(pos);
	free(c);
	return s && uc_chr(s, ren_off(s, p))[0] != '\n' ? p : -1;
}

static char *ren_placeholder(char *s)
{
	char *src, *dst;
	int wid, i;
	int c = uc_code(s);
	for (i = 0; !conf_placeholder(i, &src, &dst, &wid); i++)
		if (uc_code(src) == c)
			return dst;
	if (uc_iscomb(s)) {
		static char buf[16];
		char cbuf[8] = "";
		memcpy(cbuf, s, uc_len(s));
		sprintf(buf, "ـ%s", cbuf);
		return buf;
	}
	if (uc_isbell(s))
		return "�";
	return NULL;
}

int ren_cwid(char *s, int pos)
{
	char *src, *dst;
	int wid, i;
	if (s[0] == '\t')
		return xtabspc - (pos & (xtabspc-1));
	for (i = 0; !conf_placeholder(i, &src, &dst, &wid); i++)
		if (uc_code(src) == uc_code(s))
			return wid;
	return uc_wid(s);
}

char *ren_translate(char *s, char *ln)
{
	char *p = ren_placeholder(s);
	return p || !xshape ? p : uc_shape(ln, s);
}
