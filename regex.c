#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vi.h"

/* regular expressions atoms */
#define RA_CHR		'\0'	/* character literal */
#define RA_BEG		'^'	/* string start */
#define RA_END		'$'	/* string end */
#define RA_ANY		'.'	/* any character */
#define RA_BRK		'['	/* bracket expression */
#define RA_WBEG		'<'	/* word start */
#define RA_WEND		'>'	/* word end */

/* regular expression node types */
#define RN_ATOM		'\0'	/* regular expression */
#define RN_CAT		'c'	/* concatenation */
#define RN_ALT		'|'	/* alternate expressions */
#define RN_GRP		'('	/* pattern group */

/* regular expression program instructions */
#define RI_ATOM		'\0'	/* regular expression */
#define RI_FORK		'f'	/* fork the execution */
#define RI_JUMP		'j'	/* jump to the given instruction */
#define RI_MARK		'm'	/* mark the current position */
#define RI_MATCH	'q'	/* the pattern is matched */

static int regcompflg; /* regcomp flags */

static struct rnode *rnode_make(int rn, struct rnode *c1, struct rnode *c2)
{
	struct rnode *rnode = malloc(sizeof(*rnode));
	memset(rnode, 0, sizeof(*rnode));
	rnode->rn = rn;
	rnode->c1 = c1;
	rnode->c2 = c2;
	rnode->mincnt = 1;
	rnode->maxcnt = 1;
	return rnode;
}

static void rnode_free(struct rnode *rnode)
{
	if (rnode->c1)
		rnode_free(rnode->c1);
	if (rnode->c2)
		rnode_free(rnode->c2);
	free(rnode);
}

static void ratom_copy(struct ratom *dst, struct ratom *src)
{
	dst->ra = src->ra;
	dst->cp = src->cp;
	dst->rbrk = src->rbrk;
}

static int brk_len(char *s)
{
	int n = 1;
	if (s[n] == '^')	/* exclusion mark */
		n++;
	if (s[n] == ']')	/* handling []a] */
		n++;
	while (s[n] && s[n] != ']') {
		if (s[n] == '[' && (s[n + 1] == ':' || s[n + 1] == '='))
			while (s[n] && s[n] != ']')
				n++;
		if (s[n])
			n++;
	}
	return s[n] == ']' ? n + 1 : n;
}

static char *brk_classes[][2] = {
	{":alnum:", "a-zA-Z0-9"},
	{":alpha:", "a-zA-Z"},
	{":blank:", " \t"},
	{":digit:", "0-9"},
	{":lower:", "a-z"},
	{":print:", "\x20-\x7e"},
	{":punct:", "][!\"#$%&'()*+,./:;<=>?@\\^_`{|}~-"},
	{":space:", " \t\r\n\v\f"},
	{":upper:", "A-Z"},
	{": word:", "a-zA-Z0-9_"},
	{":hexit:", "a-fA-F0-9"},
};

static void ratom_readbrk(struct ratom *ra, char **pat)
{
	int len = brk_len(*pat);
	char *p = *pat + 1;
	*pat += len;
	ra->ra = RA_BRK;
	ra->rbrk = malloc(sizeof(struct rbrkinfo));
	struct rbrkinfo *rbrk = ra->rbrk;
	rbrk->begs = malloc(sizeof(rbrk->begs[0])*len);
	rbrk->ends = malloc(sizeof(rbrk->ends[0])*len);
	rbrk->not = p[0] == '^';
	rbrk->and = -1;
	p = rbrk->not ? p + rbrk->not : p;
	int i = 0, end, c;
	int icase = regcompflg & REG_ICASE;
	char *ptmp = NULL;
	char *pnext = NULL;
	while (*p != ']') {
		if (!*p)
		{
			if (pnext)
			{
				p = pnext;
				pnext = NULL;
			} else
				break;
		} else if (ptmp) {
			pnext = p+7;
			p = ptmp;
			ptmp = NULL;
		} else if (p[0] == '[' && p[1] == ':') {
			for (c = 0; c < LEN(brk_classes); c++) {
				if (!strncmp(brk_classes[c][0], p + 1, 7))
				{
					len = uc_slen(brk_classes[c][1])-7+len;
					rbrk->begs = realloc(rbrk->begs, sizeof(rbrk->begs[0])*len);
					rbrk->ends = realloc(rbrk->ends, sizeof(rbrk->ends[0])*len);
					ptmp = brk_classes[c][1];
					break;
				}
			}
		} else if (rbrk->not && rbrk->and < 0 && p[0] == '&' && p[1] == '&') {
			rbrk->and = i;
			p+=2;
			continue;
		}
		uc_code(rbrk->begs[i], p)
		uc_len(c, p) p += c;
		end = rbrk->begs[i];
		if (p[0] == '-' && p[1] && p[1] != ']') {
			p++;
			uc_code(end, p)
			uc_len(c, p) p += c;
		}
		rbrk->ends[i] = end;
		if (icase)
		{
			if (rbrk->begs[i] < 128 && isupper(rbrk->begs[i]))
				rbrk->begs[i] = tolower(rbrk->begs[i]);
			if (rbrk->ends[i] < 128 && isupper(rbrk->ends[i]))
				rbrk->ends[i] = tolower(rbrk->ends[i]);
		}
		i++;
	}
	if (rbrk->and < 0)
		rbrk->and = i;
	rbrk->len = i;
	rbrk->begs = realloc(rbrk->begs, sizeof(rbrk->begs[0])*i);
	rbrk->ends = realloc(rbrk->ends, sizeof(rbrk->ends[0])*i);
}

