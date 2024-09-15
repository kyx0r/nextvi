/* line editing and drawing */

static sbuf *suggestsb;
static sbuf *acsb;

int dstrlen(const char *s, char delim)
{
	register const char *i;
	for (i=s; *i && *i != delim; ++i);
	return i-s;
}

static int search(const char *pattern, int l)
{
	if (!*pattern)
		return 0;
	sbuf_cut(suggestsb, 0)
	sbuf *sylsb;
	sbuf_make(sylsb, 1024)
	char *part = strstr(acsb->s, pattern);
	while (part) {
		char *part1 = part;
		while (*part != '\n')
			part--;
		int len = dstrlen(++part, '\n');
		if (len++ != l) {
			if (part == part1)
				sbuf_mem(suggestsb, part, len)
			else
				sbuf_mem(sylsb, part, len)
		}
		part = strstr(part+len, pattern);
	}
	sbuf_mem(suggestsb, sylsb->s, sylsb->s_n)
	sbuf_free(sylsb)
	sbuf_set(suggestsb, '\0', 4)
	suggestsb->s_n -= 4;
	return suggestsb->s_n;
}

static void file_index(struct lbuf *buf)
{
	char reg[] = "[^\t ;:,`.<>[\\]\\^%$#@*\\!?+\\-|/\\=\\\\{}&\\()'\"]+";
	int len, sidx, grp = xgrp;
	char **ss = lbuf_buf(buf);
	int ln_n = lbuf_len(buf);
	int subs[grp], n;
	sbuf *ibuf;
	rset *rs = rset_make(1, (char*[]){xacreg ? xacreg->s : reg}, xic ? REG_ICASE : 0);
	if (!rs)
		return;
	sbuf_make(ibuf, 1024)
	for (n = 1; n <= acsb->s_n; n++)
		if (acsb->s[n - 1] == '\n')
			sbuf_mem(ibuf, &n, (int)sizeof(n))
	for (int i = 0; i < ln_n; i++) {
		sidx = 0;
		while (rset_find(rs, ss[i]+sidx, grp / 2, subs,
				sidx ? REG_NOTBOL | REG_NEWLINE : REG_NEWLINE) >= 0) {
			/* if target group not found, continue with group 1
			which will always be valid, otherwise there be no match */
			if (subs[grp - 2] < 0) {
				sidx += subs[1] > 0 ? subs[1] : 1;
				continue;
			}
			len = subs[grp - 1] - subs[grp - 2];
			if (len > 1) {
				char *part = ss[i]+sidx+subs[grp - 2];
				int *ip = (int*)(ibuf->s+sizeof(n));
				for (n = len+1; ip < (int*)&ibuf->s[ibuf->s_n]; ip++)
					if (*ip - ip[-1] == n &&
						!memcmp(acsb->s + ip[-1], part, len))
							goto skip;
				sbuf_mem(acsb, part, len)
				sbuf_chr(acsb, '\n')
				sbuf_mem(ibuf, &acsb->s_n, (int)sizeof(n))
			}
			skip:
			sidx += subs[grp - 1] > 0 ? subs[grp - 1] : 1;
		}
	}
	sbuf_null(acsb)
	sbuf_free(ibuf)
	rset_free(rs);
}

static char *kmap_map(int kmap, int c)
{
	static char cs[4];
	char **keymap = conf_kmap(kmap);
	cs[0] = c;
	return keymap[c] ? keymap[c] : cs;
}

static int led_posctx(int dir, int pos, int beg, int end)
{
	return dir >= 0 ? pos - beg : end - pos - 1;
}

/* map cursor horizontal position to terminal column number */
int led_pos(char *s, int pos)
{
	return led_posctx(dir_context(s), pos, xleft, xleft + xcols);
}

static int led_offdir(char **chrs, int *pos, int i)
{
	if (pos[i] + ren_cwid(chrs[i], pos[i]) == pos[i + 1])
		return +1;
	if (pos[i + 1] + ren_cwid(chrs[i + 1], pos[i + 1]) == pos[i])
		return -1;
	return 0;
}

