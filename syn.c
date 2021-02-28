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

static struct bmap {
	int id;
	struct rset *rs;
} bmap[NFTS];
static int bidx;

static struct rset *syn_ftrs;
static int syn_ctx;
static int *blockatt;
struct rset *blockrs;

static int syn_find(char *ft)
{
	int i;
	for (i = 0; i < LEN(ftmap); i++)
		if (!strcmp(ft, ftmap[i].ft))
			return i;
	return -1;
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

#define setatt()\
for (i = 0; i < LEN(subs) / 2; i++) {\
	if (subs[i * 2] >= 0) {\
		int beg = uc_off(s, sidx + subs[i * 2 + 0]);\
		int end = uc_off(s, sidx + subs[i * 2 + 1]);\
		for (j = beg; j < end; j++)\
			att[j] = syn_merge(att[j], catt[i]);\
		if (i == grp)\
			cend = MAX(cend, subs[i * 2 + 1]);\
	}\
}\

int *syn_highlight(char *ft, char *s, int n)
{
	int idx = syn_find(ft);
	int *att = malloc(n * sizeof(att[0]));
	memset(att, 0, n * sizeof(att[0]));
	if (idx < 0)
		return att;
	struct rset *rs = ftmap[idx].rs;
	int sidx = 0;
	int subs[16 * 2];
	int flg = 0;
	int hl, j, i;
	if (xhll)
		for (i = 0; i < n; i++)
			att[i] = syn_ctx;
	if (blockrs)
		rs = blockrs;
	while ((hl = rset_find(rs, s + sidx, LEN(subs) / 2, subs, flg)) >= 0)
	{
		int grp = 0;
		int cend = 1;
		int *catt;
		int patend;
		conf_highlight(hl, NULL, &catt, NULL, &grp, &patend);
		if (blockrs)
		{
			catt = blockatt;
			blockrs = NULL;
			setatt()
			return att;
		} else if (patend) {
			patend += hl;
			conf_highlight(patend, NULL, &blockatt, NULL, NULL, NULL);
			for (i = 0; i < bidx; i++)
			{
				if (patend == bmap[i].id)
				{
					blockrs = bmap[i].rs;
					setatt()
					return att;
				}
			}
		}
		setatt()
		sidx += cend;
		flg = RE_NOTBOL;
	}
	if (blockrs)
	{
		for (j = 0; j < n; j++)
			att[j] = *blockatt;
	}
	return att;
}

static void syn_initft(char *name, char *inject)
{
	char *pats[128] = {NULL};
	char *ft, *pat;
	int i, n;
	for (i = 0; !conf_highlight(i, &ft, NULL, &pat, NULL, NULL)
		&& i < LEN(pats); i++)
		if (!strcmp(ft, inject) || !strcmp(ft, name))
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

void syn_reloadft(char *ft, char *injectft, int i, char *reg)
{
	int idx;
	if ((idx = syn_find(injectft)) >= 0)
	{
		conf_changereg(i + idx, reg);
		if ((idx = syn_find(ft)) >= 0)
		{
	                if (ftmap[idx].rs)
	                        rset_free(ftmap[idx].rs);
	                ftmap[idx].ft[0] = 0;
			syn_initft(ft, injectft);
		}
	}
}

void syn_init(void)
{
	char *pats[128] = {NULL};
	char *pat, *ft;
	int i, patend;
	bidx = 0;
	for (i = 0; !conf_highlight(i, &ft, NULL, NULL, NULL, &patend); i++)
	{
		if (syn_find(ft) < 0)
			syn_initft(ft, "");
		if (patend)
		{
			conf_highlight(i+patend, NULL, NULL, &pat, NULL, NULL);
			bmap[bidx].id = i+patend;
			bmap[bidx].rs = rset_make(1, &pat, 0); 
			bidx++;
		}
	}
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
	for (i = 0; i < bidx; i++)
		rset_free(bmap[i].rs);
}
