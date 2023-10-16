#define NMARKS_BASE		('z' - 'a' + 2)
#define NMARKS			32

/* line operations */
struct lopt {
	char *ins;		/* inserted text */
	char *del;		/* deleted text */
	int pos, n_ins, n_del;	/* modification location */
	int pos_off;		/* cursor line offset */
	int seq;		/* operation number */
	int *mark, *mark_off;	/* saved marks */
};

/* line buffers */
struct lbuf {
	char **ln;		/* buffer lines */
	char *ln_glob;		/* line global mark */
	struct lopt *hist;	/* buffer history */
	int mark[NMARKS];	/* mark lines */
	int mark_off[NMARKS];	/* mark line offsets */
	int ln_n;		/* number of lines in ln[] */
	int ln_sz;		/* size of ln[] */
	int useq;		/* current operation sequence */
	int hist_sz;		/* size of hist[] */
	int hist_n;		/* current history head in hist[] */
	int hist_u;		/* current undo head in hist[] */
	int useq_zero;		/* useq for lbuf_saved() */
	int useq_last;		/* useq before hist[] */
};

struct lbuf *lbuf_make(void)
{
	struct lbuf *lb = malloc(sizeof(*lb));
	int i;
	memset(lb, 0, sizeof(*lb));
	for (i = 0; i < LEN(lb->mark); i++)
		lb->mark[i] = -1;
	lb->useq = 1;
	return lb;
}

static void lopt_done(struct lopt *lo)
{
	free(lo->ins);
	free(lo->del);
	free(lo->mark);
	free(lo->mark_off);
}

static void lbuf_savemark(struct lbuf *lb, struct lopt *lo, int m)
{
	if (lb->mark[m] >= 0) {
		lo->mark = malloc(sizeof(lb->mark));
		lo->mark_off = malloc(sizeof(lb->mark_off));
		memset(lo->mark, 0xff, sizeof(lb->mark));
		lo->mark[m] = lb->mark[m];
		lo->mark_off[m] = lb->mark_off[m];
	}
}

static void lbuf_loadmark(struct lbuf *lb, struct lopt *lo, int m)
{
	if (lo->mark && lo->mark[m] >= 0) {
		lb->mark[m] = lo->mark[m];
		lb->mark_off[m] = lo->mark_off[m];
	}
}

static int markidx(int mark)
{
	if (islower(mark))
		return mark - 'a';
	if (mark == '\'' || mark == '`')
		return 'z' - 'a' + 1;
	if (mark == '*')
		return 'z' - 'a' + 2;
	if (mark == '[')
		return 'z' - 'a' + 3;
	if (mark == ']')
		return 'z' - 'a' + 4;
	return -1;
}

static void lbuf_loadpos(struct lbuf *lb, struct lopt *lo)
{
	lb->mark[markidx('*')] = lo->pos;
	lb->mark_off[markidx('*')] = lo->pos_off;
}

void lbuf_free(struct lbuf *lb)
{
	int i;
	for (i = 0; i < lb->ln_n; i++)
		free(lb->ln[i] - sizeof(int));
	for (i = 0; i < lb->hist_n; i++)
		lopt_done(&lb->hist[i]);
	free(lb->hist);
	free(lb->ln);
	free(lb->ln_glob);
	free(lb);
}

static int linelength(char *s)
{
	int len = dstrlen(s, '\n');
	return s[len] == '\n' ? len + 1 : len;
}

static int lbuf_linecount(char *s)
{
	if (!s)
		return 0;
	int n;
	for (n = 0; *s; n++)
		s += linelength(s);
	return n;
}