/* highlight text in reverse direction */
static void led_markrev(int n, int cterm, int *off, char **chrs, int *pos, int *att)
{
	int i = 0, j;
	int hl = conf_hlrev();
	int *ratt = malloc((n+1)*sizeof(int));
	memset(ratt, 0, (n+1)*sizeof(int));
	while (i + 1 < n) {
		int dir = led_offdir(chrs, pos, i);
		int beg = i;
		while (i + 1 < n && led_offdir(chrs, pos, i) == dir)
			i++;
		if (dir < 0)
			for (j = beg; j <= i; j++)
				ratt[j] = hl;
		if (i == beg)
			i++;
	}
	for (j = 0, i = 0; i < cterm;) {
		int o = off[i++];
		if (o >= 0) {
			att[j] = syn_merge(ratt[o], att[j]);
			for (j++; off[i] == o; i++);
		}
	}
	free(ratt);
}

static char *led_bounds(int *off, char **chrs, int cterm)
{
	int i = 0;
	sbuf *out;
	sbuf_make(out, cterm*4);
	while (i < cterm) {
		int o = off[i++];
		if (o >= 0) {
			sbuf_mem(out, chrs[o], uc_len(chrs[o]))
			for (; off[i] == o; i++);
		}
	}
	sbufn_done(out)
}

#define print_ch1(out) sbuf_mem(out, chrs[o], l)
#define print_ch2(out) sbuf_mem(out, *chrs[o] == ' ' ? "_" : chrs[o], l)

#define hid_ch1(out) sbuf_set(out, ' ', i - l)
#define hid_ch2(out) \
int pre = out->s_n; \
sbuf_set(out, *chrs[o] == '\n' ? '\\' : '-', i - l) \
if (ctx > 0 && *chrs[o] == '\t') \
	out->s[out->s_n-1] = '>'; \
else if (*chrs[o] == '\t') \
	out->s[pre] = '<'; \

#define led_out(out, n) \
{ \
for (i = 0; i < cterm;) { \
	int att_new = 0; \
	o = off[i]; \
	if (o >= 0) { \
		for (l = i; off[i] == o; i++); \
		att_new = att[atti++]; \
		if (att_new != att_old) \
			sbuf_str(out, term_att(att_new)) \
		char *s = ren_translate(chrs[o], s0); \
		if (s) \
			sbuf_str(out, s) \
		else if (uc_isprint(chrs[o])) { \
			l = uc_len(chrs[o]); \
			print_ch##n(out) \
		} else { \
			hid_ch##n(out) \
		} \
	} else { \
		if (cbeg || ctx < 0) { \
			if (att_new != att_old) \
				sbuf_str(out, term_att(0)) \
			sbuf_chr(out, ' ') \
		} \
		i++; \
	} \
	att_old = att_new; \
} sbufn_str(out, term_att(0)) } \

/* render and highlight a line */
void led_render(char *s0, int cbeg, int cend)
{
	if (!xled)
		return;
	int l, n, i = 0, o = 0, cterm = cend - cbeg;
	int att_old = 0, atti = 0;
	char *bound = s0;
	int *pos;		/* pos[i]: the screen position of the i-th character */
	char **chrs;		/* chrs[i]: the i-th character in s1 */
	int off[cterm+1];	/* off[i]: the character at screen position i */
	int att[cterm+1];	/* att[i]: the attributes of i-th character */
	int ctx = dir_context(s0);
	memset(off, -1, (cterm+1) * sizeof(off[0]));
	memset(att, 0, (cterm+1) * sizeof(att[0]));
	pos = ren_position(s0, &chrs, &n);
	if (ctx < 0) {
		for (; i < n; i++) {
			int curbeg = cend - pos[i] - 1;
			if (curbeg >= 0 && curbeg < cterm) {
				int curwid = ren_cwid(chrs[i], pos[i]);
				if (o + curwid > cterm)
					break;
				if (cend - (pos[i] + curwid - 1) - 1 < 0)
					continue;
				o += curwid;
				while (--curwid >= 0)
					off[cend - (pos[i] + curwid - 1) - 2] = i;
				if (o == cterm)
					break;
			}
		}
	} else {
		for (; i < n; i++) {
			int curbeg = pos[i] - cbeg;
			if (curbeg >= 0 && curbeg < cterm) {
				int curwid = ren_cwid(chrs[i], pos[i]);
				if (o + curwid > cterm)
					break;
				if (curbeg + curwid > cterm)
					continue;
				o += curwid;
				while (--curwid >= 0)
					off[curbeg + curwid] = i;
				if (o == cterm)
					break;
			}
		}
	}
	if (pos[n] > cterm || cbeg || ctx < 0)
		bound = led_bounds(off, chrs, cterm);
	if (xhl)
		syn_highlight(att, bound, n < cterm ? n : cterm);
	if (bound != s0)
		free(bound);
	if (xhlr)
		led_markrev(n, cterm, off, chrs, pos, att);
	/* generate term output */
	if (vi_hidch)
		led_out(term_sbuf, 2)
	else
		led_out(term_sbuf, 1)
}

