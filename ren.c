/* rendering strings */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vi.h"

static char *last_str;
static char **last_chrs;
static int *last_pos;
static int last_n;
int ren_torg; /* compute tab width from this position origin */

void ren_done()
{
	free(last_pos);
	free(last_chrs);
}

/* specify the screen position of the characters in s */
int *ren_position(char *s, char ***chrs, int *n)
{
	if (last_str == s && !vi_mod)
	{
		chrs[0] = last_chrs;
		*n = last_n;
		return last_pos;
	} else
		ren_done();
	int i;
	chrs[0] = uc_chop(s, n);
	int nn = *n;
	int *off, *pos;
	int cpos = 0;
	int size = (nn + 1) * sizeof(pos[0]);
	pos = malloc(size*2);
	off = pos + nn+1;
	if (xorder)
	{
		for (i = 0; i < nn; i++)
			pos[i] = i;
		dir_reorder(s, pos, chrs[0], nn);
		for (i = 0; i < nn; i++)
			off[pos[i]] = i;
		for (i = 0; i < nn; i++) {
			pos[off[i]] = cpos;
			cpos += ren_cwid(chrs[0][off[i]], cpos);
		}
	} else {
		for (i = 0; i < nn; i++) {
			pos[i] = cpos;
			cpos += ren_cwid(chrs[0][i], cpos);
		}
	}
	pos[nn] = cpos;
	last_str = s;
	last_pos = pos;
	last_chrs = chrs[0];
	last_n = *n;
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
	int *pos = ren_position(s, &c, &n);
	int ret = off < n ? pos[off] : 0;
	return ret;
}

/* convert visual position to character offset */
int ren_off(char *s, int p)
{
	int off = -1;
	int n;
	char **c;
	int *pos = ren_position(s, &c, &n);
	int i;
	p = pos_prev(pos, n, p, 1);
	for (i = 0; i < n; i++)
		if (pos[i] == p)
			off = i;
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
	vi_mod = 0;
	pos = ren_position(s, &c, &n);
	p = pos_prev(pos, n, p, 1);
	if (uc_code(uc_chr(s, ren_off(s, p))) == '\n')
		p = pos_prev(pos, n, p, 0);
	next = pos_next(pos, n, p, 0);
	p = (next >= 0 ? next : pos[n]) - 1;
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
	int *pos = ren_position(s, &c, &n);
	p = pos_prev(pos, n, p, 1);
	if (dir >= 0)
		p = pos_next(pos, n, p, 0);
	else
		p = pos_prev(pos, n, p, 0);
	return s && uc_chr(s, ren_off(s, p))[0] != '\n' ? p : -1;
}

static char *ren_placeholder(char *s)
{
	int c = uc_code(s);
	for (int i = 0; i < placeholderslen; i++)
		if (placeholders[i].s[0] == s[0] && uc_code(placeholders[i].s) == c)
			return placeholders[i].d;
	if (uc_iscomb(s, c)) {
		static char buf[16];
		char cbuf[8] = "";
		memcpy(cbuf, s, uc_len(s));
		sprintf(buf, "ـ%s", cbuf);
		return buf;
	}
	if (uc_isbell(s, c))
		return "�";
	return NULL;
}

int ren_cwid(char *s, int pos)
{
	if (s[0] == '\t')
		return xtabspc - ((pos + ren_torg) & (xtabspc-1));
	int c = uc_code(s);
	for (int i = 0; i < placeholderslen; i++)
		if (placeholders[i].s[0] == s[0] && uc_code(placeholders[i].s) == c)
			return placeholders[i].wid;
	return (c = uc_wid(s, c)) ? c : 1;
}

char *ren_translate(char *s, char *ln)
{
	char *p = ren_placeholder(s);
	return p || !xshape ? p : uc_shape(ln, s);
}

static struct rset *dir_rslr;	/* pattern of marks for left-to-right strings */
static struct rset *dir_rsrl;	/* pattern of marks for right-to-left strings */
static struct rset *dir_rsctx;	/* direction context patterns */

