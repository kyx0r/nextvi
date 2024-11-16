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
	struct lbuf *lb = emalloc(sizeof(*lb));
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

static void lbuf_savemark(struct lbuf *lb, struct lopt *lo, int m1, int m2)
{
	lo->mark[m1] = lb->mark[m2];
	lo->mark_off[m1] = lb->mark_off[m2];
}

static void lbuf_loadmark(struct lbuf *lb, struct lopt *lo, int m1, int m2)
{
	if (lo->mark[m2] >= 0) {
		lb->mark[m1] = lo->mark[m2];
		lb->mark_off[m1] = lo->mark_off[m2];
	}
}

static int markidx(int mark)
{
	if (mark == '\'' || mark == '`')
		return 'z' - 'a' + 1;
	if (mark == '*')
		return 'z' - 'a' + 2;
	if (mark == '[')
		return 'z' - 'a' + 3;
	if (mark == ']')
		return 'z' - 'a' + 4;
	if (islower(mark))
		return mark - 'a';
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
static void lbuf_replace(struct lbuf *lb, char *s, struct lopt *lo, int n_del, int n_ins)
{
	int i, pos = lo->pos;
	rstate->s = NULL; /* there is no guarantee malloc not giving same ptr back */
	while (lb->ln_n + n_ins - n_del >= lb->ln_sz) {
		int nsz = lb->ln_sz + (lb->ln_sz ? lb->ln_sz : 512);
		char **nln = emalloc(nsz * sizeof(nln[0]));
		char *nln_glob = emalloc(nsz * sizeof(nln_glob[0]));
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
		char *n = emalloc(l_nonl + 7 + sizeof(int));
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
	for (i = 0; i < NMARKS_BASE; i++) {	/* updating marks */
		if (!s && lb->mark[i] >= pos && lb->mark[i] < pos + n_del) {
			lbuf_savemark(lb, lo, i, i);
			lb->mark[i] = -1;
			continue;
		} else if (lb->mark[i] >= pos + n_del)
			lb->mark[i] += n_ins - n_del;
		else if (n_ins && lb->mark[i] >= pos + n_ins)
			lb->mark[i] = pos + n_ins - 1;
		else
			lbuf_loadmark(lb, lo, i, i);
	}
}

void lbuf_emark(struct lbuf *lb, int hist_n, int beg, int end)
{
	struct lopt *lo = &lb->hist[hist_n];
	lbuf_savemark(lb, lo, markidx(']'), markidx(']'));
	lbuf_mark(lb, ']', end, lo->pos_off);
	if (beg >= 0) {
		lbuf_savemark(lb, lo, markidx('['), markidx('['));
		lbuf_mark(lb, '[', beg, lo->pos_off);
	}
}

/* append undo/redo history; return lopt idx */
int lbuf_opt(struct lbuf *lb, char *buf, int pos, int n_del)
{
	struct lopt *lo;
	int i;
	for (i = lb->hist_u; i < lb->hist_n; i++)
		lopt_done(&lb->hist[i]);
	lb->hist_n = lb->hist_u;
	if (lb->hist_n == lb->hist_sz) {
		int sz = lb->hist_sz + (lb->hist_sz ? lb->hist_sz : 128);
		struct lopt *hist = emalloc(sz * sizeof(hist[0]));
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
	lo->mark = emalloc(sizeof(lb->mark));
	lo->mark_off = emalloc(sizeof(lb->mark_off));
	memset(lo->mark, 0xff, sizeof(lb->mark));
	return lb->hist_n - 1;
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
		long nl = lbuf_slen(ln) + 1;
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
	int i = lbuf_opt(lb, buf, beg, end - beg);
	struct lopt *lo = &lb->hist[i];
	lbuf_replace(lb, buf, lo, lo->n_del, lo->n_ins);
	lbuf_emark(lb, i, lb->hist_u < 2 ||
			lb->hist[lb->hist_u - 2].seq != lb->useq ? beg : -1,
			beg + (lo->n_ins ? lo->n_ins - 1 : 0));
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
	if (!lb->hist_u)
		return 1;
	struct lopt *lo = &lb->hist[lb->hist_u - 1];
	int useq = lo->seq;
	lbuf_savemark(lb, lo, markidx('*'), markidx('['));
	while (lb->hist_u && lb->hist[lb->hist_u - 1].seq == useq) {
		lo = &lb->hist[--(lb->hist_u)];
		lbuf_replace(lb, lo->del, lo, lo->n_ins, lbuf_linecount(lo->del));
	}
	lbuf_loadpos(lb, lo);
	lbuf_savemark(lb, lo, markidx('`'), markidx(']'));
	lbuf_loadmark(lb, lo, markidx('['), markidx('['));
	lbuf_loadmark(lb, lo, markidx(']'), markidx(']'));
	return 0;
}

int lbuf_redo(struct lbuf *lb)
{
	if (lb->hist_u == lb->hist_n)
		return 1;
	struct lopt *lo = &lb->hist[lb->hist_u];
	int useq = lo->seq;
	lbuf_loadmark(lb, lo, markidx(']'), markidx('`'));
	while (lb->hist_u < lb->hist_n && lb->hist[lb->hist_u].seq == useq) {
		lo = &lb->hist[lb->hist_u++];
		lbuf_replace(lb, lo->ins, lo, lo->n_del, lbuf_linecount(lo->ins));
	}
	lbuf_loadpos(lb, lo);
	lbuf_loadmark(lb, lo, markidx('['), markidx('*'));
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
		ln += uc_len(ln);
	return ln[o] ? o : o - 2;
}

static int uc_nextdir(char **s, char *beg, int dir)
{
	if (dir < 0) {
		if (*s == beg)
			return 1;
		*s = uc_beg(beg, (*s) - 1);
	} else {
		*s += uc_len(s[0]);
		if (!(*s)[0])
			return 1;
	}
	return 0;
}

int lbuf_findchar(struct lbuf *lb, char *cs, int cmd, int n, int *row, int *off)
{
	char *ln = lbuf_get(lb, *row);
	char *s;
	int c1, c2, l, dir = (cmd == 'f' || cmd == 't') ? +1 : -1;
	if (!ln)
		return 1;
	if (n < 0)
		dir = -dir;
	if (n < 0)
		n = -n;
	s = uc_chr(ln, *off);
	while (n > 0 && !uc_nextdir(&s, ln, dir)) {
		uc_code(c1, s, l)
		uc_code(c2, cs, l)
		if (c1 == c2)
			n--;
	}
	if (!n && (cmd == 't' || cmd == 'T'))
		uc_nextdir(&s, ln, -dir);
	if (!n)
		*off = uc_off(ln, s - ln);
	return n != 0;
}

int lbuf_search(struct lbuf *lb, rset *re, int dir, int *r,
			int *o, int ln_n, int skip)
{
	int r0 = *r, o0 = *o;
	int offs[re->grpcnt * 2], i = r0;
	char *s = lbuf_get(lb, i);
	int off = skip >= 0 && *uc_chr(s, o0 + skip) ? uc_chr(s, o0 + skip) - s : 0;
	for (; i >= 0 && i < ln_n; i += dir) {
		s = lb->ln[i];
		while (rset_find(re, s + off, offs,
				off ? REG_NOTBOL | REG_NEWLINE : REG_NEWLINE) >= 0) {
			int g1 = offs[xgrp], g2 = offs[xgrp + 1];
			if (g1 < 0) {
				off += offs[1] > 0 ? offs[1] : 1;
				continue;
			}
			if (dir < 0 && r0 == i && uc_off(s, off+g1) >= o0)
				break;
			*o = uc_off(s, off + g1);
			*r = i;
			off += g2 > 0 ? g2 : 1;
			if (dir > 0)
				return 0;
			ln_n = -1; /* break outer loop efficiently */
		}
		off = 0;
	}
	return ln_n < 0 ? 0 : 1;
}

int lbuf_sectionbeg(struct lbuf *lb, int dir, int *row, int *off, int ch)
{
	if (ch == '\n')
		while (*row >= 0 && *row < lbuf_len(lb) && *lbuf_get(lb, *row) == ch)
			*row += dir;
	else
		*row += dir;
	while (*row >= 0 && *row < lbuf_len(lb) && *lbuf_get(lb, *row) != ch)
		*row += dir;
	*row = MAX(0, MIN(*row, lbuf_len(lb) - 1));
	*off = 0;
	return 0;
}

int lbuf_eol(struct lbuf *lb, int row)
{
	int len = 0;
	if (lbuf_get(lb, row))
		len = ren_position(lbuf_get(lb, row))->n;
	return len ? len - 1 : len;
}

static int lbuf_next(struct lbuf *lb, int dir, int *r, int *o)
{
	int odir = dir > 0 ? 1 : -1;
	int len, off = *o + odir;
	if (odir < 0 && *r >= lbuf_len(lb))
		*r = MAX(0, lbuf_len(lb) - 1);
	if (lbuf_get(lb, *r))
		len = ren_position(lbuf_get(lb, *r))->n;
	else
		return -1;
	if (off < 0 || off >= len) {
		if (dir % 2 == 0 || !lbuf_get(lb, *r + odir))
			return -1;
		*r += odir;
		if (odir > 0) {
			ren_position(lbuf_get(lb, *r));
			*o = 0;
		} else
			*o = lbuf_eol(lb, *r);
	} else
		*o = off;
	return 0;
}

/* move to the last character of the word */
static int lbuf_wordlast(struct lbuf *lb, int kind, int dir, int *row, int *off)
{
	if (!kind || !(uc_kind(rstate->chrs[*off]) & kind))
		return 0;
	while (uc_kind(rstate->chrs[*off]) & kind)
		if (lbuf_next(lb, dir, row, off))
			return 1;
	if (!(uc_kind(rstate->chrs[*off]) & kind))
		lbuf_next(lb, -dir, row, off);
	return 0;
}

int lbuf_wordbeg(struct lbuf *lb, int big, int dir, int *row, int *off)
{
	int nl;
	if (!lbuf_get(lb, *row))
		return 1;
	ren_state *r = ren_position(lbuf_get(lb, *row));
	lbuf_wordlast(lb, big ? 3 : uc_kind(r->chrs[*off]), dir, row, off);
	nl = *rstate->chrs[*off] == '\n';
	if (lbuf_next(lb, dir, row, off))
		return 1;
	while (uc_isspace(rstate->chrs[*off])) {
		nl += *rstate->chrs[*off] == '\n';
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
	if (!lbuf_get(lb, *row))
		return 1;
	ren_state *r = ren_position(lbuf_get(lb, *row));
	if (!uc_isspace(r->chrs[*off])) {
		if (lbuf_next(lb, dir, row, off))
			return 1;
		nl = dir < 0 && *rstate->chrs[*off] == '\n';
	}
	nl += dir > 0 && *rstate->chrs[*off] == '\n';
	while (uc_isspace(rstate->chrs[*off])) {
		if (lbuf_next(lb, dir, row, off))
			return 1;
		nl += *rstate->chrs[*off] == '\n';
		if (nl == 2) {
			if (dir < 0)
				lbuf_next(lb, -dir, row, off);
			return 0;
		}
	}
	lbuf_wordlast(lb, big ? 3 : uc_kind(rstate->chrs[*off]), dir, row, off);
	return 0;
}

/* move to the matching character */
int lbuf_pair(struct lbuf *lb, int *row, int *off)
{
	int r = *row, o = *off;
	char *pairs = "()[]{}";
	int p, c, dep = 1;
	if (!lbuf_get(lb, r))
		return 1;
	ren_state *rs = ren_position(lbuf_get(lb, r));
	for (; !strchr(pairs, *rs->chrs[o]); o++);
	if (!strchr(pairs, *rs->chrs[o]))
		return 1;
	p = strchr(pairs, *rs->chrs[o]) - pairs;
	while (!lbuf_next(lb, (p & 1) ? -1 : +1, &r, &o)) {
		c = *rstate->chrs[o];
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