static int led_lastchar(char *s)
{
	char *r = *s ? strchr(s, '\0') : s;
	if (r != s)
		r = uc_beg(s, r - 1);
	return r - s;
}

static int led_lastword(char *s)
{
	char *r = *s ? uc_beg(s, strchr(s, '\0') - 1) : s;
	int kind;
	while (r > s && uc_isspace(r))
		r = uc_beg(s, r - 1);
	kind = r > s ? uc_kind(r) : 0;
	while (r > s && uc_kind(uc_beg(s, r - 1)) == kind)
		r = uc_beg(s, r - 1);
	return r - s;
}

static void led_printparts(sbuf *sb, int ps, char *post)
{
	if (!xled)
		return;
	int off, pos, psn = sb->s_n;
	int idir = 0, next = sb->s[ps];
	sbuf_str(sb, post)
	sbuf_set(sb, '\0', 4)
	rstate->s = NULL;
	ren_position(sb->s+ps, &(char**){NULL}, &off);
	off -= uc_slen(post);
	pos = ren_cursor(sb->s+ps, ren_pos(sb->s+ps, MAX(0, off - 1)));
	if (pos >= xleft + xcols)
		xleft = pos - xcols / 2;
	if (pos < xleft)
		xleft = pos < xcols ? 0 : pos - xcols / 2;
	syn_blockhl = 0;
	led_crender(sb->s+ps, -1, vi_lncol, xleft, xleft + xcols - vi_lncol);
	/* cursor position for inserting the next character */
	if (next) {
		if (off - 2 >= 0)
			idir = ren_pos(sb->s+ps, off-1) - ren_pos(sb->s+ps, off-2);
		idir = idir < 0 ? -1 : 1;
	}
	term_pos(-1, led_pos(sb->s+ps, pos + idir) + vi_lncol);
	sbufn_cut(sb, psn)
}

/* read a character from the terminal */
char *led_read(int *kmap, int c)
{
	static char buf[32];
	int c1, c2, i, n;
	while (!TK_INT(c)) {
		switch (c) {
		case TK_CTL('f'):
			*kmap = xkmap_alt;
			break;
		case TK_CTL('e'):
			*kmap = 0;
			break;
		case TK_CTL('v'):	/* literal character */
			buf[0] = term_read();
			buf[1] = '\0';
			return buf;
		case TK_CTL('k'):	/* digraph */
			c1 = term_read();
			if (TK_INT(c1))
				return NULL;
			c2 = term_read();
			if (TK_INT(c2))
				return NULL;
			return conf_digraph(c1, c2);
		default:
			if ((c & 0xc0) == 0xc0) {	/* utf-8 character */
				buf[0] = c;
				n = uc_len(buf);
				for (i = 1; i < n; i++)
					buf[i] = term_read();
				buf[n] = '\0';
				return buf;
			}
			return kmap_map(*kmap, c);
		}
		c = term_read();
	}
	return NULL;
}

static void led_info(char *str, int ai_max)
{
	led_recrender(str, xtop+xrows, 0, 0, xcols)
	if (ai_max >= 0)
		term_pos(xrow - xtop, 0);
}

static void led_redraw(char *cs, int r, int orow, int lsh)
{
	for (int nl = 0; r < xrows; r++) {
		if (vi_lncol) {
			term_pos(r, 0);
			term_kill();
		}
		if (r >= orow-xtop && r < xrow-xtop) {
			sbuf *cb; sbuf_make(cb, 128)
			nl = dstrlen(cs, '\n');
			sbuf_mem(cb, cs, nl+!!cs[nl])
			sbuf_set(cb, '\0', 4)
			led_recrender(cb->s, r, vi_lncol, xleft, xleft + xcols - vi_lncol)
			sbuf_free(cb)
			cs += nl+!!cs[nl];
			continue;
		}
		nl = r < xrow-xtop ? r+xtop : (r-(xrow-orow+lsh))+xtop;
		led_crender(lbuf_get(xb, nl) ? lbuf_get(xb, nl) : "~", r,
			vi_lncol, xleft, xleft + xcols - vi_lncol);
	}
	term_pos(xrow - xtop, 0);
}

