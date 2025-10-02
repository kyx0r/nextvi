/* line editing and drawing */

static sbuf *suggestsb;
static sbuf *acsb;
sbuf *led_attsb;

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
	sbuf_smake(sylsb, 1024)
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
	free(sylsb->s);
	sbuf_set(suggestsb, '\0', 4)
	suggestsb->s_n -= 4;
	return suggestsb->s_n;
}

static void file_index(struct lbuf *buf)
{
	char reg[] = "[^\t !-/:-@[-\\]^`{-\x7f]+";
	int len, sidx, grp = xgrp;
	char **ss = buf->ln;
	int ln_n = lbuf_len(buf), n;
	rset *rs = rset_smake(xacreg ? xacreg->s : reg, xic ? REG_ICASE : 0);
	if (!rs)
		return;
	int subs[rs->grpcnt * 2];
	sbuf_smake(ibuf, 1024)
	for (n = 1; n <= acsb->s_n; n++)
		if (acsb->s[n - 1] == '\n')
			sbuf_mem(ibuf, &n, (int)sizeof(n))
	for (int i = 0; i < ln_n; i++) {
		sidx = 0;
		while (rset_find(rs, ss[i]+sidx, subs,
				sidx ? REG_NOTBOL | REG_NEWLINE : REG_NEWLINE) >= 0) {
			/* if target group not found, continue with group 1
			which will always be valid, otherwise there be no match */
			if (subs[grp] < 0) {
				sidx += subs[1] > 0 ? subs[1] : 1;
				continue;
			}
			len = subs[grp + 1] - subs[grp];
			if (len > 1) {
				char *part = ss[i]+sidx+subs[grp];
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
			sidx += subs[grp + 1] > 0 ? subs[grp + 1] : 1;
		}
	}
	sbuf_null(acsb)
	free(ibuf->s);
	rset_free(rs);
}

static char *kmap_map(int kmap, int c)
{
	static char cs[4];
	char **keymap = conf_kmap(kmap);
	cs[0] = c;
	return keymap[c] ? keymap[c] : cs;
}

/* map cursor horizontal position to terminal column number */
int led_pos(char *s, int pos)
{
	if (dir_context(s) < 0)
		return xleft + xcols - pos - 1;
	return pos - xleft;
}

#define print_ch1(out) sbuf_mem(out, chrs[o], l)
#define print_ch2(out) sbuf_mem(out, *chrs[o] == ' ' ? "_" : chrs[o], l)

#define hid_ch1(out) sbuf_set(out, ' ', i - l)
#define hid_ch2(out) \
sbuf_set(out, *chrs[o] == '\n' ? '\\' : '-', i - l) \
if (ctx > 0 && *chrs[o] == '\t') \
	out->s[out->s_n-1] = '>'; \
else if (*chrs[o] == '\t') \
	out->s[out->s_n - (i - l)] = '<'; \

#define led_out(out, n) \
{ \
for (i = 0; i < cterm;) { \
	int att_new = 0; \
	o = off[i]; \
	if (o >= 0) { \
		for (l = i; off[i] == o; i++); \
		att_new = att[bound ? ctt[atti++] : o]; \
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
			i++; \
		} else \
			break; \
	} \
	att_old = att_new; \
} } \

