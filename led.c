/* line editing and drawing */

typedef struct tern {
	char word;
	char type;
	struct tern* l_child;
	struct tern* r_child;
	struct tern* m_child;
} tern_t;
static tern_t *ROOT = &(tern_t){.word= 'a'};
static sbuf *suggestsb;

/* create a ternary search tree */
static tern_t *create_node(char w)
{
	tern_t *node = (tern_t*)malloc(sizeof(tern_t));
	node->word = w;
	node->l_child = NULL;
	node->m_child = NULL;
	node->r_child= NULL;
	node->type = 0;
	return node;
}

/* insert a null-terminated word into the tree. */
static tern_t *insert_node(const char *string, tern_t *node)
{
	if (!node)
		node = create_node(string[0]);
	if (string[0] < node->word)
		node->l_child = insert_node(string, node->l_child);
	else if (string[0] > node->word)
		node->r_child = insert_node(string, node->r_child);
	else if (*(string+1))
		node->m_child = insert_node(++string, node->m_child);
	else
		node->type = 1;
	return node;
}

static tern_t *find_node(const char *string, int l, tern_t *node)
{
	int i = 0;
	tern_t *currentNode = node;
	while (i < l) {
		if (!currentNode)
			break;
		/* look to the left of word */
		if (string[i] < currentNode->word)
			currentNode = currentNode->l_child;
		/* look to the right of word */
		else if (string[i] > currentNode->word)
			currentNode = currentNode->r_child;
		/* if out of characters, prefix ends on the current node. Now start search */
		else if (i++ == l - 1)
			return currentNode;
		else
			currentNode = currentNode->m_child;
	}
	return NULL;
}

static void deep_search(const char *pattern, int len, tern_t *start)
{
	if (start->type) {
		sbuf_mem(suggestsb, pattern, len)
		sbuf_chr(suggestsb, start->word)
		sbuf_chr(suggestsb, '\n')
	}
	if (start->l_child)
		deep_search(pattern, len, start->l_child);
	if (start->r_child)
		deep_search(pattern, len, start->r_child);
	if (start->m_child) {
		char _pattern[++len];
		memcpy(_pattern, pattern, len);
		_pattern[len-1] = start->word;
		_pattern[len] = '\0';
		deep_search(_pattern, len, start->m_child);
	}
}

static int search(const char *pattern, int l, tern_t *node)
{
	sbuf_cut(suggestsb, 0)
	/* finds the node where the prefix ends. */
	tern_t *current = find_node(pattern, l, node);
	if (!current)
		return 0;
	else if (current->m_child) {
		deep_search(pattern, l, current->m_child);
		sbuf_null(suggestsb)
		return 1;
	}
	return -1;
}

/* Note: Does not free the root node of the tree. */
static void delete(tern_t *root, tern_t *node)
{
	if (node) {
		if (node->l_child) {
			delete(root, node->l_child);
			node->l_child = NULL;
		}
		if (node->r_child) {
			delete(root, node->r_child);
			node->r_child = NULL;
		}
		if (node->m_child) {
			delete(root, node->m_child);
			node->m_child = NULL;
		}
		if (!node->l_child && !node->r_child && !node->m_child) {
			if (node != root)
				free(node);
			return;
		}
	}
}

int dstrlen(const char *s, char delim)
{
	register const char *i;
	for (i=s; *i && *i != delim; ++i);
	return i-s;
}