/* read a line from the terminal */
static void led_line(sbuf *sb, int ps, int pre, char *post, int ai_max,
		int *key, int *kmap, int orow, int lsh)
{
	int len, p_reg = 0;
	int c, lnmode, i = 0, last_sug = 0, sug_pt = -1;
	char *cs, *sug = NULL, *_sug = NULL;
	if (!post)
		post = "";
	while (1) {
		led_printparts(sb, ps, post);
		len = sb->s_n;
		c = term_read();
		switch (c) {
		case TK_CTL('h'):
		case 127:
			if (len - pre)
				sbufn_cut(sb, led_lastchar(sb->s + pre) + pre)
			else
				goto leave;
			break;
		case TK_CTL('u'):
			sbufn_cut(sb, sug_pt > pre && len > sug_pt ? sug_pt : pre)
			break;
		case TK_CTL('w'):
			if (len - pre)
				sbufn_cut(sb, led_lastword(sb->s + pre) + pre)
			else
				term_push("bdwi", 5);
			break;
		case TK_CTL('t'):
			cs = uc_dup(sb->s + ps);
			sbuf_cut(sb, ps)
			sbuf_chr(sb, '\t')
			sbufn_str(sb, cs)
			free(cs);
			break;
		case TK_CTL('d'):
			if (sb->s[ps] == ' ' || sb->s[ps] == '\t') {
				sbuf_cut(sb, ps)
				sbufn_str(sb, sb->s+ps+1)
			}
			break;
		case TK_CTL(']'):
		case TK_CTL('\\'):
			i = 0; 
			retry:
			if (c == TK_CTL(']')) {
				if (!p_reg || p_reg == '9')
					p_reg = '/';
				while (p_reg < '9' && !vi_regget(++p_reg, &lnmode));
			} else {
				c = term_read();
				p_reg = c == TK_CTL('\\') ? 0 : c;
			}
			if ((cs = vi_regget(p_reg, &lnmode))) {
				sbuf_chr(sb, p_reg ? p_reg : '~')
				sbuf_chr(sb, ' ')
				sbufn_str(sb, cs)
				led_info(sb->s + len, ai_max);
				sbufn_cut(sb, len)
			} else if (!i++)
				goto retry;
			continue;
		case TK_CTL('p'):
			if (vi_regget(p_reg, &lnmode))
				sbufn_str(sb, vi_regget(p_reg, &lnmode))
			break;
		case TK_CTL('g'):
			if (!suggestsb) {
				sbuf_make(suggestsb, 1)
				sbuf_make(acsb, 1024)
				sbufn_chr(acsb, '\n')
			}
			file_index(xb);
			break;
		case TK_CTL('y'):
			led_done();
			suggestsb = NULL;
			break;
		case TK_CTL('r'):
			if (!suggestsb || !suggestsb->s_n)
				continue;
			if (!sug)
				sug = suggestsb->s;
			if (suggestsb->s_n == sug - suggestsb->s)
				sug--;
			for (c = 0; sug != suggestsb->s; sug--) {
				if (!*sug) {
					c++;
					if (c == 3) {
						sug++;
						goto redo_suggest;
					} else
						*sug = '\n';
				}
			}
			goto redo_suggest;
		case TK_CTL('z'):
			term_suspend();
			if (ai_max >= 0)
				led_redraw(sb->s, 0, orow, lsh);
			continue;
		case TK_CTL('x'):
			sug_pt = sug_pt == len ? -1 : len;
			char buf[100];
			itoa(sug_pt, buf);
			led_info(buf, ai_max);
			continue;
		case TK_CTL('n'):
			if (!suggestsb)
				continue;
			last_sug = sug_pt >= 0 ? sug_pt : led_lastword(sb->s + pre) + pre;
			if (_sug) {
				if (suggestsb->s_n == sug - suggestsb->s)
					continue;
				redo_suggest:
				if (!(_sug = strchr(sug, '\n'))) {
					sug = suggestsb->s;
					goto lookup;
				}
				suggest:
				*_sug = '\0';
				sbuf_cut(sb, last_sug)
				sbufn_str(sb, sug)
				sug = _sug+1;
				continue;
			}
			lookup:
			if (search(sb->s + last_sug, len - last_sug)) {
				sug = suggestsb->s;
				if (!(_sug = strchr(sug, '\n')))
					continue;
				goto suggest;
			}
			continue;
		case TK_CTL('b'):
			if (ai_max >= 0) {
				pac:;
				int r = xrow-xtop+1;
				if (sug)
					goto pac_;
				c = sug_pt >= 0 ? sug_pt : led_lastword(sb->s + pre) + pre;
				if (suggestsb && search(sb->s + c, sb->s_n - c)) {
					sug = suggestsb->s;
					pac_:
					syn_setft("/ac");
					preserve(int, xtd, 2)
					for (int left = 0; r < xrows; r++) {
						led_crender(sug, r, 0, left, left+xcols)
						left += xcols;
						if (left >= rstate->pos[rstate->n])
							break;
					}
					restore(xtd)
					syn_setft(ex_ft);
					r++;
				}
				led_redraw(sb->s, r, orow, lsh);
				continue;
			}
			temp_pos(0, -1, 0, 0);
			temp_write(0, sb->s + pre);
			preserve(struct buf*, ex_pbuf, ex_pbuf)
			preserve(struct buf*, ex_buf, ex_buf)
			temp_switch(0);
			vi(1);
			temp_switch(0);
			restore(ex_pbuf)
			restore(ex_buf)
			exbuf_load(ex_buf)
			syn_setft(ex_ft);
			vi(1); /* redraw past screen */
			syn_setft("/-");
			term_pos(xrows, 0);
			xquit = 0;
			cur_histstr:
			i = 0;
		case TK_CTL('a'):
			cs = lbuf_get(tempbufs[0].lb, tempbufs[0].row - i);
			if (cs) {
				sbuf_cut(sb, pre)
				sbuf_str(sb, cs)
				sb->s[--sb->s_n] = '\0';
				i++;
			} else if (i)
				goto cur_histstr;
			break;
		case TK_CTL('l'):
			if (ai_max < 0)
				term_clean();
			else
				led_redraw(sb->s, 0, orow, lsh);
			continue;
		case TK_CTL('o'):;
			preserve(int, xvis, xvis & 4 ? xvis & ~4 : xvis | 4)
			syn_setft(ex_ft);
			if (xvis & 4)
				ex();
			else
				vi(1);
			xquit = 0;
			restore(xvis)
			continue;
		default:
			if (c == '\n' || TK_INT(c))
				goto leave;
			if ((cs = led_read(kmap, c)))
				sbufn_str(sb, cs)
		}
		sug = NULL; _sug = NULL;
		if (ai_max >= 0 && xpac)
			goto pac;
	}
leave:
	vi_insmov = c;
	*key = c;
}