/* low-level line replacement */
static void lbuf_replace(struct lbuf *lb, char *s, int pos, int n_del, int n_ins)
{
	int i;
	rstate->ren_laststr = NULL; /* there is no guarantee malloc not giving same ptr back */
	while (lb->ln_n + n_ins - n_del >= lb->ln_sz) {
		int nsz = lb->ln_sz + (lb->ln_sz ? lb->ln_sz : 512);
		char **nln = malloc(nsz * sizeof(nln[0]));
		char *nln_glob = malloc(nsz * sizeof(nln_glob[0]));
		memcpy(nln, lb->ln, lb->ln_n * sizeof(lb->ln[0]));
		memcpy(nln_glob, lb->ln_glob, lb->ln_n * sizeof(lb->ln_glob[0]));
		free(lb->ln);
		free(lb->ln_glob);
		lb->ln = nln;
		lb->ln_glob = nln_glob;
		lb->ln_sz = nsz;
	}
	for (i = 0; i < n_del; i++)
		free(lb->ln[pos + i] - sizeof(int));
	if (n_ins != n_del) {
		memmove(lb->ln + pos + n_ins, lb->ln + pos + n_del,
			(lb->ln_n - pos - n_del) * sizeof(lb->ln[0]));
		memmove(lb->ln_glob + pos + n_ins, lb->ln_glob + pos + n_del,
			(lb->ln_n - pos - n_del) * sizeof(lb->ln_glob[0]));
	}
	lb->ln_n += n_ins - n_del;
	for (i = 0; i < n_ins; i++) {
		int l = linelength(s);
		int l_nonl = l - (s[l - !!l] == '\n');
		char *n = malloc(l_nonl + 7 + sizeof(int));
		*(int*)n = l_nonl;		/* store length */
		n += sizeof(int);
		memcpy(n, s, l_nonl);
		memset(&n[l_nonl + 1], 0, 5);	/* fault tolerance pad */
		n[l_nonl] = '\n';
		lb->ln[pos + i] = n;
		s += l;
	}
	for (i = n_del; i < n_ins; i++)
		lb->ln_glob[pos + i] = 0;
	for (i = 0; i < LEN(lb->mark); i++) {	/* updating marks */
		if (!s && lb->mark[i] >= pos && lb->mark[i] < pos + n_del)
			lb->mark[i] = -1;
		else if (lb->mark[i] >= pos + n_del)
			lb->mark[i] += n_ins - n_del;
		else if (lb->mark[i] >= pos + n_ins)
			lb->mark[i] = pos + n_ins - 1;
	}
	if (lb->hist_u < 2 || lb->hist[lb->hist_u - 2].seq != lb->useq)
		lbuf_mark(lb, '[', pos, 0);
	lbuf_mark(lb, ']', pos + (n_ins ? n_ins - 1 : 0), 0);
}

/* append undo/redo history; return linecount */
int lbuf_opt(struct lbuf *lb, char *buf, int pos, int n_del)
{
	struct lopt *lo;
	int i;
	for (i = lb->hist_u; i < lb->hist_n; i++)
		lopt_done(&lb->hist[i]);
	lb->hist_n = lb->hist_u;
	if (lb->hist_n == lb->hist_sz) {
		int sz = lb->hist_sz + (lb->hist_sz ? lb->hist_sz : 128);
		struct lopt *hist = malloc(sz * sizeof(hist[0]));
		memcpy(hist, lb->hist, lb->hist_n * sizeof(hist[0]));
		free(lb->hist);
		lb->hist = hist;
		lb->hist_sz = sz;
	}
	lo = &lb->hist[lb->hist_n++];
	lb->hist_u = lb->hist_n;
	lo->ins = buf ? uc_dup(buf) : NULL;
	lo->del = n_del ? lbuf_cp(lb, pos, pos + n_del) : NULL;
	lo->pos = pos;
	lo->n_ins = lbuf_linecount(buf);
	lo->n_del = n_del;
	lo->pos_off = lb->mark[markidx('*')] >= 0 ? lb->mark_off[markidx('*')] : 0;
	lo->seq = lb->useq;
	lo->mark = NULL;
	lo->mark_off = NULL;
	for (i = 0; i < NMARKS_BASE; i++)
		if (lb->mark[i] >= pos && lb->mark[i] < pos + n_del)
			lbuf_savemark(lb, lo, i);
	return lo->n_ins;
}

int lbuf_rd(struct lbuf *lbuf, int fd, int beg, int end)
{
	long nr;
	sbuf *sb; sbuf_make(sb, 1000000)
	while ((nr = read(fd, sb->s + sb->s_n, sb->s_sz - sb->s_n)) > 0) {
		if (sb->s_n + nr >= sb->s_sz) {
			int newsz = NEXTSZ((unsigned int)sb->s_sz, (unsigned int)sb->s_sz + 1);
			if (newsz < 0)
				break; /* can't read files > ~2GB */
			sb->s_n += nr;
			sbuf_extend(sb, newsz)
		} else
			sb->s_n += nr;
	}
	sbuf_null(sb)
	lbuf_edit(lbuf, sb->s, beg, end);
	sbuf_free(sb)
	return nr != 0;
}