static void ratom_read(struct ratom *ra, char **pat)
{
	switch ((unsigned char) **pat) {
	case '.':
		ra->ra = RA_ANY;
		(*pat)++;
		break;
	case '^':
		ra->ra = RA_BEG;
		(*pat)++;
		break;
	case '$':
		ra->ra = RA_END;
		(*pat)++;
		break;
	case '[':
		ratom_readbrk(ra, pat);
		break;
	case '\\':
		if ((*pat)[1] == '<' || (*pat)[1] == '>') {
			ra->ra = (*pat)[1] == '<' ? RA_WBEG : RA_WEND;
			*pat += 2;
			break;
		}
		(*pat)++;
	default:;
		ra->ra = RA_CHR;
		uc_code(ra->cp, pat[0])
		if (regcompflg & REG_ICASE)
			ra->cp = tolower(ra->cp);
		int l; uc_len(l, pat[0]) *pat += l;
	}
}

static int isword(char *s)
{
	int c = (unsigned char) s[0];
	return isalnum(c) || c == '_' || c > 127;
}

static struct rnode *rnode_parse(char **pat);

static struct rnode *rnode_grp(char **pat)
{
	struct rnode *rnode = NULL;
	if ((*pat)[0] != '(')
		return NULL;
	*pat += 1;
	if ((*pat)[0] != ')') {
		rnode = rnode_parse(pat);
		if (!rnode)
			return NULL;
	}
	if ((*pat)[0] != ')') {
		rnode_free(rnode);
		return NULL;
	}
	*pat += 1;
	return rnode_make(RN_GRP, rnode, NULL);
}

static struct rnode *rnode_atom(char **pat)
{
	struct rnode *rnode;
	switch ((*pat)[0])
	{
	case '|':
	case ')':
	case 0:
		return NULL;
	case '(':
		rnode = rnode_grp(pat);
		if (!rnode)
			return NULL;
		break;
	default:
		rnode = rnode_make(RN_ATOM, NULL, NULL);
		if (!rnode)
			return NULL;
		ratom_read(&rnode->ra, pat);
		break;
	}
	switch ((*pat)[0])
	{
	case '*':
	case '?':
		rnode->mincnt = 0;
		rnode->maxcnt = (*pat)[0] == '*' ? -1 : 1;
		*pat += 1;
		break;
	case '+':
		rnode->mincnt = 1;
		rnode->maxcnt = -1;
		*pat += 1;
		break;
	case '{':
		rnode->mincnt = 0;
		rnode->maxcnt = 0;
		*pat += 1;
		while (isdigit((unsigned char) **pat))
			rnode->mincnt = rnode->mincnt * 10 + *(*pat)++ - '0';
		if (**pat == ',') {
			(*pat)++;
			if ((*pat)[0] == '}')
				rnode->maxcnt = -1;
			while (isdigit((unsigned char) **pat))
				rnode->maxcnt = rnode->maxcnt * 10 + *(*pat)++ - '0';
		} else {
			rnode->maxcnt = rnode->mincnt;
		}
		*pat += 1;
		if (rnode->mincnt > NREPS || rnode->maxcnt > NREPS) {
			rnode_free(rnode);
			return NULL;
		}
		break;
	}
	return rnode;
}

static struct rnode *rnode_seq(char **pat)
{
	struct rnode *c1 = rnode_atom(pat);
	struct rnode *c2;
	if (!c1)
		return NULL;
	c2 = rnode_seq(pat);
	return c2 ? rnode_make(RN_CAT, c1, c2) : c1;
}