static void file_ternary(struct lbuf *buf)
{
	char reg[] = "[^\t ;:,`.<>[\\]^%$#@*!?+\\-|/=\\\\{}&\\()'\"]+";
	int len, sidx, grp = xgrp;
	char **ss = lbuf_buf(buf);
	int ln_n = lbuf_len(buf);
	int subs[grp];
	rset *rs = rset_make(1, (char*[]){xacreg ? xacreg->s : reg}, xic ? REG_ICASE : 0);
	if (!rs)
		return;
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
				char ch = part[len];
				part[len] = '\0';
				insert_node(part, ROOT);
				part[len] = ch;
			}
			sidx += subs[grp - 1] > 0 ? subs[grp - 1] : 1;
		}
	}
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
static void led_markrev(int n, char **chrs, int *pos, int *att)
{
	int i = 0, j;
	int hl = conf_hlrev();
	while (i + 1 < n) {
		int dir = led_offdir(chrs, pos, i);
		int beg = i;
		while (i + 1 < n && led_offdir(chrs, pos, i) == dir)
			i++;
		if (dir < 0)
			for (j = beg; j <= i; j++)
				att[j] = syn_merge(hl, att[j]);
		if (i == beg)
			i++;
	}
}

void led_bounds(sbuf *out, int *off, char **chrs, int cbeg, int cend)
{
	int l, i = cbeg;
	int pad = rstate->ren_torg;
	while (i < cend) {
		int o = off[i - cbeg];
		if (o >= 0) {
			if (pad) {
				char pd[i - cbeg];
				memset(pd, ' ', i - cbeg);
				sbuf_mem(out, pd, i - cbeg)
				pad = 0;
			}
			uc_len(l, chrs[o])
			sbuf_mem(out, chrs[o], l)
			for (; off[i - cbeg] == o; i++);
		} else
			i++;
	}
	sbuf_null(out)
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
{ int l, att_old = 0, i = 0; \
while (i < cend) { \
	int o = off[i]; \
	int att_new = 0; \
	if (o >= 0) { \
		for (l = i; off[i] == o; i++); \
		att_new = att[o]; \
		if (att_new != att_old) \
			sbuf_str(out, term_att(att_new)) \
		char *s = ren_translate(chrs[o], s0, i, cend-1); \
		if (s) \
			sbuf_str(out, s) \
		else if (uc_isprint(chrs[o])) { \
			uc_len(l, chrs[o]) \
			print_ch##n(out) \
		} else { \
			hid_ch##n(out) \
		} \
	} else { \
		if (ctx < 0) \
			sbuf_chr(out, ' ') \
		i++; \
	} \
	att_old = att_new; \
} sbufn_str(out, term_att(0)) } \

/* set xtd and return its old value */
static void td_set(int td)
{
	xotd = xtd;
	xtd = td;
}

#define off_for()\
for (i = 0; i < n; i++) { \
	int curbeg = pos[i] - cbeg; \
	if (curbeg >= 0 && curbeg < cterm) { \
		int curwid = ren_cwid(chrs[i], pos[i]); \
		if (curbeg + curwid > cterm) \
			break; \
		for (j = 0; j < curwid; j++) \
			off[curbeg + j] = i; \
	} \
} \

#define off_rev()\
for (i = 0; i < n; i++) { \
	int curbeg = cend - pos[i] - 1; \
	if (curbeg >= 0 && curbeg < cterm) { \
		int curwid = ren_cwid(chrs[i], pos[i]); \
		if (cend - (pos[i] + curwid - 1) - 2 > cterm) \
			break; \
		for (j = 0; j < curwid; j++) \
			off[cend - (pos[i] + j - 1) - 2] = i; \
	} \
} \

#define cull_line(name, postfix)\
led_bounds(name, off, chrs, cbeg, cend); \
s0 = name->s; \
postfix \
cbeg = 0; \
cend = cterm; \
memset(off, -1, (cterm+1) * sizeof(off[0])); \
pos = ren_position(s0, &chrs, &n); \

/* render and highlight a line */
void led_render(char *s0, int row, int cbeg, int cend)
{
	int i, j, n, cterm = cend - cbeg;
	sbuf *bsb, *bound = NULL;
	int *pos;		/* pos[i]: the screen position of the i-th character */
	char **chrs;		/* chrs[i]: the i-th character in s1 */
	int off[cterm+1];	/* off[i]: the character at screen position i */
	int att[cterm+1];	/* att[i]: the attributes of i-th character */
	int ctx = dir_context(s0);
	memset(off, -1, (cterm+1) * sizeof(off[0]));
	memset(att, 0, (cterm+1) * sizeof(att[0]));
	pos = ren_position(s0, &chrs, &n);
	if (ctx < 0) {
		off_rev()
		if (pos[n] > xcols || cbeg)
		{
			td_set(-2);
			ren_save(1, cbeg);
			sbuf_make(bsb, xcols)
			cull_line(bsb, if (strchr(s0, '\n')) *strchr(s0, '\n') = ' ';)
			off_rev()
			sbuf_make(bound, xcols)
			cull_line(bound, /**/)
			off_rev()
			sbuf_free(bsb)
			td_set(xotd);
			/* s0 would be padded, this is only partially right for syn hl */
		}
	} else {
		off_for()
		if (pos[n] > xcols || cbeg)
		{
			ren_save(1, cbeg);
			int xord = xorder;
			xorder = 0;
			sbuf_make(bound, xcols)
			cull_line(bound, /**/)
			off_for()
			xorder = xord;
		}
	}
	if (xhl)
		syn_highlight(att, s0, n);
	if (xhlr)
		led_markrev(n, chrs, pos, att);
	/* generate term output */
	term_pos(row, 0);
	term_kill();
	if (vi_hidch)
		led_out(term_sbuf, 2)
	else
		led_out(term_sbuf, 1)
	if (!term_record)
		term_commit();
	if (bound) {
		rstate->ren_laststr = NULL;
		ren_save(0, 0);
		sbuf_free(bound)
	}
}

/* print a line on the screen; for ex messages */
void led_printmsg(char *s, int row)
{
	td_set(+2);
	led_reprint(s, row);
	td_set(xotd);
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

static void led_printparts(char *ai, const char *pref, char *main,
		char *post, int kmap)
{
	sbuf *ln;
	int off, pos;
	int idir = 0;
	sbuf_make(ln, xcols)
	sbuf_str(ln, ai)
	sbuf_str(ln, pref)
	sbufn_str(ln, main)
	off = uc_slen(ln->s);
	/* cursor position for inserting the next character */
	if (*pref || *main || *ai) {
		int len = ln->s_n;
		sbuf_str(ln, kmap_map(kmap, 'a'))
		sbufn_str(ln, post)
		rstate->ren_laststr = NULL;
		idir = ren_pos(ln->s, off) - ren_pos(ln->s, off - 1) < 0 ? -1 : +1;
		sbuf_cut(ln, len)
	}
	sbufn_str(ln, post)
	rstate->ren_laststr = NULL;
	pos = ren_cursor(ln->s, ren_pos(ln->s, MAX(0, off - 1)));
	if (pos >= xleft + xcols)
		xleft = pos - xcols / 2;
	if (pos < xleft)
		xleft = pos < xcols ? 0 : pos - xcols / 2;
	syn_blockhl = 0;
	led_print(ln->s, -1);
	term_pos(-1, led_pos(ln->s, pos + idir));
	sbuf_free(ln)
}

/* read a character from the terminal */
char *led_read(int *kmap, int c)
{
	static char buf[8];
	int c1, c2, i, n;
	while (!TK_INT(c)) {
		switch (c) {
		case TK_CTL('f'):
			*kmap = xkmap_alt;
			break;
		case TK_CTL('e'):
			*kmap = 0;
			break;
		case TK_CTL('a'):	/* literal character */
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
				uc_len(n, buf)
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

/* read a line from the terminal */
static char *led_line(const char *pref, char *post, char *ai,
		int ai_max, int *key, int *kmap,
		char *insert, int orow)
{
	sbuf *sb;
	int ai_len = strlen(ai), len;
	int c, lnmode, i = 0, last_sug = 0, sug_pt = -1;
	char *cs, *sug = NULL, *_sug = NULL;
	time_t quickexit = 0;
	sbufn_make(sb, xcols)
	if (insert)
		sbufn_str(sb, insert)
	if (!pref)
		pref = "";
	if (!post)
		post = "";
	while (1) {
		led_printparts(ai, pref, sb->s, post, *kmap);
		len = sb->s_n;
		c = term_read();
		switch (c) {
		case TK_CTL('h'):
		case 127:
			if (len)
				sbufn_cut(sb, led_lastchar(sb->s))
			else
				goto leave;
			break;
		case TK_CTL('u'):
			sbufn_cut(sb, sug_pt > 0 && len > sug_pt ? sug_pt : 0)
			break;
		case TK_CTL('w'):
			if (len)
				sbufn_cut(sb, led_lastword(sb->s))
			else
				term_push("bdwi", 5);
			break;
		case TK_CTL('t'):
			if (ai_len < ai_max) {
				ai[ai_len++] = '\t';
				ai[ai_len] = '\0';
			}
			break;
		case TK_CTL('d'):
			/* when ai and pref are empty, remove the first space of sb */
			if (ai_len == 0 && !pref[0]) {
				if (sb->s[0] == ' ' || sb->s[0] == '\t') {
					sbuf_cut(sb, 0)
					sbufn_str(sb, sb->s+1)
				}
			}
			if (ai_len > 0)
				ai[--ai_len] = '\0';
			break;
		case TK_CTL('p'):
			if (vi_regget(0, &lnmode))
				sbufn_str(sb, vi_regget(0, &lnmode))
			break;
		case TK_CTL('g'):
			if (!suggestsb)
				sbuf_make(suggestsb, 1)
			file_ternary(xb);
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
		case TK_CTL('z'):;
			char buf[100];
			sug_pt = sug_pt == len ? -1 : len;
			itoa(sug_pt, buf);
			rstate->ren_laststr = NULL;
			led_render(buf, xtop+xrows, 0, xcols);
			if (ai_max)
				term_pos(xrow - xtop, 0);
			continue;
		case TK_CTL('n'):
			if (!suggestsb)
				continue;
			last_sug = sug_pt >= 0 ? sug_pt : led_lastword(sb->s);
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
			if (search(sb->s + last_sug, len - last_sug, ROOT) == 1) {
				sug = suggestsb->s;
				if (!(_sug = strchr(sug, '\n')))
					continue;
				goto suggest;
			}
			continue;
		case TK_CTL('b'):
			if (ai_max) {
				pac:;
				int r = xrow-xtop+1;
				if (sug)
					goto pac_;
				c = sug_pt >= 0 ? sug_pt : led_lastword(sb->s);
				if (suggestsb && search(sb->s + c, sb->s_n - c, ROOT) == 1) {
					sug = suggestsb->s;
					pac_:
					syn_setft("/ac");
					for (int left = 0; r < xrows; r++) {
						led_render(sug, r, left, left+xcols);
						left += xcols;
						if (left >= rstate->ren_lastpos[rstate->ren_lastn])
							break;
					}
					syn_setft(ex_filetype);
					r++;
				}
				for (; r < xrows && lbuf_get(xb, (r-(xrow-orow))+xtop); r++)
					led_print(lbuf_get(xb, (r-(xrow-orow))+xtop), r);
				term_pos(xrow - xtop, 0);
				continue;
			}
			td_set(xotd);
			temp_pos(0, -1, 0, 0);
			temp_write(0, sb->s);
			temp_switch(0);
			vi(1);
			temp_switch(0);
			vi(1); /* redraw past screen */
			syn_setft("/-");
			term_pos(xrows, 0);
			td_set(+2);
			xquit = 0;
			cur_histstr:
			i = 0;
		case TK_CTL('v'):
			cs = temp_curstr(0, i);
			if (cs) {
				sbuf_cut(sb, 0)
				sbuf_str(sb, cs)
				sb->s[--sb->s_n] = '\0';
				i++;
			} else if (i)
				goto cur_histstr;
			break;
		case TK_CTL('x'):
			term_push("u", 2);
			break;
		case TK_CTL('l'):
			term_clean();
			continue;
		case TK_CTL(']'):
		case TK_CTL('\\'):
			len = sug_pt >= 0 ? sug_pt : len;
			ex_krsset(sb->s + len, 1);
			term_exec(c == TK_CTL('\\') ? "nj" : "Nk", 2,
				/*nop*/, term_push("qq", 3);)
			continue;
		case TK_CTL('o'):
			term_exec(":", 1, /*nop*/, /*nop*/)
			continue;
		case 'j':
			if (xqexit &&
				(difftime(time(0), quickexit) * 1000) < 1000)
			{
				if (sb->s_n) {
					if (sb->s[led_lastchar(sb->s)] != 'k')
						goto _default;
					sbuf_cut(sb, led_lastchar(sb->s))
				}
				c = TK_ESC;
				goto leave;
			}
			goto _default;
		case 'k':
			quickexit = time(0);
		default:
_default:
			if (c == '\n' || TK_INT(c))
				goto leave;
			if ((cs = led_read(kmap, c)))
				sbufn_str(sb, cs)
		}
		sug = NULL; _sug = NULL;
		if (ai_max && xpac)
			goto pac;
	}
leave:
	vi_insmov = c;
	*key = c;
	sbufn_done(sb)
}

/* read an ex command */
char *led_prompt(const char *pref, char *post, char *insert,
		int *kmap)
{
	int key;
	td_set(+2);
	char *s = led_line(pref, post, "", 0, &key, kmap, insert, 0);
	td_set(xotd);
	if (key == '\n') {
		temp_write(0, s);
		sbuf *sb; sbuf_make(sb, 256)
		if (pref)
			sbuf_str(sb, pref)
		sbuf_str(sb, s)
		if (post)
			sbuf_str(sb, post)
		free(s);
		sbufn_done(sb)
	}
	free(s);
	return NULL;
}

/* read visual command input */
char *led_input(char *pref, char *post, int *kmap, int row)
{
	sbuf *sb; sbuf_make(sb, 256)
	char ai[128];
	int ai_max = sizeof(ai) - 1;
	int n = 0, key, orow = row >= 0 ? row : xrow;
	while (n < ai_max && (*pref == ' ' || *pref == '\t'))
		ai[n++] = *pref++;
	ai[n] = '\0';
	while (1) {
		char *ln = led_line(pref, post, ai, ai_max, &key, kmap, NULL, orow);
		int ln_sp = 0;	/* number of initial spaces in ln */
		while (ln[ln_sp] == ' ' || ln[ln_sp] == '\t')
			ln_sp++;
		/* append the auto-indent only if there are other characters */
		if (ln[ln_sp] || (pref && pref[0]) ||
				(key != '\n' && post[0] && post[0] != '\n'))
			sbuf_str(sb, ai)
		if (pref)
			sbuf_str(sb, pref)
		sbuf_str(sb, ln)
		if (key == '\n')
			sbuf_chr(sb, '\n')
		led_printparts(ai, pref ? pref : "", uc_lastline(ln),
				key == '\n' ? "" : post, *kmap);
		if (key == '\n')
			term_chr('\n');
		if (!pref || !pref[0]) {	/* updating autoindent */
			int ai_len = ai_max ? strlen(ai) : 0;
			int ai_new = ln_sp;
			if (ai_len + ai_new > ai_max)
				ai_new = ai_max - ai_len;
			memcpy(ai + ai_len, ln, ai_new);
			ai[ai_len + ai_new] = '\0';
		}
		if (!xai)
			ai[0] = '\0';
		free(ln);
		if (key != '\n')
			break;
		term_room(1);
		pref = NULL;
		n = 0;
		while (xai && (post[n] == ' ' || post[n] == '\t'))
			n++;
		memmove(post, post + n, strlen(post) - n + 1);
		xrow++;
	}
	if (TK_INT(key) || xrow != row)
		sbuf_str(sb, post)
	else
		sbuf_cut(sb, 0)
	sbufn_done(sb)
}

void led_done(void)
{
	if (suggestsb)
		sbuf_free(suggestsb)
	delete(ROOT, ROOT);
}