static int dir_match(char **chrs, int beg, int end, int ctx, int *rec,
		int *r_beg, int *r_end, int *c_beg, int *c_end, int *dir)
{
	int subs[16 * 2];
	struct rset *rs = ctx < 0 ? dir_rsrl : dir_rslr;
	int grp;
	int flg = (beg ? RE_NOTBOL : 0) | (chrs[end][0] ? RE_NOTEOL : 0);
	int found = -1;
	int l = chrs[end] - chrs[beg];
	char s[l+1];
	memcpy(s, chrs[beg], l);
	s[l] = '\0';
	if (rs)
		found = rset_find(rs, s, LEN(subs) / 2, subs, flg);
	if (found >= 0 && r_beg && r_end && c_beg && c_end) {
		*dir = dmarks[found].dir;
		grp = dmarks[found].grp;
		*r_beg = beg + uc_off(s, subs[0]);
		*r_end = beg + uc_off(s, subs[1]);
		*c_beg = subs[grp * 2 + 0] >= 0 ?
			beg + uc_off(s, subs[grp * 2 + 0]) : *r_beg;
		*c_end = subs[grp * 2 + 1] >= 0 ?
			beg + uc_off(s, subs[grp * 2 + 1]) : *r_end;
		*rec = grp > 0;
	}
	return found < 0;
}

static void dir_reverse(int *ord, int beg, int end)
{
	end--;
	while (beg < end) {
		int tmp = ord[beg];
		ord[beg] = ord[end];
		ord[end] = tmp;
		beg++;
		end--;
	}
}

/* reorder the characters based on direction marks and characters */
static void dir_fix(char **chrs, int *ord, int dir, int beg, int end)
{
	int r_beg, r_end, c_beg, c_end;
	int c_dir, c_rec;
	while (beg < end && !dir_match(chrs, beg, end, dir, &c_rec,
				&r_beg, &r_end, &c_beg, &c_end, &c_dir)) {
		if (dir < 0)
			dir_reverse(ord, r_beg, r_end);
		if (c_dir < 0)
			dir_reverse(ord, c_beg, c_end);
		if (c_beg == r_beg)
			c_beg++;
		if (c_rec)
			dir_fix(chrs, ord, c_dir, c_beg, c_end);
		beg = r_end;
	}
}

/* return the direction context of the given line */
int dir_context(char *s)
{
	int found;
	if (xtd > +1)
		return +1;
	if (xtd < -1)
		return -1;
	if (dir_rsctx)
		if ((found = rset_find(dir_rsctx, s ? s : "", 0, NULL, 0)) >= 0)
			return dctxs[found].dir;
	return xtd < 0 ? -1 : +1;
}

/* reorder the characters in s */
void dir_reorder(char *s, int *ord, char **chrs, int n)
{
	int dir = dir_context(s);
	if (n && chrs[n - 1][0] == '\n') {
		ord[n - 1] = n - 1;
		n--;
	}
	dir_fix(chrs, ord, dir, 0, n);
}

void dir_init(void)
{
	char *relr[128];
	char *rerl[128];
	char *ctx[128];
	int i;
	for (i = 0; i < dmarkslen; i++) {
		relr[i] = dmarks[i].ctx >= 0 ? dmarks[i].pat : NULL;
		rerl[i] = dmarks[i].ctx <= 0 ? dmarks[i].pat : NULL;
	}
	dir_rslr = rset_make(i, relr, 0);
	dir_rsrl = rset_make(i, rerl, 0);
	for (i = 0; i < dctxlen; i++)
		ctx[i] = dctxs[i].pat;
	dir_rsctx = rset_make(i, ctx, 0);
}

void dir_done(void)
{
	rset_free(dir_rslr);
	rset_free(dir_rsrl);
	rset_free(dir_rsctx);
}

#define NFTS		16
/* mapping filetypes to regular expression sets */
static struct ftmap {
	char ft[32];
	struct rset *rs;
} ftmap[NFTS];
static int ftidx;

static struct bmap {
	int tid;		/* target pattern idx */
	int sid;		/* self pattern idx */
	int pid;		/* tid - sid, patend */
	int mapidx;		/* self idx in bmap */
	struct rset *rs;	/* target rset */
} bmap[NFTS];
static int bidx;

static struct rset *syn_ftrs;
static int syn_ctx;
static int *blockatt;
struct bmap *blockmap;

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

void syn_setft(char *ft)
{
	ftidx = syn_find(ft);
}

void syn_blswap(int scdir, int scdiff)
{
	static int last_scdir;
	if (ftidx >= 0 && last_scdir != scdir)
	{
		last_scdir = scdir;
		blockmap = NULL;
		struct rset *rs = ftmap[ftidx].rs;
		for (int i = 0; i < bidx; i++)
		{
			if (bmap[i].sid > ftidx + rs->n)
				break;
			if ((bmap[i].pid < 0 && scdir <= 0) ||
					(bmap[i].pid > 0 && scdir > 0))
				hls[bmap[i].sid].patend = 0;
			else
				hls[bmap[i].sid].patend = bmap[i].pid;
		}
	} else if (scdiff >= 0 && blockmap && blockmap->pid > 0)
		blockmap = NULL;
}