static struct rnode *rnode_parse(char **pat)
{
	struct rnode *c1 = rnode_seq(pat);
	struct rnode *c2;
	if ((*pat)[0] != '|')
		return c1;
	*pat += 1;
	c2 = rnode_parse(pat);
	return c2 ? rnode_make(RN_ALT, c1, c2) : c1;
}

static int rnode_count(struct rnode *rnode)
{
	int n = 1;
	if (!rnode)
		return 0;
	if (rnode->rn == RN_CAT)
		n = rnode_count(rnode->c1) + rnode_count(rnode->c2);
	if (rnode->rn == RN_ALT)
		n = rnode_count(rnode->c1) + rnode_count(rnode->c2) + 2;
	if (rnode->rn == RN_GRP)
		n = rnode_count(rnode->c1) + 2;
	if (rnode->mincnt == 0 && rnode->maxcnt == 0)
		return 0;
	if (rnode->mincnt == 1 && rnode->maxcnt == 1)
		return n;
	if (rnode->maxcnt < 0) {
		n = (rnode->mincnt + 1) * n + 1;
	} else {
		n = (rnode->mincnt + rnode->maxcnt) * n +
			rnode->maxcnt - rnode->mincnt;
	}
	if (!rnode->mincnt)
		n++;
	return n;
}

static int rnode_grpnum(struct rnode *rnode, int num)
{
	int cur = 0;
	if (!rnode)
		return 0;
	if (rnode->rn == RN_GRP)
		rnode->grp = num + cur++;
	cur += rnode_grpnum(rnode->c1, num + cur);
	cur += rnode_grpnum(rnode->c2, num + cur);
	return cur;
}

static int re_insert(regex_t *re, int ri)
{
	re->p[re->n++].ri = ri;
	return re->n - 1;
}

static void rnode_emit(struct rnode *n, regex_t *p);

static void rnode_emitnorep(struct rnode *n, regex_t *p)
{
	int fork, done, mark;
	switch (n->rn)
	{
	case RN_ALT:
		fork = re_insert(p, RI_FORK);
		p->p[fork].a1 = p->n;
		rnode_emit(n->c1, p);
		done = re_insert(p, RI_JUMP);
		p->p[fork].a2 = p->n;
		rnode_emit(n->c2, p);
		p->p[done].a1 = p->n;
		break;
	case RN_CAT:
		rnode_emit(n->c1, p);
		rnode_emit(n->c2, p);
		break;
	case RN_GRP:
		mark = re_insert(p, RI_MARK);
		p->p[mark].mark = 2 * n->grp;
		rnode_emit(n->c1, p);
		mark = re_insert(p, RI_MARK);
		p->p[mark].mark = 2 * n->grp + 1;
		break;
	case RN_ATOM:
		mark = re_insert(p, RI_ATOM);
		ratom_copy(&p->p[mark].ra, &n->ra);
		break;
	}
}

static void rnode_emit(struct rnode *n, regex_t *p)
{
	int jmpend[NREPS];
	int last, i, fork, jmpend_cnt = 0;
	if (!n)
		return;
	if (n->mincnt == 0 && n->maxcnt == 0)
		return;
	if (n->mincnt == 1 && n->maxcnt == 1) {
		rnode_emitnorep(n, p);
		return;
	}
	if (n->mincnt == 0) {
		fork = re_insert(p, RI_FORK);
		p->p[fork].a1 = p->n;
		jmpend[jmpend_cnt++] = fork;
	}
	for (i = 0; i < MAX(1, n->mincnt); i++) {
		last = p->n;
		rnode_emitnorep(n, p);
	}
	if (n->maxcnt < 0) {
		fork = re_insert(p, RI_FORK);
		p->p[fork].a1 = last;
		p->p[fork].a2 = p->n;
	}
	for (i = MAX(1, n->mincnt); i < n->maxcnt; i++) {
		fork = re_insert(p, RI_FORK);
		p->p[fork].a1 = p->n;
		jmpend[jmpend_cnt++] = fork;
		rnode_emitnorep(n, p);
	}
	for (i = 0; i < jmpend_cnt; i++)
		p->p[jmpend[i]].a2 = p->n;
}

