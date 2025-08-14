struct lbuf *lbuf_make(void)
{
	struct lbuf *lb = emalloc(sizeof(*lb));
	memset(lb, 0, sizeof(*lb));
	for (int i = 0; i < LEN(lb->mark); i++)
		lb->mark[i] = -1;
	return lb;
}

static void lopt_done(struct lopt *lo)
{
	free(lo->mark);
	free(lo->del);
	free(lo->ins);
}

#define _lbuf_movemark(dst, at0, off0, src, at1, off1) \
{ dst[at0] = src[at1]; dst[at0 + off0] = src[at1 + off1]; } \

#define lbuf_movemark(dst, at0, src, at1) _lbuf_movemark(dst, at0, NMARKS, src, at1, NMARKS)

#define lbuf_loadmark(dst, at0, src0, src1) \
{ dst[at0] = src0; dst[at0 + NMARKS] = src1; } \

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

void lbuf_mark(struct lbuf *lb, int mark, int pos, int off)
{
	int mk = markidx(mark);
	if (mk >= 0)
		lbuf_loadmark(lb->mark, mk, pos, off)
}

int lbuf_jump(struct lbuf *lb, int mark, int *pos, int *off)
{
	int mk = markidx(mark);
	if (mk < 0 || lb->mark[mk] < 0)
		return 1;
	*pos = lb->mark[mk];
	*off = lb->mark[mk + NMARKS];
	return 0;
}

void lbuf_free(struct lbuf *lb)
{
	int i;
	for (i = 0; i < lb->ln_n; i++)
		free(lbuf_i(lb, i));
	for (i = 0; i < lb->hist_n; i++)
		lopt_done(&lb->hist[i]);
	free(lb->hist);
	free(lb->ln);
	free(lb);
}

static int linelength(char *s)
{
	int len = dstrlen(s, '\n');
	return s[len] == '\n' ? len + 1 : len;
}

/* low-level line replacement */
static int lbuf_replace(struct lbuf *lb, char *s, struct lopt *lo, int n_del)
{
	int i, n_ins = 0, pos = lo->pos;
	sbuf_smake(sb, 0)
	if (s) {
		for (; *s; n_ins++) {
			int l = linelength(s);
			int l_nonl = l - (s[l - !!l] == '\n');
			struct linfo *n = emalloc(l_nonl + 7 + sizeof(struct linfo));
			n->len = l_nonl;
			n->grec = 0;
			char *ln = (char*)(n + 1);
			memcpy(ln, s, l_nonl);
			memset(&ln[l_nonl + 1], 0, 5);	/* fault tolerance pad */
			ln[l_nonl] = '\n';
			sbuf_mem(sb, &ln, (int)sizeof(s))
			s += l;
		}
	}
	for (i = 0; i < n_del; i++) {
		if (lb->ln[pos + i] == rstate->s)
			rstate->s = NULL;	/* invalidate deleted cached rstate */
		free(lbuf_i(lb, pos + i));
	}
	if (lb->ln_n + n_ins - n_del >= lb->ln_sz) {
		int nsz = lb->ln_n + n_ins - n_del + 512;
		char **nln = emalloc(nsz * sizeof(lb->ln[0]));
		memcpy(nln, lb->ln, lb->ln_n * sizeof(lb->ln[0]));
		free(lb->ln);
		lb->ln = nln;
		lb->ln_sz = nsz;
	}
	if (n_ins != n_del) {
		memmove(lb->ln + pos + n_ins, lb->ln + pos + n_del,
			(lb->ln_n - pos - n_del) * sizeof(lb->ln[0]));
	}
	lb->ln_n += n_ins - n_del;
	for (i = 0; i < n_ins; i++)
		lb->ln[pos + i] = *((char**)sb->s + i);
	for (i = 0; i < NMARKS_BASE; i++) {	/* updating marks */
		if (!s && lb->mark[i] >= pos && lb->mark[i] < pos + n_del) {
			lbuf_movemark(lo->mark, i, lb->mark, i)
			lb->mark[i] = -1;
			continue;
		} else if (lb->mark[i] >= pos + n_del)
			lb->mark[i] += n_ins - n_del;
		else if (n_ins && lb->mark[i] >= pos + n_ins)
			lb->mark[i] = pos + n_ins - 1;
		else if (lo->mark[i] >= 0)
			lbuf_movemark(lb->mark, i, lo->mark, i)
	}
	free(sb->s);
	return n_ins;
}

