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
	free(rnode->ra.s);
	//free(rnode->ra.rbrk);
	free(rnode);
}

static void ratom_copy(struct ratom *dst, struct ratom *src)
{
	dst->ra = src->ra;
	dst->s = NULL;
	if (src->s) {
		int len = strlen(src->s) + 1;
		dst->s = malloc(len);
		memcpy(dst->s, src->s, len);
		dst->rbrk = src->rbrk;
	}
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

static void ratom_readbrk(struct ratom *ra, char **pat)
{
	int len = brk_len(*pat);
	ra->ra = RA_BRK;
	ra->s = malloc(len + 1);
	memcpy(ra->s, *pat, len);
	ra->s[len] = '\0';
	*pat += len;

	char *p = ra->s + 1;
	ra->rbrk = malloc(sizeof(struct rbrkinfo));
	struct rbrkinfo *rbrk = ra->rbrk;
	rbrk->not = p[0] == '^';
	rbrk->and = rbrk->not && p[1] == '&' && p[2] != ']';
	p = rbrk->not ? p + rbrk->not + rbrk->and : p;
	int i = 0, end;
	int icase = 0; //todo
	/*
	while (*p && *p != ']') {
		if (p[0] == '[' && p[1] == ':') {
			for (i = 0; i < LEN(brk_classes); i++) {
				if (!strncmp(brk_classes[i][0], p + 1, cl_lens[i]))
					if (!brk_match(brk_classes[i][1], c, s, icase))
						return not;
			}
			p += brk_len(p);
			continue;
		}
	*/
	while (*p && *p != ']') {
		rbrk->begs[i] = uc_code(p);
		p += uc_len(p);
		end = rbrk->begs[i];
		if (p[0] == '-' && p[1] && p[1] != ']') {
			p++;
			end = uc_code(p);
			p += uc_len(p);
		}
		if (icase)
		{
			if (rbrk->begs[i] < 128 && isupper(rbrk->begs[i]))
				rbrk->begs[i] = tolower(rbrk->begs[i]);
			if (rbrk->ends[i] < 128 && isupper(rbrk->ends[i]))
				rbrk->ends[i] = tolower(rbrk->ends[i]);
		}
		rbrk->offs[i] = p;
		rbrk->ends[i] = end;
		i++;
	}
	rbrk->len = i;
}

static void ratom_read(struct ratom *ra, char **pat)
{
	int len;
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
		len = uc_len(*pat);
		ra->s = malloc(8);
		memcpy(ra->s, *pat, len);
		ra->s[len] = '\0';
		*pat += len;
	}
}

static int isword(char *s)
{
	int c = (unsigned char) s[0];
	return isalnum(c) || c == '_' || c > 127;
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
	{":word:", "a-zA-Z0-9_"},
	{":xdigit:", "a-fA-F0-9"},
};
/* length of brk_classes[i][0] */
static char cl_lens[] = {7,7,7,7,7,7,7,7,7,6,8};

static int brk_match(struct rbrkinfo *brki, char *brk, int c, char* s, int icase)
{
	int i, oc = c;
	int not = brki->not;
	int and = brki->and;
	int len = brki->len;
	if (icase && c < 128 && isupper(c))
		c = tolower(c);
	for (i = 0; i < len; i++)
	{
		if (c >= brki->begs[i] && c <= brki->ends[i])
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

static int ratom_match(struct ratom *ra, struct rstate *rs)
{
	switch (ra->ra)
	{
	case RA_CHR:;
		int c1 = uc_code(ra->s);
		int c2 = uc_code(rs->s);
		if (rs->flg & REG_ICASE)
		{
			if (c1 < 128 && isupper(c1))
				c1 = tolower(c1);
			if (c2 < 128 && isupper(c2))
				c2 = tolower(c2);
		}
		if (c1 != c2)
			return 1;
		rs->s += uc_len(ra->s);
		return 0;
	case RA_ANY:
		if (!rs->s[0] || (rs->s[0] == '\n' && !(rs->flg & REG_NOTEOL)))
			return 1;
		rs->s += uc_len(rs->s);
		return 0;
	case RA_BRK:;
		int c = uc_code(rs->s);
		if (!c || (c == '\n' && !(rs->flg & REG_NOTEOL)))
			return 1;
		rs->s += uc_len(rs->s);
		return brk_match(ra->rbrk, ra->s + 1, c, rs->s, rs->flg & REG_ICASE);
	case RA_BEG:
		return rs->flg & REG_NOTBOL ? 1 : !(rs->s == rs->o || rs->s[-1] == '\n');
	case RA_END:
		return rs->flg & REG_NOTEOL ? 1 : rs->s[0] != '\0' && rs->s[0] != '\n';
	case RA_WBEG:
		return !((rs->s == rs->o || !isword(uc_beg(rs->o, rs->s - 1))) &&
			isword(rs->s));
	case RA_WEND:
		return !(rs->s != rs->o && isword(uc_beg(rs->o, rs->s - 1)) &&
			(!rs->s[0] || !isword(rs->s)));
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
	return 0;
}

void regfree(regex_t *re)
{
	int i;
	for (i = 0; i < re->n; i++)
		if (re->p[i].ri == RI_ATOM)
			free(re->p[i].ra.s);
	free(re->p);
}

static int re_rec(regex_t *re, struct rstate *rs, int *mark, int *mmax)
{
	struct rinst *ri;
	if (++(rs->dep) >= NDEPT)
		return 1;
	while (1) {
		ri = &re->p[rs->pc];
		switch (ri->ri)
		{
		case RI_ATOM:
			if (ratom_match(&ri->ra, rs))
				return 1;
			rs->pc++;
			continue;
		case RI_MARK:
			if (ri->mark < NGRPS)
			{
				mark[ri->mark] = rs->s - rs->o;
				if (*mmax < ri->mark)
					*mmax = ri->mark+1;
			}
			rs->pc++;
			continue;
		case RI_JUMP:
			rs->pc = ri->a1;
			continue;
		case RI_FORK:;
			struct rstate base;
			base.s = rs->s;
			base.o = rs->o;
			base.pc = ri->a1;
			base.flg = rs->flg;
			base.dep = rs->dep;
			if (!re_rec(re, &base, mark, mmax))
			{
				*rs = base;
				return 0;
			}
			rs->pc = ri->a2;
			continue;
		}
		break;
	}
	return ri->ri != RI_MATCH;
}

int regexec(regex_t *re, char *s, int nsub, regmatch_t psub[], int flg)
{
	struct rstate rs;
	int mark[NGRPS * 2];
	int i, mmax = LEN(mark);
	rs.flg = re->flg | flg;
	rs.o = s;
	nsub = flg & REG_NOSUB ? 0 : nsub;
	while (*s) {
		rs.s = s;
		rs.pc = 0;
		rs.dep = 0;
		for (i = 0; i < mmax; i++)
			mark[i] = -1;
		mmax = 0;
		if (!re_rec(re, &rs, mark, &mmax))
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
