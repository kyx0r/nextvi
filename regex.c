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
	dst->len = src->len;
	dst->rbrk = src->rbrk;
	src->rbrk = NULL;
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
	rbrk->and = rbrk->not && p[1] == '&' && p[2] != ']';
	p = rbrk->not ? p + rbrk->not + rbrk->and : p;
	int i = 0, end;
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
			for (int c = 0; c < LEN(brk_classes); c++) {
				if (!strncmp(brk_classes[c][0], p + 1, 7))
				{
					end = strlen(brk_classes[c][1]);
					rbrk->begs = realloc(rbrk->begs, sizeof(rbrk->begs[0])*(len-7+end));
					rbrk->ends = realloc(rbrk->ends, sizeof(rbrk->ends[0])*(len-7+end));
					ptmp = brk_classes[c][1];
					break;
				}
			}
		}
		rbrk->begs[i] = uc_code(p);
		p += uc_len(p);
		end = rbrk->begs[i];
		if (p[0] == '-' && p[1] && p[1] != ']') {
			p++;
			end = uc_code(p);
			p += uc_len(p);
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
	rbrk->len = i;
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
	default:
		ra->ra = RA_CHR;
		ra->cp = uc_code(*pat);
		if (regcompflg & REG_ICASE && ra->cp < 128 && isupper(ra->cp))
			ra->cp = tolower(ra->cp);
		ra->len = uc_len(*pat);
		*pat += ra->len;
	}
}

static int isword(char *s)
{
	int c = (unsigned char) s[0];
	return isalnum(c) || c == '_' || c > 127;
}

static int brk_match(struct rbrkinfo *brki, int c, char* s)
{
	int i, oc = c;
	int not = brki->not;
	int and = brki->and;
	int len = brki->len;
	int *begs = brki->begs;
	int *ends = brki->ends;
	for (i = 0; i < len; i++)
	{
		if (c >= begs[i] && c <= ends[i])
		{
			if (and)
			{
				if (i < len-1)
				{
					c = uc_code(s);
					s += uc_len(s);
					continue;
				}
				return c == oc ? !not : not;
			}
			return not;
		}
	}
	return !not;
}

static int ratom_match(struct ratom *ra, char **sp, char *s, char *o, int flg)
{
	switch (ra->ra)
	{
	case RA_CHR:;
		int cp = uc_code(s);
		if (flg & REG_ICASE && cp < 128 && isupper(cp))
			cp = tolower(cp);
		if (ra->cp != cp)
			return 1;
		*sp += ra->len;
		return 0;
	case RA_ANY:
		if (!s[0] || (s[0] == '\n' && !(flg & REG_NOTEOL)))
			return 1;
		*sp += uc_len(s);
		return 0;
	case RA_BRK:;
		int c = uc_code(s);
		if (!c || (c == '\n' && !(flg & REG_NOTEOL)))
			return 1;
		*sp += uc_len(s);
		if (flg & REG_ICASE && c < 128 && isupper(c))
			c = tolower(c);
		return brk_match(ra->rbrk, c, *sp);
	case RA_BEG:
		return flg & REG_NOTBOL ? 1 : !(s == o || s[-1] == '\n');
	case RA_END:
		return flg & REG_NOTEOL ? 1 : s[0] != '\0' && s[0] != '\n';
	case RA_WBEG:
		return !((s == o || !isword(uc_beg(o, s - 1))) &&
			isword(s));
	case RA_WEND:
		return !(s != o && isword(uc_beg(o, s - 1)) &&
			(!s[0] || !isword(s)));
	}
	return 1;
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
	case RN_ATOM:;
		int atom = re_insert(p, RI_ATOM);
		ratom_copy(&p->p[atom].ra, &n->ra);
		break;
	}
}

