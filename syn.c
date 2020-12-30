#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vi.h"

#define NFTS		16

/* mapping filetypes to regular expression sets */
static struct ftmap {
	char ft[32];
	struct rset *rs;
} ftmap[NFTS];

static struct rset *syn_ftrs;
static int syn_ctx;
int blockpat;

static struct rset *syn_find(char *ft)
{
	int i;
	for (i = 0; i < LEN(ftmap); i++)
		if (!strcmp(ft, ftmap[i].ft))
			return ftmap[i].rs;
	return NULL;
}

int syn_merge(int old, int new)
{
	int fg = SYN_FGSET(new) ? SYN_FG(new) : SYN_FG(old);
	int bg = SYN_BGSET(new) ? SYN_BG(new) : SYN_BG(old);
	return ((old | new) & SYN_FLG) | (bg << 8) | fg;
}

void syn_context(int att)
{
	syn_ctx = att;
}

int *syn_highlight(char *ft, char *s, int n, int cbeg, int cend)
{
	struct rset *rs = syn_find(ft);
	int *att = malloc(n * sizeof(att[0]));
	memset(att, 0, n * sizeof(att[0]));
	if (!rs)
		return att;
	int sidx = 0;
	int subs[16 * 2];
	int flg = 0;
	int hl, j, i, bpat = 0;
	char ctmp = 0;
	int xright;
	for (i = cbeg; i < n; i++)
		att[i] = syn_ctx;
	if (n > cend)
	{
		xright = utf8_w2nb(s, cend);
		ctmp = s[xright];
		s[xright] = '\0';
	}
	while ((hl = rset_find(rs, s + sidx, LEN(subs) / 2, subs, flg)) >= 0
		|| blockpat) {
		int grp = 0;
		int cend = 1;
		int *catt;
		int patend;
		conf_highlight(hl, NULL, &catt, NULL, &grp, &patend);
		if (blockpat)
		{
			if (hl == blockpat)
			{
				blockpat = 0;
				goto skip_last;
			}
			conf_highlight(blockpat, NULL, &catt, NULL, NULL, NULL);
			for (j = cbeg; j < n; j++)
				att[j] = *catt;
			goto ret;
		} else if (patend)
			bpat = patend;
		skip_last:
		for (i = 0; i < LEN(subs) / 2; i++) {
			if (subs[i * 2] >= 0) {
				int beg = uc_off(s, sidx + subs[i * 2 + 0]);
				int end = uc_off(s, sidx + subs[i * 2 + 1]);
				for (j = beg; j < end; j++)
					att[j+cbeg] = syn_merge(att[j+cbeg], catt[i]);
				if (i == grp)
					cend = MAX(cend, subs[i * 2 + 1]);
			}
		}
		sidx += cend;
		flg = RE_NOTBOL;
	}
	if (bpat)
		blockpat = bpat;
	ret:
	if (ctmp)
		s[xright] = ctmp;
	return att;
}

static void syn_initft(char *name)
{
	char *pats[128] = {NULL};
	char *ft, *pat;
	int i, n;
	for (i = 0; !conf_highlight(i, &ft, NULL, &pat, NULL, NULL) 
		&& i < LEN(pats); i++)
		if (*ft == '/' || !strcmp(ft, name))
			pats[i] = pat;
	n = i;
	for (i = 0; i < LEN(ftmap); i++) {
		if (!ftmap[i].ft[0]) {
			strcpy(ftmap[i].ft, name);
			ftmap[i].rs = rset_make(n, pats, 0);
			return;
		}
	}
}

char *syn_filetype(char *path)
{
	int hl = rset_find(syn_ftrs, path, 0, NULL, 0);
	char *ft;
	if (!conf_filetype(hl, &ft, NULL))
		return ft;
	return "/";
}

void syn_init(void)
{
	char *pats[128] = {NULL};
	char *pat, *ft;
	int i;
	for (i = 0; !conf_highlight(i, &ft, NULL, NULL, NULL, NULL); i++)
		if (!syn_find(ft))
			syn_initft(ft);
	for (i = 0; !conf_filetype(i, NULL, &pat) && i < LEN(pats); i++)
		pats[i] = pat;
	syn_ftrs = rset_make(i, pats, 0);
}

void syn_done(void)
{
	int i;
	for (i = 0; i < LEN(ftmap); i++)
	{
		if (ftmap[i].rs)
			rset_free(ftmap[i].rs);
		ftmap[i].rs = NULL;
		memset(&ftmap[i].ft, 0, 32);
	}
	rset_free(syn_ftrs);
}