void syn_highlight(int *att, char *s, int n)
{
	if (ftidx < 0)
		return;
	struct rset *rs = ftmap[ftidx].rs;
	int sidx = 0;
	int subs[16 * 2];
	int flg = 0;
	int blk = 0;
	int hl, j, i;
	if (xhll)
		for (i = 0; i < n; i++)
			att[i] = syn_ctx;
	if (blockmap)
		rs = blockmap->rs;
	while ((hl = rset_find(rs, s + sidx, LEN(subs) / 2, subs, flg)) >= 0)
	{
		int cend = 1;
		int grp = hls[hl].end;
		int *catt = hls[hl].att;
		int patend = hls[hl].patend;
		if (blockmap && !blk)
		{
			catt = blockatt;
			blockmap = NULL;
			rs = ftmap[ftidx].rs;
			sidx = 0;
			continue;
		} else if (patend) {
			blk = 1;
			patend += hl;
			blockatt = hls[patend].att;
			for (i = 0; i < bidx; i++)
				if (patend == bmap[i].tid)
				{
					blockmap = &bmap[i];
					if (rset_find(blockmap->rs, s, 0, NULL, flg) >= 0)
						blockmap = NULL; /* handle single line pattern */
					break;
				}
		}
		for (i = 0; i < LEN(subs) / 2; i++) {
			if (subs[i * 2] >= 0) {
				int beg = uc_off(s, sidx + subs[i * 2 + 0]);
				int end = uc_off(s, sidx + subs[i * 2 + 1]);
				for (j = beg; j < end; j++)
					att[j] = syn_merge(att[j], catt[i]);
				if (i == grp)
					cend = MAX(cend, subs[i * 2 + 1]);
			}
		}
		sidx += cend;
		flg = RE_NOTBOL;
	}
	if (blockmap && blockmap->rs == rs)
		for (j = 0; j < n; j++)
			att[j] = *blockatt;
}

static void syn_initft(char *name, char *inject)
{
	char *pats[128] = {NULL};
	int i, n;
	for (i = 0; i < hlslen && i < LEN(pats); i++)
		if (!strcmp(hls[i].ft, inject) || !strcmp(hls[i].ft, name))
			pats[i] = hls[i].pat;
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
	if (hl >= 0 && hl < ftslen)
		return fts[hl].ft;
	return "/";
}

void syn_reloadft(char *ft, char *injectft, int i, char *reg)
{
	int idx;
	if ((idx = syn_find(injectft)) >= 0)
	{
		hls[i + idx].pat = reg;
		if ((idx = syn_find(ft)) >= 0)
		{
			rset_free(ftmap[idx].rs);
			ftmap[idx].ft[0] = 0;
			syn_initft(ft, injectft);
		}
	}
}

void syn_init(void)
{
	char *pats[128] = {NULL};
	int i, e, patend;
	bidx = 0;
	for (i = 0; i < hlslen; i++)
	{
		if (syn_find(hls[i].ft) < 0)
			syn_initft(hls[i].ft, "");
		if ((patend = hls[i].patend))
		{
			bmap[bidx].tid = i+patend;
			bmap[bidx].sid = i;
			bmap[bidx].pid = patend;
			bmap[bidx].mapidx = bidx;
			if (patend > 0)
			{
				for (e = 0; e < bidx; e++)
					if (bmap[e].tid == patend + i + hls[i+patend].patend)
						{bmap[bidx].mapidx = e; break;}
			} else
				hls[i].patend = 0;
			bmap[bidx].rs = rset_make(1, &hls[i+patend].pat, 0);
			bidx++;
		}
	}
	for (i = 0; i < ftslen && i < LEN(pats); i++)
		pats[i] = fts[i].pat;
	syn_ftrs = rset_make(i, pats, 0);
}

void syn_done(void)
{
	int i;
	for (i = 0; i < LEN(ftmap); i++)
	{
		rset_free(ftmap[i].rs);
		ftmap[i].rs = NULL;
		memset(&ftmap[i].ft, 0, 32);
	}
	rset_free(syn_ftrs);
	for (i = 0; i < bidx; i++)
		rset_free(bmap[i].rs);
}