int lbuf_wr(struct lbuf *lbuf, int fd, int beg, int end)
{
	for (int i = beg; i < end; i++) {
		char *ln = lbuf->ln[i];
		long nw = 0;
		long nl = *(int*)(ln - sizeof(int)) + 1;
		while (nw < nl) {
			long nc = write(fd, ln + nw, nl - nw);
			if (nc < 0)
				return 1;
			nw += nc;
		}
	}
	return 0;
}

/* replace lines beg through end with buf */
void lbuf_edit(struct lbuf *lb, char *buf, int beg, int end)
{
	if (beg > lb->ln_n)
		beg = lb->ln_n;
	if (end > lb->ln_n)
		end = lb->ln_n;
	if (beg == end && !buf)
		return;
	int lc = lbuf_opt(lb, buf, beg, end - beg);
	lbuf_replace(lb, buf, beg, end - beg, lc);
}

char *lbuf_cp(struct lbuf *lb, int beg, int end)
{
	sbuf *sb; sbuf_make(sb, 64)
	for (int i = beg; i < end; i++)
		if (i < lb->ln_n)
			sbuf_str(sb, lb->ln[i])
	sbufn_done(sb)
}

char *lbuf_get(struct lbuf *lb, int pos)
{
	return pos >= 0 && pos < lb->ln_n ? lb->ln[pos] : NULL;
}

char **lbuf_buf(struct lbuf *lb)
{
	return lb->ln;
}

int lbuf_len(struct lbuf *lb)
{
	return lb->ln_n;
}

void lbuf_mark(struct lbuf *lbuf, int mark, int pos, int off)
{
	if (markidx(mark) >= 0) {
		lbuf->mark[markidx(mark)] = pos;
		lbuf->mark_off[markidx(mark)] = off;
	}
}

int lbuf_jump(struct lbuf *lbuf, int mark, int *pos, int *off)
{
	int mk = markidx(mark);
	if (mk < 0 || lbuf->mark[mk] < 0)
		return 1;
	*pos = lbuf->mark[mk];
	if (off)
		*off = lbuf->mark_off[mk];
	return 0;
}

int lbuf_undo(struct lbuf *lb)
{
	int useq, i;
	if (!lb->hist_u)
		return 1;
	useq = lb->hist[lb->hist_u - 1].seq;
	while (lb->hist_u && lb->hist[lb->hist_u - 1].seq == useq) {
		struct lopt *lo = &lb->hist[--(lb->hist_u)];
		lbuf_replace(lb, lo->del, lo->pos, lo->n_ins, lbuf_linecount(lo->del));
		lbuf_loadpos(lb, lo);
		for (i = 0; i < LEN(lb->mark); i++)
			lbuf_loadmark(lb, lo, i);
	}
	return 0;
}

int lbuf_redo(struct lbuf *lb)
{
	int useq;
	if (lb->hist_u == lb->hist_n)
		return 1;
	useq = lb->hist[lb->hist_u].seq;
	while (lb->hist_u < lb->hist_n && lb->hist[lb->hist_u].seq == useq) {
		struct lopt *lo = &lb->hist[lb->hist_u++];
		lbuf_replace(lb, lo->ins, lo->pos, lo->n_del, lbuf_linecount(lo->ins));
		lbuf_loadpos(lb, lo);
	}
	return 0;
}

static int lbuf_seq(struct lbuf *lb)
{
	return lb->hist_u ? lb->hist[lb->hist_u - 1].seq : lb->useq_last;
}

/* mark buffer as saved and, if clear, clear the undo history */
void lbuf_saved(struct lbuf *lb, int clear)
{
	int i;
	if (clear) {
		for (i = 0; i < lb->hist_n; i++)
			lopt_done(&lb->hist[i]);
		lb->hist_n = 0;
		lb->hist_u = 0;
		lb->useq_last = lb->useq;
	}
	lb->useq_zero = lbuf_seq(lb);
	lbuf_modified(xb);
}