void lbuf_emark(struct lbuf *lb, struct lopt *lo, int beg, int end)
{
	int mk = markidx(']');
	lbuf_movemark(lo->mark, mk, lb->mark, mk)
	lbuf_loadmark(lb->mark, mk, end, lo->pos_off)
	if (beg >= 0) {
		mk = markidx('[');
		lbuf_movemark(lo->mark, mk, lb->mark, mk)
		lbuf_loadmark(lb->mark, mk, beg, lo->pos_off)
	}
}

static int smark[NMARKS * 2] = {-1};
static struct lopt slo = {.mark = smark};

/* append undo/redo history */
struct lopt *lbuf_opt(struct lbuf *lb, char *buf, int pos, int n_del, int init)
{
	struct lopt *lo;
	for (int i = lb->hist_u; i < lb->hist_n; i++)
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
	if (xseq < 0)
		lo = &slo;
	else {
		lo = &lb->hist[lb->hist_n++];
		lb->hist_u = lb->hist_n;
		lo->ins = !init && buf ? uc_dup(buf) : NULL;
		lo->del = n_del ? lbuf_cp(lb, pos, pos + n_del) : NULL;
		lo->mark = emalloc(sizeof(lb->mark));
		memset(lo->mark, -1, sizeof(lb->mark) / 2);
		lo->seq = lb->useq;
	}
	lo->pos = pos;
	lo->n_ins = 0;
	lo->n_del = n_del;
	lo->pos_off = lb->mark[markidx('*')] >= 0 ?
			lb->mark[markidx('*') + NMARKS] : 0;
	return lo;
}

int lbuf_rd(struct lbuf *lb, int fd, int beg, int end, int init)
{
	long nr;
	sbuf_smake(sb, 1048575)  /* caps at 2147481600 on 32 bit */
	while ((nr = read(fd, sb->s + sb->s_n, sb->s_sz - sb->s_n)) > 0) {
		sb->s_n += nr;
		if (sb->s_n >= sb->s_sz) {
			if (sb->s_n > INT_MAX / 2)
				break;
			sbuf_extend(sb, sb->s_n * 2)
		}
	}
	sbuf_null(sb)
	lbuf_iedit(lb, sb->s, beg, end, init);
	free(sb->s);
	return nr != 0;
}