/* read an ex command */
char *led_prompt(char *pref, char *post, char *insert, int *kmap)
{
	int key, n;
	sbuf *sb; sbufn_make(sb, xcols)
	if (pref)
		sbufn_str(sb, pref)
	n = sb->s_n;
	if (insert)
		sbufn_str(sb, insert)
	preserve(int, xtd, +2)
	led_line(sb, 0, n, post, -1, &key, kmap, 0, 0);
	restore(xtd)
	if (key == '\n') {
		temp_pos(0, -1, 0, 0);
		temp_write(0, sb->s + n);
		if (post)
			sbuf_str(sb, post)
		sbufn_done(sb)
	}
	sbuf_free(sb)
	return NULL;
}

/* read visual command input */
sbuf *led_input(char *pref, char **post, int *kmap, int row, int lsh)
{
	sbuf *sb; sbuf_make(sb, xcols)
	int ai_max = 128 * xai;
	int n, ps = 0, key;
	sbufn_str(sb, pref)
	while (1) {
		led_line(sb, ps, sb->s_n, *post, ai_max, &key, kmap, row, lsh);
		if (key != '\n') {
			sbuf_str(sb, *post)
			sbuf_set(sb, '\0', 4)
			sb->s_n -= 4;
			return sb;
		}
		sbufn_chr(sb, key)
		led_printparts(sb, ps, "");
		term_chr('\n');
		term_room(1);
		xrow++;
		n = ps;
		ps = sb->s_n;
		if (ai_max) {	/* updating autoindent */
			while (**post == ' ' || **post == '\t')
				++*post;
			int ai_new = n;
			while (sb->s[ai_new] == ' ' || sb->s[ai_new] == '\t')
				ai_new++;
			ai_new = ai_max > ai_new - n ? ai_new - n : ai_max;
			sbufn_mem(sb, sb->s+n, ai_new)
		}
	}
}

void led_done(void)
{
	if (suggestsb) {
		sbuf_free(suggestsb)
		sbuf_free(acsb)
	}
}