/* was the file modified since the last reset */
int lbuf_modified(struct lbuf *lb)
{
	lb->useq++;
	return lbuf_seq(lb) != lb->useq_zero;
}

/* mark the line for ex global command */
void lbuf_globset(struct lbuf *lb, int pos, int dep)
{
	lb->ln_glob[pos] |= 1 << dep;
}

/* return and clear ex global command mark */
int lbuf_globget(struct lbuf *lb, int pos, int dep)
{
	int o = lb->ln_glob[pos] & (1 << dep);
	lb->ln_glob[pos] &= ~(1 << dep);
	return o > 0;
}

int lbuf_indents(struct lbuf *lb, int r)
{
	char *ln = lbuf_get(lb, r);
	int o;
	if (!ln)
		return 0;
	for (o = 0; uc_isspace(ln); o++)
		ln = uc_next(ln);
	return o;
}

static int uc_nextdir(char **s, char *beg, int dir)
{
	if (dir < 0) {
		if (*s == beg)
			return 1;
		*s = uc_prev(beg, *s);
	} else {
		*s = uc_next(*s);
		if (!(*s)[0])
			return 1;
	}
	return 0;
}

int lbuf_findchar(struct lbuf *lb, char *cs, int cmd, int n, int *row, int *off)
{
	char *ln = lbuf_get(lb, *row);
	char *s;
	int c1, c2, dir = (cmd == 'f' || cmd == 't') ? +1 : -1;
	if (!ln)
		return 1;
	if (n < 0)
		dir = -dir;
	if (n < 0)
		n = -n;
	s = uc_chr(ln, *off);
	while (n > 0 && !uc_nextdir(&s, ln, dir)) {
		uc_code(c1, s) uc_code(c2, cs)
		if (c1 == c2)
			n--;
	}
	if (!n && (cmd == 't' || cmd == 'T'))
		uc_nextdir(&s, ln, -dir);
	if (!n)
		*off = uc_off(ln, s - ln);
	return n != 0;
}

int lbuf_search(struct lbuf *lb, rset *re, int dir, int *r, int *o, int *len)
{
	int r0 = *r, o0 = *o, grp = xgrp;
	int offs[grp], i = r0;
	char *s = lbuf_get(lb, i);
	int off = dir > 0 && *uc_chr(s, o0 + 1) ? uc_chr(s, o0 + 1) - s : 0;
	int ln_n = lbuf_len(lb);
	for (; i >= 0 && i < ln_n; i += dir) {
		s = lb->ln[i];
		while (rset_find(re, s + off, grp / 2, offs,
				off ? REG_NOTBOL | REG_NEWLINE : REG_NEWLINE) >= 0) {
			int g1 = offs[grp - 2], g2 = offs[grp - 1];
			if (g1 < 0) {
				off += offs[1] > 0 ? offs[1] : 1;
				continue;
			}
			if (dir < 0 && r0 == i && uc_off(s, off+g1) >= o0)
				break;
			*o = uc_off(s, off + g1);
			*r = i;
			*len = uc_off(s + off + g1, g2 - g1);
			off += g2 > 0 ? g2 : 1;
			if (dir > 0)
				return 0;
			ln_n = -1; /* break outer loop efficiently */
		}
		off = 0;
	}
	return ln_n < 0 ? 0 : 1;
}

int lbuf_paragraphbeg(struct lbuf *lb, int dir, int *row, int *off)
{
	while (*row >= 0 && *row < lbuf_len(lb) && *lbuf_get(lb, *row) == '\n')
		*row += dir;
	while (*row >= 0 && *row < lbuf_len(lb) && *lbuf_get(lb, *row) != '\n')
		*row += dir;
	*row = MAX(0, MIN(*row, lbuf_len(lb) - 1));
	*off = 0;
	return 0;
}

int lbuf_sectionbeg(struct lbuf *lb, int dir, int *row, int *off)
{
	*row += dir;
	while (*row >= 0 && *row < lbuf_len(lb) && *lbuf_get(lb, *row) != '{')
		*row += dir;
	*row = MAX(0, MIN(*row, lbuf_len(lb) - 1));
	*off = 0;
	return 0;
}