int lbuf_wr(struct lbuf *lb, int fd, int beg, int end)
{
	for (int i = beg; i < end; i++) {
		char *ln = lb->ln[i];
		long nw = 0;
		long nl = lbuf_s(ln)->len + 1;
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
void lbuf_iedit(struct lbuf *lb, char *buf, int beg, int end, int init)
{
	if (beg > lb->ln_n)
		beg = lb->ln_n;
	if (end > lb->ln_n)
		end = lb->ln_n;
	if (beg == end && !buf)
		return;
	struct lopt *lo = lbuf_opt(lb, buf, beg, end - beg, init);
	lo->n_ins = lbuf_replace(lb, buf, lo, lo->n_del);
	lbuf_emark(lb, lo, lb->hist_u < 2 ||
			lb->hist[lb->hist_u - 2].seq != lb->useq ? beg : -1,
			beg + (lo->n_ins ? lo->n_ins - 1 : 0));
	lb->modified = 1;
}

char *lbuf_cp(struct lbuf *lb, int beg, int end)
{
	sbuf_smake(sb, 64)
	for (int i = beg; i < end; i++)
		if (i < lb->ln_n)
			sbuf_str(sb, lb->ln[i])
	sbufn_sret(sb)
}

char *lbuf_get(struct lbuf *lb, int pos)
{
	return pos >= 0 && pos < lb->ln_n ? lb->ln[pos] : NULL;
}

int lbuf_undo(struct lbuf *lb)
{
	if (!lb->hist_u)
		return 1;
	struct lopt *lo = &lb->hist[lb->hist_u - 1];
	const int useq = lo->seq;
	const int m0 = markidx(']'), m1 = markidx('[');
	if (lb->hist_u == lb->hist_n) {
		_lbuf_movemark(lb->tmp_mark, 0, 2, lb->mark, m0, NMARKS)
		_lbuf_movemark(lb->tmp_mark, 1, 2, lb->mark, m1, NMARKS)
	}
	while (lb->hist_u && lb->hist[lb->hist_u - 1].seq == useq) {
		lo = &lb->hist[--lb->hist_u];
		lbuf_replace(lb, lo->del, lo, lo->n_ins);
	}
	lbuf_loadmark(lb->mark, markidx('*'), lo->pos, lo->pos_off)
	lbuf_movemark(lb->mark, m0, lo->mark, m0)
	lbuf_movemark(lb->mark, m1, lo->mark, m1)
	lb->modified = lb->hist_u != 0;
	return 0;
}

int lbuf_redo(struct lbuf *lb)
{
	if (lb->hist_u == lb->hist_n)
		return 1;
	struct lopt *lo = &lb->hist[lb->hist_u];
	const int useq = lo->seq;
	const int m0 = markidx(']'), m1 = markidx('[');
	while (lb->hist_u < lb->hist_n && lb->hist[lb->hist_u].seq == useq) {
		lo = &lb->hist[lb->hist_u++];
		lbuf_replace(lb, lo->ins, lo, lo->n_del);
	}
	lbuf_loadmark(lb->mark, markidx('*'), lo->pos, lo->pos_off)
	if (lb->hist_u < lb->hist_n) {
		lo++;
		lbuf_movemark(lb->mark, m0, lo->mark, m0)
		lbuf_movemark(lb->mark, m1, lo->mark, m1)
	} else {
		_lbuf_movemark(lb->mark, m0, NMARKS, lb->tmp_mark, 0, 2)
		_lbuf_movemark(lb->mark, m1, NMARKS, lb->tmp_mark, 1, 2)
	}
	lb->modified = 1;
	return 0;
}

/* mark buffer as saved and, if clear, clear the undo history */
void lbuf_saved(struct lbuf *lb, int clear)
{
	if (clear) {
		for (int i = 0; i < lb->hist_n; i++)
			lopt_done(&lb->hist[i]);
		lb->hist_n = 0;
		lb->hist_u = 0;
	}
	lb->modified = 0;
}

int lbuf_indents(struct lbuf *lb, int r)
{
	char *ln = lbuf_get(lb, r);
	int o;
	if (!ln)
		return 0;
	for (o = 0; uc_isspace(ln); o++)
		ln += uc_len(ln);
	return *ln ? o : o - 2;
}

int lbuf_findchar(struct lbuf *lb, char *cs, int cmd, int n, int *row, int *off)
{
	char *ln = lbuf_get(lb, *row);
	int c1, c2, l, dir = (cmd == 'f' || cmd == 't') ? +1 : -1;
	if (!ln)
		return 1;
	ren_state *r = ren_position(ln);
	uc_code(c2, cs, l)
	*off += dir + (cmd == 't') - (cmd == 'T');
	while (n > 0 && *off >= 0 && *off < r->n) {
		uc_code(c1, r->chrs[*off], l)
		if (c1 == c2)
			n--;
		if (n > 0)
			*off += dir;
	}
	if (!n && (cmd == 't' || cmd == 'T'))
		*off = MIN(MAX(0, *off - dir), r->n-1);
	return n != 0;
}

int lbuf_search(struct lbuf *lb, rset *re, int dir, int *r,
			int *o, int ln_n, int skip)
{
	int r0 = *r, o0 = *o;
	int offs[re->grpcnt * 2], i = r0;
	char *s = lbuf_get(lb, i);
	int off = skip >= 0 && s ? uc_chr(s, o0 + skip) - s : 0;
	int g1, g2, _o, step;
	for (; i >= 0 && i < ln_n; i += dir) {
		_o = 0;
		step = 0;
		s = lb->ln[i];
		while (rset_find(re, s + off, offs,
				off ? REG_NOTBOL | REG_NEWLINE : REG_NEWLINE) >= 0) {
			g1 = offs[xgrp], g2 = offs[xgrp + 1];
			if (g1 < 0) {
				off += offs[1] > 0 ? offs[1] : 1;
				continue;
			}
			_o += uc_off(s + step, off + g1 - step);
			if (dir < 0 && r0 == i && _o >= o0)
				break;
			*o = _o;
			*r = i;
			if (dir > 0)
				return 0;
			step = off + g1;
			off += g2 > 0 ? g2 : 1;
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
	for (; o < rs->n-1 && !memchr(pairs, *rs->chrs[o], 6); o++);
	if (!memchr(pairs, *rs->chrs[o], 6))
		return 1;
	p = (char*)memchr(pairs, *rs->chrs[o], 6) - pairs;
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
