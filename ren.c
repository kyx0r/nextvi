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
static int dir_reorder(char **chrs, int *ord, int end)
{
	int dir = dir_context(chrs[0]);
	rset *rs = dir < 0 ? dir_rsrl : dir_rslr;
	if (!rs)
		return 0;
	int beg = 0, end1 = end, r_beg, r_end, c_beg, c_end;
	int subs[32], grp, found;
	while (beg < end) {
		char *s = chrs[beg];
		found = rset_find(rs, s, 16, subs,
				*chrs[end-1] == '\n' ? REG_NEWLINE : 0);
		if (found >= 0) {
			for (int i = 0; i < end1; i++)
				ord[i] = i;
			end1 = -1;
			grp = dmarks[found].grp;
			r_beg = uc_off(s, subs[0]);
			r_end = uc_off(s, subs[1]);
			c_beg = subs[grp * 2 + 0] >= 0 ? uc_off(s, subs[grp * 2 + 0]) : r_beg;
			c_end = subs[grp * 2 + 1] >= 0 ? uc_off(s, subs[grp * 2 + 1]) : r_end;
			if (dir < 0)
				dir_reverse(ord, r_beg+beg, r_end+beg);
			if (dmarks[found].dir < 0)
				dir_reverse(ord, c_beg+beg, c_end+beg);
			beg = r_end+beg;
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
		if ((found = rset_find(dir_rsctx, s, 0, NULL, 0)) >= 0)
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

void dir_done(void)
{
	rset_free(dir_rslr);
	rset_free(dir_rsrl);
	rset_free(dir_rsctx);
}

static char **ren_lastchrs;
static int *ren_lastpos;
static int ren_lastn;
char *ren_laststr; 	/* prevent redundant computations, you must ensure pointer uniqueness */
int ren_torg; 		/* compute tab width from this position origin */

void ren_done(void)
{
	free(ren_lastpos);
	free(ren_lastchrs);
}

/* specify the screen position of the characters in s */
int *ren_position(char *s, char ***chrs, int *n)
{
	if (ren_laststr == s) {
		chrs[0] = ren_lastchrs;
		*n = ren_lastn;
		return ren_lastpos;
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
	if (xorder && dir_reorder(chrs[0], pos, nn))
	{
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
	ren_laststr = s;
	ren_lastpos = pos;
	ren_lastchrs = chrs[0];
	ren_lastn = *n;
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
	pos = ren_position(s, &c, &n);
	p = pos_prev(pos, n, p, 1);
	if (*uc_chr(s, ren_off(s, p)) == '\n')
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
	int c; uc_code(c, s)
	for (int i = 0; i < placeholderslen; i++)
		if (placeholders[i].cp == c)
			return placeholders[i].d;
	if (uc_iscomb(s, c)) {
		static char buf[16];
		char cbuf[8] = ""; uc_len(c, s)
		memcpy(cbuf, s, c);
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
	int c; uc_code(c, s)
	for (int i = 0; i < placeholderslen; i++)
		if (placeholders[i].cp == c)
			return placeholders[i].wid;
	return (c = uc_wid(c)) ? c : 1;
}

char *ren_translate(char *s, char *ln)
{
	char *p = ren_placeholder(s);
	return p || !xshape ? p : uc_shape(ln, s);
}

#define NFTS		20
/* mapping filetypes to regular expression sets */
static struct ftmap {
	char ft[32];
	int setbidx;
	int seteidx;
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

static int syn_find(char *ft)
{
	for (int i = 0; i < ftmidx; i++)
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

void syn_setft(char *ft)
{
	for (int i = 1; i < 4; i++)
		syn_addhl(NULL, i, 0);
	if ((ftidx = syn_find(ft)) < 0)
		ftidx = 0;
}

void syn_scdir(int scdir)
{
	if (last_scdir != scdir) {
		last_scdir = scdir;
		syn_blockhl = 0;
	}
}

void syn_highlight(int *att, char *s, int n)
{
	rset *rs = ftmap[ftidx].rs;
	int subs[16 * 2];
	int blk = 0, blkm = 0, sidx = 0, flg = 0, hl, j, i;
	int bend = 0, cend = 0;
	while ((hl = rset_find(rs, s + sidx, LEN(subs) / 2, subs, flg)) >= 0)
	{
		hl += ftmap[ftidx].setbidx;
		int *catt = hls[hl].att;
		int blkend = hls[hl].blkend;
		if (blkend && sidx >= bend) {
			for (i = 0; i < LEN(subs) / 2; i++)
				if (subs[i * 2] >= 0)
					blk = i;
			blkm += blkm > hls[hl].blkend ? -1 : 1;
			if (blkm == 1 && last_scdir > 0)
				blkend = 1;
			if (syn_blockhl == hl && blk == blkend)
				syn_blockhl = 0;
			else if (!syn_blockhl && blk != blkend) {
				syn_blockhl = hl;
				blockatt = catt;
				blockcont = hls[hl].end[blk];
			} else
				blk = 0;
		}
		for (i = 0; i < LEN(subs) / 2; i++) {
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

static void syn_initft(int fti, int *n, char *name)
{
	int i = *n;
	char *pats[hlslen];
	for (; i < hlslen && !strcmp(hls[i].ft, name); i++)
		pats[i - *n] = hls[i].pat;
	ftmap[fti].setbidx = *n;
	strcpy(ftmap[fti].ft, name);
	ftmap[fti].rs = rset_make(i - *n, pats, 0);
	*n += i - *n;
	ftmap[fti].seteidx = *n;
}

char *syn_filetype(char *path)
{
	int hl = rset_find(syn_ftrs, path, 0, NULL, 0);
	return hl >= 0 && hl < ftslen ? fts[hl].ft : "/";
}

void syn_reloadft()
{
	if (syn_reload) {
		int hlset = ftmap[ftidx].setbidx;
		rset_free(ftmap[ftidx].rs);
		syn_initft(ftidx, &hlset, ftmap[ftidx].ft);
		syn_reload = 0;
	}
}

int syn_addhl(char *reg, int func, int reload)
{
	for (int i = ftmap[ftidx].setbidx; i < ftmap[ftidx].seteidx; i++)
		if (hls[i].func == func) {
			hls[i].pat = reg;
			syn_reload = reload;
			return i;
		}
	return -1;
}

void syn_init(void)
{
	char *pats[ftslen];
	int i;
	for (i = 0; i < hlslen; ftmidx++)
		syn_initft(ftmidx, &i, hls[i].ft);
	for (i = 0; i < ftslen; i++)
		pats[i] = fts[i].pat;
	syn_ftrs = rset_make(i, pats, 0);
}

void syn_done(void)
{
	for (; ftmidx >= 0; ftmidx--)
		rset_free(ftmap[ftmidx].rs);
	rset_free(syn_ftrs);
}