int regcomp(regex_t *re, char *pat, int flg)
{
	regcompflg = flg;
	struct rnode *rnode = rnode_parse(&pat);
	int n = rnode_count(rnode) + 4;
	int mark;
	if (!rnode)
		return 1;
	rnode_grpnum(rnode, 1);
	re->n = 0;
	re->p = malloc(n * sizeof(re->p[0]));
	memset(re->p, 0, n * sizeof(re->p[0]));
	re->p[re->n].ri = 'b'; /* break; pattern not matched */
	re->p[re->n].a2 = -1;
	re->p++;
	mark = re_insert(re, RI_MARK);
	re->p[mark].mark = 0;
	rnode_emit(rnode, re);
	mark = re_insert(re, RI_MARK);
	re->p[mark].mark = 1;
	re_insert(re, RI_MATCH);
	rnode_free(rnode);
	re->flg = flg;
	regcompflg = 0;
	return 0;
}

void regfree(regex_t *re)
{
	int i, c;
	for (i = 0; i < re->n; i++)
	{
		if (re->p[i].ra.rbrk) {
			free(re->p[i].ra.rbrk->begs);
			free(re->p[i].ra.rbrk->ends);
			free(re->p[i].ra.rbrk);
			struct rbrkinfo *brki = re->p[i].ra.rbrk;
			for (c = 0; c < re->n; c++)
				if (brki == re->p[c].ra.rbrk)
					re->p[c].ra.rbrk = NULL;
		}
	}
	free(re->p-1);
}

