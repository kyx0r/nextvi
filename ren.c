/* rendering strings */

static rset *dir_rslr;	/* pattern of marks for left-to-right strings */
static rset *dir_rsrl;	/* pattern of marks for right-to-left strings */
static rset *dir_rsctx;	/* direction context patterns */

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
static int dir_reorder(char **chrs, int *ord, int end, int dir)
{
	rset *rs = dir < 0 ? dir_rsrl : dir_rslr;
	int beg = 0, end1 = end, c_beg, c_end;
	int subs[LEN(dmarks[0].dir) * 2], gdir, found, i;
	while (beg < end) {
		char *s = chrs[beg];
		found = rset_find(rs, s, subs,
				*chrs[end-1] == '\n' ? REG_NEWLINE : 0);
		if (found >= 0) {
			for (i = 0; i < end1; i++)
				ord[i] = i;
			c_end = 0;
			end1 = -1;
			for (i = 0; i < rs->setgrpcnt[found]; i++) {
				gdir = dmarks[found].dir[i];
				if (subs[i * 2] < 0 || gdir >= 0)
					continue;
				c_beg = uc_off(s, subs[i * 2]);
				c_end = uc_off(s, subs[i * 2 + 1]);
				dir_reverse(ord, beg+c_beg, beg+c_end);
			}
			beg += c_end ? c_end : 1;
		} else
			break;
	}
	return end1 < 0;
}