/* render and highlight a line */
void led_render(char *s0, int cbeg, int cend)
{
	if (!xled)
		return;
	ren_state *r = ren_position(s0);
	int j, c, l, i, o, n = r->n;
	int att_old = 0, atti = 0, cterm = cend - cbeg;
	char *bound = NULL;
	char **chrs = r->chrs;	/* chrs[i]: the i-th character in s0 */
	int off[cterm+1];	/* off[i]: the character at screen position i */
	int att[cterm+1];	/* att[i]: the attributes of i-th character */
	int stt[cterm+1];	/* stt[i]: remap off indexes */
	int ctt[cterm+1];	/* ctt[i]: cterm bound attrs */
	int ctx = r->ctx;
	off[cterm] = -1;
	if (ctx < 0) {
		o = cbeg;
		for (c = cterm-1; c >= 0; c--, o++)
			off[c] = o <= r->cmax ? r->col[o] : -1;
	} else {
		for (c = cbeg; c < cend; c++)
			off[c - cbeg] = c <= r->cmax ? r->col[c] : -1;
	}
	if (r->cmax > cterm || cbeg) {
		i = ctx < 0 ? cterm-1 : 0;
		o = off[i];
		if (o >= 0 && cbeg && r->pos[o] < cbeg)
			while (off[i] == o)
				off[ctx < 0 ? i-- : i++] = -1;
		i = ctx < 0 ? 0 : cterm-1;
		o = off[i];
		if (o >= 0 && r->cmax > cterm && r->pos[o] + r->wid[o] > cend)
			while (off[i] == o)
				off[ctx < 0 ? i++ : i--] = -1;
		for (i = 0, c = 0; i < cterm;) {
			if ((o = off[i++]) >= 0) {
				att[c++] = o;
				for (; off[i] == o; i++);
			}
		}
		stt[0] = 0;
		for (i = 1; i < c; i++) {
			int key0 = att[i];
			j = i - 1;
			while (j >= 0 && att[j] > key0) {
				att[j + 1] = att[j];
				stt[j + 1] = stt[j];
				j = j - 1;
			}
			att[j + 1] = key0;
			stt[j + 1] = i;
		}
		sbuf_smake(bsb, cterm*4);
		for (i = 0; i < c; i++) {
			ctt[stt[i]] = i;
			stt[i] = att[i];
			sbuf_mem(bsb, chrs[att[i]], uc_len(chrs[att[i]]))
		}
		sbuf_set(bsb, '\0', 4)
		bound = bsb->s;
	}
	memset(att, 0, MIN(n, cterm+1) * sizeof(att[0]));
	if (xhl)
		syn_highlight(att, bound ? bound : s0, MIN(n, cterm));
	free(bound);
	if (led_attsb && xhl) {
		led_att *p = (led_att*)led_attsb->s;
		for (; (char*)p < &led_attsb->s[led_attsb->s_n]; p++) {
			if (p->s != s0)
				continue;
			if (!bound)
				att[p->off] = syn_merge(p->att, att[p->off]);
			else if (c && stt[0] <= p->off && stt[c-1] >= p->off) {
				i = p->off - stt[0];
				if (i < c && stt[i] == p->off) {
					att[i] = syn_merge(p->att, att[i]);
					continue; /* text not reordered */
				}
				for (l = 0, j = c - 1; l <= j;) {
					i = l + (j - l) / 2;
					if (stt[i] == p->off) {
						att[i] = syn_merge(p->att, att[i]);
						break;
					} else if (stt[i] < p->off)
						l = i + 1;
					else
						j = i - 1;
				}
			}
		}
	}
	if (xhlr && xhl) {
		for (l = 0, i = 0; i < cterm;) {
			o = off[i++];
			if (o < 0)
				continue;
			for (l++; off[i] == o; i++);
			if (o+1 >= n || r->pos[o] + r->wid[o] == r->pos[o + 1])
				continue;
			if (r->pos[o + 1] + r->wid[o + 1] != r->pos[o])
				continue;
			j = bound ? ctt[l-1] : o;
			att[j] = syn_merge(conf_hlrev, att[j]);
			att[j+1] = syn_merge(conf_hlrev, att[j+1]);
		}
	}
	/* generate term output */
	if (vi_hidch)
		led_out(term_sbuf, 2)
	else
		led_out(term_sbuf, 1)
	sbufn_str(term_sbuf, term_att(0))
	if (r->holelen) {
		memcpy(chrs[n], r->nullhole, r->holelen);
		r->holelen = 0;
	}
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

static void led_printparts(sbuf *sb, int pre, int ps,
	char *post, int postn, int ai_max)
{
	if (!xled) {
		sbuf_set(sb, '\0', 4)
		sb->s_n -= 4;
		return;
	}
	int dir, off, pos, psn = sb->s_n;
	sbuf_str(sb, post)
	sbuf_set(sb, '\0', 4)
	/* XXX: O(n) insertion; recursive array data structure cannot be optimized.
	For correctness, rstate must be recomputed. */
	rstate += 2;
	rstate->s = NULL;
	ren_state *r = ren_position(sb->s + ps);
	off = r->n - postn;
	if (ai_max >= 0)
		xoff = off;
	pos = ren_cursor(r->s, r->pos[MAX(0, off-1)]);
	if (off > 0) {
		int two = off > 1 && psn != pre;
		dir = r->pos[off-two] - r->pos[off-(two+1)];
		if (abs(dir) > r->wid[off-(two+1)])
			pos = ren_cursor(r->s, r->pos[off-two]);
		pos += dir < 0 ? -1 : 1;
	}
	if (pos >= xleft + xcols || pos < xleft)
		xleft = pos < xcols ? 0 : pos - xcols / 2;
	syn_blockhl = -1;
	led_crender(r->s, -1, vi_lncol, xleft, xleft + xcols - vi_lncol);
	term_pos(-1, led_pos(r->s, pos) + vi_lncol);
	sbufn_cut(sb, psn)
	rstate -= 2;
}

/* read a character from the terminal */
char *led_read(int *kmap, int c)
{
	static char buf[5];
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
	RS(2, led_crender(str, xtop+xrows, 0, 0, xcols))
	if (ai_max >= 0)
		term_pos(xrow - xtop, 0);
}

static void led_redraw(char *cs, int r, int orow, int lsh)
{
	rstate++;
	for (int nl = 0; r < xrows; r++) {
		if (vi_lncol) {
			term_pos(r, 0);
			term_kill();
		}
		if (r >= orow-xtop && r < xrow-xtop) {
			sbuf_smake(cb, 128)
			nl = dstrlen(cs, '\n');
			sbuf_mem(cb, cs, nl+!!cs[nl])
			sbuf_set(cb, '\0', 4)
			rstate->s = NULL;
			led_crender(cb->s, r, vi_lncol, xleft, xleft + xcols - vi_lncol)
			free(cb->s);
			cs += nl+!!cs[nl];
			continue;
		}
		nl = r < xrow-xtop ? r+xtop : (r-(xrow-orow+lsh))+xtop;
		led_crender(lbuf_get(xb, nl) ? lbuf_get(xb, nl) : "~", r,
			vi_lncol, xleft, xleft + xcols - vi_lncol)
	}
	term_pos(xrow - xtop, 0);
	rstate--;
}

void led_modeswap(void)
{
	preserve(int, xquit, xquit = 0;)
	preserve(int, texec, if (texec == '@') texec = 0;)
	preserve(int, xvis, xvis ^= 4;)
	if (xvis & 4)
		ex();
	else {
		syn_setft(xb_ft);
		vi(1);
	}
	if (xquit > 0)
		restore(xquit)
	restore(texec)
	restore(xvis)
}

/* read a line from the terminal */
static void led_line(sbuf *sb, int ps, int pre, char *post, int postn,
	int ai_max, int *key, int *kmap, int orow, int lsh)
{
	int len, t_row = -2, p_reg = 0;
	int c, i, lsug = 0, sug_pt = -1;
	char *cs, *sug = NULL, *_sug = NULL;
	while (1) {
		led_printparts(sb, pre, ps, post, postn, ai_max);
		len = sb->s_n;
		c = term_read();
		switch (c) {
		case TK_CTL('h'):
		case 127:
			c = 127;
			if (len - pre > 0)
				sbuf_cut(sb, led_lastchar(sb->s + pre) + pre)
			else
				goto leave;
			break;
		case TK_CTL('u'):
			sbuf_cut(sb, sug_pt > pre && len > sug_pt ? sug_pt : pre)
			break;
		case TK_CTL('w'):
			if (len - pre > 0)
				sbuf_cut(sb, led_lastword(sb->s + pre) + pre)
			else if (ai_max >= 0)
				term_push("bdwi", 5);
			break;
		case TK_CTL('t'):
			cs = uc_dup(sb->s + ps);
			sbuf_cut(sb, ps)
			sbuf_chr(sb, '\t')
			sbuf_str(sb, cs)
			free(cs);
			pre++;
			break;
		case TK_CTL('d'):
			if (sb->s[ps] == ' ' || sb->s[ps] == '\t') {
				sbuf_cut(sb, ps)
				sbuf_str(sb, sb->s+ps+1)
				pre--;
			}
			break;
		case TK_CTL(']'):
		case TK_CTL('\\'):
			i = 0;
			retry:
			if (c == TK_CTL(']')) {
				if (!p_reg || p_reg == '9')
					p_reg = '/';
				while (p_reg < '9' && !xregs[++p_reg]);
			} else {
				c = term_read();
				p_reg = c == TK_CTL('\\') ? 0 : c;
			}
			if (xregs[p_reg]) {
				sbuf_chr(sb, p_reg ? p_reg : '~')
				sbuf_chr(sb, ' ')
				sbuf_mem(sb, xregs[p_reg]->s, xregs[p_reg]->s_n)
				sbuf_set(sb, '\0', 4)
				led_info(sb->s + len, ai_max);
				sbuf_cut(sb, len)
			} else if (!i++)
				goto retry;
			continue;
		case TK_CTL('p'):
			if (xregs[p_reg])
				sbuf_mem(sb, xregs[p_reg]->s, xregs[p_reg]->s_n)
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
			lsug = sug_pt >= 0 ? sug_pt : led_lastword(sb->s + pre) + pre;
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
				sbuf_cut(sb, lsug)
				sbuf_str(sb, sug)
				sug = _sug+1;
				continue;
			}
			lookup:
			if (search(sb->s + lsug, len - lsug)) {
				sug = suggestsb->s;
				if (!(_sug = strchr(sug, '\n')))
					continue;
				goto suggest;
			}
			continue;
		case TK_CTL('b'):
			if (ai_max >= 0) {
				pac:;
				sbuf_null(sb)
				int r = xrow-xtop+1;
				if (sug)
					goto pac_;
				c = sug_pt >= 0 ? sug_pt : led_lastword(sb->s + pre) + pre;
				if (suggestsb && search(sb->s + c, sb->s_n - c)) {
					sug = suggestsb->s;
					pac_:
					syn_setft(ac_ft);
					preserve(int, xtd, xtd = 2;)
					for (int left = 0; r < xrows; r++) {
						RS(2, led_crender(sug, r, 0, left, left+xcols))
						left += xcols;
						if (left >= rstates[2].pos[rstates[2].n])
							break;
					}
					restore(xtd)
					syn_setft(xb_ft);
					r++;
				}
				led_redraw(sb->s, r, orow, lsh);
				continue;
			}
			temp_pos(0, -1, 0, 0);
			temp_write(0, sb->s + pre);
			preserve(struct buf*, ex_pbuf,)
			preserve(struct buf*, ex_buf,)
			preserve(int, texec, if (texec == '@') texec = 0;)
			preserve(int, xquit, xquit = 0;)
			temp_switch(0);
			vi(1);
			temp_switch(0);
			restore(ex_pbuf)
			restore(ex_buf)
			restore(texec)
			exbuf_load(ex_buf)
			syn_setft(xb_ft);
			vi(1); /* redraw past screen */
			syn_setft(ex_ft);
			term_pos(xrows, 0);
			if (xquit > 0)
				restore(xquit)
			t_row = tempbufs[0].row;
		case TK_CTL('a'):
			t_row = t_row < -1 ? tempbufs[0].row : t_row;
			t_row += lbuf_len(tempbufs[0].lb);
			t_row = t_row % MAX(1, lbuf_len(tempbufs[0].lb));
			if ((cs = lbuf_get(tempbufs[0].lb, t_row--))) {
				sbuf_cut(sb, pre)
				sbuf_str(sb, cs)
				sb->s_n--;
			}
			break;
		case TK_CTL('l'):
			if (ai_max < 0)
				term_clean();
			else
				led_redraw(sb->s, 0, orow, lsh);
			continue;
		case TK_CTL('o'):;
			led_modeswap();
			continue;
		default:
			if (c == '\n' || TK_INT(c))
				goto leave;
			if ((cs = led_read(kmap, c)))
				sbuf_str(sb, cs)
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
void led_prompt(sbuf *sb, char *insert, int *kmap, int *key, int ps, int hist)
{
	int n = sb->s_n;
	vi_lncol = 0;
	if (insert)
		sbuf_str(sb, insert)
	preserve(int, xtd, xtd = 2;)
	led_line(sb, ps, n, "", 0, -1, key, kmap, 0, 0);
	restore(xtd)
	if (*key == '\n' && hist) {
		lbuf_dedup(tempbufs[0].lb, sb->s + n, sb->s_n - n)
		temp_pos(0, -1, 0, 0);
		temp_write(0, sb->s + n);
	}
}

/* read visual command input */
void led_input(sbuf *sb, char **post, int postn, int row, int lsh)
{
	int ai_max = 128 * xai;
	int n, key, ps = 0;
	while (1) {
		led_line(sb, ps, sb->s_n, *post, postn, ai_max, &key, &xkmap, row, lsh);
		if (key != '\n') {
			if (!xled)
				xoff = uc_slen(sb->s+ps);
			return;
		}
		sbuf_chr(sb, key)
		led_printparts(sb, -1, ps, "", 0, 0);
		term_chr('\n');
		term_room(1);
		xrow++;
		n = ps;
		ps = sb->s_n;
		if (ai_max) {	/* updating autoindent */
			for (; **post == ' ' || **post == '\t'; postn--)
				++*post;
			int ai_new = n;
			while (sb->s[ai_new] == ' ' || sb->s[ai_new] == '\t')
				ai_new++;
			ai_new = ai_max > ai_new - n ? ai_new - n : ai_max;
			sbuf_mem(sb, sb->s+n, ai_new)
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