#define backtrack(n) \
{ prs--; prs->pc = (&p[prs->pc])->a2; goto next##n; } \

#define incpc(n) prs->pc++; goto next##n; \

#define match(n, cpn) \
while (*s) \
{ \
	prs = rs+1; \
	prs->pc = 0; \
	prs->s = s; \
	uc_code(prs->cp, prs->s) cpn \
	for (i = 0; i < mmax+1; i++) \
		mark[i] = -1; \
	next##n: \
	ri = &p[prs->pc]; \
	switch (ri->ri) \
	{ \
	case RI_ATOM: \
		switch (ri->ra.ra) \
		{ \
		case RA_CHR: \
			if (ri->ra.cp != prs->cp) \
				backtrack(n) \
			uc_len(l, prs->s) prs->s += l; \
			uc_code(prs->cp, prs->s) cpn \
			incpc(n) \
		case RA_ANY: \
			if (!prs->cp) \
				backtrack(n) \
			uc_len(l, prs->s) prs->s += l; \
			uc_code(prs->cp, prs->s) cpn \
			incpc(n) \
		case RA_BRK: \
			uc_len(l, prs->s) prs->s += l; \
			ts = prs->s; c = prs->cp; \
			len = ri->ra.rbrk->len; \
			not = ri->ra.rbrk->not; \
			begs = ri->ra.rbrk->begs; \
			ends = ri->ra.rbrk->ends; \
			for (i = 0; i < len; i++) \
			{ \
				if (c >= begs[i] && c <= ends[i]) \
				{ \
					if (i < ri->ra.rbrk->and) \
						{not = !not; break;} \
					if (i >= len-1) {\
						if (ts == prs->s) \
							break; \
						backtrack(n) \
					} \
					uc_code(c, ts) \
					uc_len(l, ts) ts += l;\
				} \
			} \
			if (!not || !prs->cp) \
				backtrack(n) \
			uc_code(prs->cp, prs->s) cpn \
			incpc(n) \
		case RA_BEG: \
			if (flg & REG_NOTBOL || (prs->s-o && prs->s[-1] != '\n')) \
				backtrack(n) \
			incpc(n) \
		case RA_END: \
			if (flg & REG_NOTEOL || (prs->cp && prs->cp != '\n')) \
				backtrack(n) \
			incpc(n) \
		case RA_WBEG: \
			if ((prs->s-o && isword(uc_beg(o, prs->s-1))) \
					|| !isword(prs->s)) \
				backtrack(n) \
			incpc(n) \
		case RA_WEND: \
			if (prs->s == o || !isword(uc_beg(o, prs->s - 1)) \
					|| (*prs->s && isword(prs->s))) \
				backtrack(n) \
			incpc(n) \
		} \
	case RI_FORK: \
		if (prs == &rs[NDEPT]) \
			break; \
		(prs+1)->s = prs->s; \
		(prs+1)->cp = prs->cp; \
		prs++; \
	case RI_JUMP: \
		prs->pc = ri->a1; \
		goto next##n; \
	case RI_MARK: \
		mmax = ri->mark; \
		mark[mmax] = prs->s - o; \
		incpc(n) \
	case RI_MATCH: \
		for (i = 0; i < nsub; i++) { \
			psub[i].rm_so = i * 2 < LEN(mark) ? mark[i * 2] : -1; \
			psub[i].rm_eo = i * 2 < LEN(mark) ? mark[i * 2 + 1] : -1; \
		} \
		return 0; \
	} \
	uc_len(l, s) s += l; \
} \

int regexec(regex_t *re, char *s, int nsub, regmatch_t psub[], int flg)
{
	struct rstate rs[NDEPT+1], *prs;
	struct rinst *ri, *p = re->p;
	int mmax = NGRPS, i, l, c, len, not, *begs, *ends;
	int mark[mmax * 2];
	char *o = s, *ts;
	rs[0].pc = -1;
	rs[0].s = s;
	rs[0].cp = 0;
	flg = re->flg | flg;
	nsub = flg & REG_NOSUB ? 0 : nsub;
	if (flg & REG_ICASE)
		match(1, prs->cp = tolower(prs->cp);)
	else
		match(2, /*nop*/)
	return 1;
}

static int re_groupcount(char *s)
{
	int n = 0;
	while (*s) {
		if (s[0] == '(')
			n++;
		if (s[0] == '[') {
			int dep = 0;
			s += s[1] == '^' ? 3 : 2;
			while (s[0] && (s[0] != ']' || dep)) {
				if (s[0] == '[')
					dep++;
				if (s[0] == ']')
					dep--;
				s++;
			}
		}
		if (s[0] == '\\' && s[1])
			s++;
		s++;
	}
	return n;
}

struct rset *rset_make(int n, char **re, int flg)
{
	struct rset *rs = malloc(sizeof(*rs));
	struct sbuf *sb = sbuf_make(1024);
	int regex_flg = REG_EXTENDED | (flg & REG_ICASE ? REG_ICASE : 0);
	int i;
	rs->grp = malloc((n + 1) * sizeof(rs->grp[0]));
	rs->setgrpcnt = malloc((n + 1) * sizeof(rs->setgrpcnt[0]));
	rs->grpcnt = 2;
	rs->n = n;
	sbuf_chr(sb, '(');
	for (i = 0; i < n; i++) {
		if (!re[i]) {
			rs->grp[i] = -1;
			rs->setgrpcnt[i] = 0;
			continue;
		}
		if (sbuf_len(sb) > 1)
			sbuf_chr(sb, '|');
		sbuf_chr(sb, '(');
		sbuf_str(sb, re[i]);
		sbuf_chr(sb, ')');
		rs->grp[i] = rs->grpcnt;
		rs->setgrpcnt[i] = re_groupcount(re[i]);
		rs->grpcnt += 1 + rs->setgrpcnt[i];
	}
	rs->grp[n] = rs->grpcnt;
	sbuf_chr(sb, ')');
	if (regcomp(&rs->regex, sbuf_buf(sb), regex_flg)) {
		free(rs->grp);
		free(rs->setgrpcnt);
		free(rs);
		sbuf_free(sb);
		return NULL;
	}
	sbuf_free(sb);
	return rs;
}

/* return the index of the matching regular expression or -1 if none matches */
int rset_find(struct rset *rs, char *s, int n, int *grps, int flg)
{
	int i, grp, set = -1;
	if (rs->grpcnt <= 2)
		return set;
	regmatch_t subs[rs->grpcnt];
	if (!regexec(&rs->regex, s, rs->grpcnt, subs, flg))
	{
		for (i = rs->n-1; i >= 0; i--)
			if (rs->grp[i] >= 0 && subs[rs->grp[i]].rm_so >= 0)
			{ 
				set = i;
				int sgrp = rs->setgrpcnt[set] + 1;
				for (i = 0; i < n; i++) {
					if (i < sgrp) {
						grp = rs->grp[set] + i;
						grps[i * 2] = subs[grp].rm_so;
						grps[i * 2 + 1] = subs[grp].rm_eo;
					} else {
						grps[i * 2 + 0] = -1;
						grps[i * 2 + 1] = -1;
					}
				}
				break;
			}
	}
	return set;
}

void rset_free(struct rset *rs)
{
	if (!rs)
		return;
	regfree(&rs->regex);
	free(rs->setgrpcnt);
	free(rs->grp);
	free(rs);
}

/* read a regular expression enclosed in a delimiter */
char *re_read(char **src)
{
	struct sbuf *sbuf = sbuf_make(1024);
	char *s = *src;
	int delim = (unsigned char) *s++;
	if (!delim)
		return NULL;
	while (*s && *s != delim) {
		if (s[0] == '\\' && s[1])
			if (*(++s) != delim)
				sbuf_chr(sbuf, '\\');
		sbuf_chr(sbuf, (unsigned char) *s++);
	}
	*src = *s ? s + 1 : s;
	return sbuf_done(sbuf);
}