/* return the direction context of the given line */
int dir_context(char *s)
{
	int found;
	if (xtd > +1)
		return +1;
	if (xtd < -1)
		return -1;
	if (dir_rsctx && s)
		if ((found = rset_find(dir_rsctx, s, NULL, 0)) >= 0)
			return dctxs[found].dir;
	return xtd < 0 ? -1 : +1;
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

static int ren_cwid(char *s, int pos)
{
	if (s[0] == '\t')
		return xtabspc - (pos & (xtabspc-1));
	if (s[0] == '\n')
		return 1;
	int c, l; uc_code(c, s, l)
	for (int i = 0; i < phlen; i++)
		if (c >= ph[i].cp[0] && c <= ph[i].cp[1] && l == ph[i].l)
			return ph[i].wid;
	return uc_wid(c);
}

static ren_state rstates[2];
ren_state *rstate = &rstates[0];

/* specify the screen position of the characters in s */
ren_state *ren_position(char *s)
{
	if (rstate->s == s)
		return rstate;
	else if (rstate->col) {
		free(rstate->col - 2);
		free(rstate->pos);
		free(rstate->chrs);
	}
	unsigned int n, i, c = 2;
	int cpos = 0, wid, *off, *pos, *col;
	char **chrs = uc_chop(s, &n);
	pos = emalloc(((n + 1) * sizeof(pos[0])) * 2);
	off = &pos[n+1];
	rstate->ctx = dir_context(s);
	if (xorder && dir_reorder(chrs, off, n, rstate->ctx)) {
		int *wids = emalloc(n * sizeof(wids[0]));
		for (i = 0; i < n; i++) {
			pos[off[i]] = cpos;
			cpos += ren_cwid(chrs[off[i]], cpos);
		}
		col = emalloc((cpos + 2) * sizeof(col[0]));
		pos[n] = cpos;
		for (i = 0; i < n; i++) {
			wid = ren_cwid(chrs[off[i]], pos[off[i]]);
			wids[off[i]] = wid;
			while (wid--)
				col[c++] = off[i];
		}
		memcpy(off, wids, n * sizeof(wids[0]));
		free(wids);
	} else {
		for (i = 0; i < n; i++) {
			pos[i] = cpos;
			cpos += ren_cwid(chrs[i], cpos);
		}
		col = emalloc((cpos + 2) * sizeof(col[0]));
		pos[n] = cpos;
		for (i = 0; i < n; i++) {
			wid = pos[i+1] - pos[i];
			off[i] = wid;
			while (wid--)
				col[c++] = i;
		}
	}
	off[n] = 0;
	col[0] = n;
	col[1] = n;
	rstate->wid = off;
	rstate->cmax = cpos - 1;
	rstate->col = col + 2;
	rstate->s = s;
	rstate->pos = pos;
	rstate->chrs = chrs;
	rstate->n = n;
	return rstate;
}

/* convert character offset to visual position */
int ren_pos(char *s, int off)
{
	ren_state *r = ren_position(s);
	return off < r->n ? r->pos[off] : 0;
}

/* convert visual position to character offset */
int ren_off(char *s, int p)
{
	ren_state *r = ren_position(s);
	return r->col[p < r->cmax ? p : r->cmax];
}

/* adjust cursor position */
int ren_cursor(char *s, int p)
{
	if (!s)
		return 0;
	ren_state *r = ren_position(s);
	if (p >= r->cmax)
		p = r->cmax - (*r->chrs[r->col[r->cmax]] == '\n');
	int i = r->col[p];
	return r->pos[i] + r->wid[i] - 1;
}

/* return an offset before EOL */
int ren_noeol(char *s, int o)
{
	if (!s)
		return 0;
	ren_state *r = ren_position(s);
	o = o >= r->n ? r->n - 1 : MAX(0, o);
	return o - (o > 0 && *r->chrs[o] == '\n');
}

/* the visual position of the next character */
int ren_next(char *s, int p, int dir)
{
	ren_state *r = ren_position(s);
	if (p+dir < 0 || p > r->cmax)
		return r->pos[r->col[r->cmax]];
	int i = r->col[p];
	if (r->wid[i] > 1 && dir > 0)
		return r->pos[i] + r->wid[i];
	return r->pos[i] + dir;
}

char *ren_translate(char *s, char *ln)
{
	if (s[0] == '\t' || s[0] == '\n')
		return NULL;
	int c, l; uc_code(c, s, l)
	for (int i = 0; i < phlen; i++)
		if (c >= ph[i].cp[0] && c <= ph[i].cp[1] && l == ph[i].l)
			return ph[i].d;
	if (l == 1)
		return NULL;
	if (uc_acomb(c)) {
		static char buf[16] = "ـ";
		*((char*)memcpy(buf+2, s, l)+l) = '\0';
		return buf;
	}
	if (uc_isbell(c))
		return "�";
	return xshape ? uc_shape(ln, s, c) : NULL;
}

#define NFTS		30
/* mapping filetypes to regular expression sets */
static struct ftmap {
	int setbidx;
	int seteidx;
	char *ft;
	rset *rs;
} ftmap[NFTS];
static int ftmidx;
static int ftidx;

static rset *syn_ftrs;
static int last_scdir;
static int *blockatt;
static int blockcont;
int syn_reload;
int syn_blockhl;

static void syn_initft(int fti, int n, char *name)
{
	int i = n;
	char *pats[hlslen];
	for (; i < hlslen && !strcmp(hls[i].ft, name); i++)
		pats[i - n] = hls[i].pat;
	ftmap[fti].setbidx = n;
	ftmap[fti].ft = name;
	ftmap[fti].rs = rset_make(i - n, pats, 0);
	ftmap[fti].seteidx = i;
}

char *syn_setft(char *ft)
{
	for (int i = 1; i < 4; i++)
		syn_addhl(NULL, i, 0);
	for (int i = 0; i < ftmidx; i++)
		if (!strcmp(ft, ftmap[i].ft)) {
			ftidx = i;
			return ftmap[ftidx].ft;
		}
	for (int i = 0; i < hlslen; i++)
		if (!strcmp(ft, hls[i].ft)) {
			ftidx = ftmidx;
			syn_initft(ftmidx++, i, hls[i].ft);
			break;
		}
	return ftmap[ftidx].ft;
}

void syn_scdir(int scdir)
{
	if (last_scdir != scdir) {
		last_scdir = scdir;
		syn_blockhl = 0;
	}
}

int syn_merge(int old, int new)
{
	int fg = SYN_FGSET(new) ? SYN_FG(new) : SYN_FG(old);
	int bg = SYN_BGSET(new) ? SYN_BG(new) : SYN_BG(old);
	return ((old | new) & SYN_FLG) | (bg << 8) | fg;
}

void syn_highlight(int *att, char *s, int n)
{
	rset *rs = ftmap[ftidx].rs;
	int subs[rs->grpcnt * 2], sl;
	int blk = 0, blkm = 0, sidx = 0, flg = 0, hl, j, i;
	int bend = 0, cend = 0;
	while ((sl = rset_find(rs, s + sidx, subs, flg)) >= 0) {
		hl = sl + ftmap[ftidx].setbidx;
		int *catt = hls[hl].att;
		int blkend = hls[hl].blkend;
		if (blkend && sidx >= bend) {
			for (i = 0; i < rs->setgrpcnt[sl]; i++)
				if (subs[i * 2] >= 0)
					blk = i;
			blkm += blkm > abs(hls[hl].blkend) ? -1 : 1;
			if (blkm == 1 && last_scdir > 0)
				blkend = blkend < 0 ? -1 : 1;
			if (syn_blockhl == hl && blk == abs(blkend))
				syn_blockhl = 0;
			else if (!syn_blockhl && blk != blkend) {
				syn_blockhl = hl;
				blockatt = catt;
				blockcont = hls[hl].end[blk];
			} else
				blk = 0;
		}
		for (i = 0; i < rs->setgrpcnt[sl]; i++) {
			if (subs[i * 2] >= 0) {
				int beg = uc_off(s, sidx + subs[i * 2 + 0]);
				int end = uc_off(s, sidx + subs[i * 2 + 1]);
				for (j = beg; j < end; j++)
					att[j] = syn_merge(att[j], catt[i]);
				if (!hls[hl].end[i])
					cend = MAX(cend, subs[i * 2 + 1]);
				else {
					if (blkend)
						bend = MAX(cend, subs[i * 2 + 1]) + sidx;
					if (hls[hl].end[i] > 0)
						cend = MAX(cend, subs[i * 2]);
				}
			}
		}
		sidx += cend;
		cend = 1;
		flg = REG_NOTBOL;
	}
	if (syn_blockhl && !blk)
		for (j = 0; j < n; j++)
			att[j] = blockcont && att[j] ? att[j] : *blockatt;
}

char *syn_filetype(char *path)
{
	int hl = rset_find(syn_ftrs, path, NULL, 0);
	return hl >= 0 && hl < ftslen ? fts[hl].ft : hls[0].ft;
}

void syn_reloadft(void)
{
	if (syn_reload) {
		rset *rs = ftmap[ftidx].rs;
		syn_initft(ftidx, ftmap[ftidx].setbidx, ftmap[ftidx].ft);
		if (!ftmap[ftidx].rs) {
			ftmap[ftidx].rs = rs;
		} else
			rset_free(rs);
		syn_reload = 0;
	}
}

int syn_findhl(int id)
{
	for (int i = ftmap[ftidx].setbidx; i < ftmap[ftidx].seteidx; i++)
		if (hls[i].id == id)
			return i;
	return -1;
}

void syn_addhl(char *reg, int id, int reload)
{
	int ret = syn_findhl(id);
	if (ret >= 0) {
		hls[ret].pat = reg;
		syn_reload = reload;
	}
}

void syn_init(void)
{
	char *pats[ftslen];
	int i = 0;
	for (; i < ftslen; i++)
		pats[i] = fts[i].pat;
	syn_ftrs = rset_make(i, pats, 0);
}