static void rnode_emit(struct rnode *n, regex_t *p)
{
	int last;
	int jmpend[NREPS];
	int jmpend_cnt = 0;
	int i;
	if (!n)
		return;
	if (n->mincnt == 0 && n->maxcnt == 0)
		return;
	if (n->mincnt == 1 && n->maxcnt == 1) {
		rnode_emitnorep(n, p);
		return;
	}
	if (n->mincnt == 0) {
		int fork = re_insert(p, RI_FORK);
		p->p[fork].a1 = p->n;
		jmpend[jmpend_cnt++] = fork;
	}
	for (i = 0; i < MAX(1, n->mincnt); i++) {
		last = p->n;
		rnode_emitnorep(n, p);
	}
	if (n->maxcnt < 0) {
		int fork;
		fork = re_insert(p, RI_FORK);
		p->p[fork].a1 = last;
		p->p[fork].a2 = p->n;
	}
	for (i = MAX(1, n->mincnt); i < n->maxcnt; i++) {
		int fork = re_insert(p, RI_FORK);
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
	int n = rnode_count(rnode) + 3;
	int mark;
	if (!rnode)
		return 1;
	rnode_grpnum(rnode, 1);
	re->n = 0;
	re->p = malloc(n * sizeof(re->p[0]));
	memset(re->p, 0, n * sizeof(re->p[0]));
	mark = re_insert(re, RI_MARK);
	re->p[mark].mark = 0;
	rnode_emit(rnode, re);
	mark = re_insert(re, RI_MARK);
	re->p[mark].mark = 1;
	mark = re_insert(re, RI_MATCH);
	rnode_free(rnode);
	re->flg = flg;
	regcompflg = 0;
	return 0;
}

void regfree(regex_t *re)
{
	int i;
	for (i = 0; i < re->n; i++)
	{
		if (re->p[i].ra.rbrk) {
			free(re->p[i].ra.rbrk->begs);
			free(re->p[i].ra.rbrk->ends);
			free(re->p[i].ra.rbrk);
		}
	}
	free(re->p);
}

static int re_match(struct rinst *p, struct rstate *rs, int *mark, int *mmax, char *o, int flg)
{
	struct rinst *ri;
	struct rstate *brs = rs;
	next:
	ri = &p[rs->pc];
	switch (ri->ri)
	{
	case RI_ATOM:
		if (ratom_match(&ri->ra, &rs->s, rs->s, o, flg))
		{
			if (brs == rs)
				return 1;
			else {
				rs--;
				rs->pc = (&p[rs->pc])->a2;
				goto next;
			}
		}
		rs->pc++;
		goto next;
	case RI_FORK:
		(rs+1)->s = rs->s;
		rs++;
	case RI_JUMP:
		rs->pc = ri->a1;
		goto next;
	case RI_MARK:
		if (ri->mark < NGRPS)
		{
			mark[ri->mark] = rs->s - o;
			if (*mmax < ri->mark)
				*mmax = ri->mark+1;
		}
		rs->pc++;
		goto next;
	}
	return ri->ri != RI_MATCH;
}

int regexec(regex_t *re, char *s, int nsub, regmatch_t psub[], int flg)
{
	struct rstate rs[NDEPT];
	struct rstate *prs = rs;
	int mark[NGRPS * 2];
	int i, mmax = LEN(mark);
	char *o = s;
	flg = re->flg | flg;
	nsub = flg & REG_NOSUB ? 0 : nsub;
	while (*s) {
		prs->s = s;
		prs->pc = 0;
		for (i = 0; i < mmax; i++)
			mark[i] = -1;
		mmax = 0;
		if (!re_match(re->p, prs, mark, &mmax, o, flg))
		{
			for (i = 0; i < nsub; i++) {
				psub[i].rm_so = i * 2 < LEN(mark) ? mark[i * 2] : -1;
				psub[i].rm_eo = i * 2 < LEN(mark) ? mark[i * 2 + 1] : -1;
			}
			return 0;
		}
		s += uc_len(s);
	}
	return 1;
}

int regerror(int errcode, regex_t *preg, char *errbuf, int errbuf_size)
{
	return 0;
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
	struct sbuf *sb = sbuf_make();
	sbuf_extend(sb, 1024);
	int regex_flg = REG_EXTENDED | (flg & RE_ICASE ? REG_ICASE : 0);
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
	int regex_flg = 0;
	if (rs->grpcnt <= 2)
		return set;
	if (flg & RE_NOTBOL)
		regex_flg |= REG_NOTBOL;
	if (flg & RE_NOTEOL)
		regex_flg |= REG_NOTEOL;
	regmatch_t subs[rs->grpcnt];
	if (!regexec(&rs->regex, s, rs->grpcnt, subs, regex_flg))
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
	struct sbuf *sbuf = sbuf_make();
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