static int lbuf_lnnext(struct lbuf *lb, int dir, int *r, int *o)
{
	int off = *o + dir;
	if (off < 0 || !lbuf_get(lb, *r) || off >= uc_slen(lbuf_get(lb, *r)))
		return 1;
	*o = off;
	return 0;
}

int lbuf_eol(struct lbuf *lb, int row)
{
	int len = lbuf_get(lb, row) ? uc_slen(lbuf_get(lb, row)) : 0;
	return len ? len - 1 : 0;
}

static int lbuf_next(struct lbuf *lb, int dir, int *r, int *o)
{
	if (dir < 0 && *r >= lbuf_len(lb))
		*r = MAX(0, lbuf_len(lb) - 1);
	if (lbuf_lnnext(lb, dir, r, o)) {
		if (!lbuf_get(lb, *r + dir))
			return -1;
		*r += dir;
		*o = dir > 0 ? 0 : lbuf_eol(lb, *r);
		return 0;
	}
	return 0;
}

/* return a pointer to the character at visual position c of line r */
static char *lbuf_chr(struct lbuf *lb, int r, int c)
{
	char *ln = lbuf_get(lb, r);
	return ln ? uc_chr(ln, c) : "";
}

/* move to the last character of the word */
static int lbuf_wordlast(struct lbuf *lb, int kind, int dir, int *row, int *off)
{
	if (!kind || !(uc_kind(lbuf_chr(lb, *row, *off)) & kind))
		return 0;
	while (uc_kind(lbuf_chr(lb, *row, *off)) & kind)
		if (lbuf_next(lb, dir, row, off))
			return 1;
	if (!(uc_kind(lbuf_chr(lb, *row, *off)) & kind))
		lbuf_next(lb, -dir, row, off);
	return 0;
}

int lbuf_wordbeg(struct lbuf *lb, int big, int dir, int *row, int *off)
{
	int nl;
	lbuf_wordlast(lb, big ? 3 : uc_kind(lbuf_chr(lb, *row, *off)), dir, row, off);
	nl = *lbuf_chr(lb, *row, *off) == '\n';
	if (lbuf_next(lb, dir, row, off))
		return 1;
	while (uc_isspace(lbuf_chr(lb, *row, *off))) {
		nl += *lbuf_chr(lb, *row, *off) == '\n';
		if (nl == 2)
			return 0;
		if (lbuf_next(lb, dir, row, off))
			return 1;
	}
	return 0;
}

int lbuf_wordend(struct lbuf *lb, int big, int dir, int *row, int *off)
{
	int nl = 0;
	if (!uc_isspace(lbuf_chr(lb, *row, *off))) {
		if (lbuf_next(lb, dir, row, off))
			return 1;
		nl = dir < 0 && *lbuf_chr(lb, *row, *off) == '\n';
	}
	nl += dir > 0 && *lbuf_chr(lb, *row, *off) == '\n';
	while (uc_isspace(lbuf_chr(lb, *row, *off))) {
		if (lbuf_next(lb, dir, row, off))
			return 1;
		nl += *lbuf_chr(lb, *row, *off) == '\n';
		if (nl == 2) {
			if (dir < 0)
				lbuf_next(lb, -dir, row, off);
			return 0;
		}
	}
	if (lbuf_wordlast(lb, big ? 3 : uc_kind(lbuf_chr(lb, *row, *off)), dir, row, off))
		return 1;
	return 0;
}

/* move to the matching character */
int lbuf_pair(struct lbuf *lb, int *row, int *off)
{
	int r = *row, o = *off;
	char *pairs = "()[]{}";
	int c;			/* parenthesis character */
	int p;			/* index for pairs[] */
	int dep = 1;		/* parenthesis depth */
	while ((c = (unsigned char) lbuf_chr(lb, r, o)[0]) && !strchr(pairs, c))
		o++;
	if (!c)
		return 1;
	p = strchr(pairs, c) - pairs;
	while (!lbuf_next(lb, (p & 1) ? -1 : +1, &r, &o)) {
		int c = (unsigned char) lbuf_chr(lb, r, o)[0];
		if (c == pairs[p ^ 1])
			dep--;
		if (c == pairs[p])
			dep++;
		if (!dep) {
			*row = r;
			*off = o;
			return 0;
		}
	}
	return 1;
}
