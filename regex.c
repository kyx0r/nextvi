#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vi.h"

#define NGRPS		64	/* maximum number of groups */
#define NREPS		128	/* maximum repetitions */
#define NDEPT		256	/* re_rec() recursion depth limit */

#define MAX(a, b)	((a) < (b) ? (b) : (a))
#define LEN(a)		(sizeof(a) / sizeof((a)[0]))

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

/* regular expression atom */
struct ratom {
	int ra;			/* atom type (RA_*) */
	char *s;		/* atom argument */
};

/* regular expression instruction */
struct rinst {
	struct ratom ra;	/* regular expression atom (RI_ATOM) */
	int ri;			/* instruction type (RI_*) */
	int a1, a2;		/* destination of RI_FORK and RI_JUMP */
	int mark;		/* mark (RI_MARK) */
};

/* regular expression program */
struct regex {
	struct rinst *p;	/* the program */
	int n;			/* number of instructions */
	int flg;		/* regcomp() flags */
};

/* regular expression matching state */
struct rstate {
	char *s;		/* the current position in the string */
	char *o;		/* the beginning of the string */
	int mark[NGRPS * 2];	/* marks for RI_MARK */
	int pc;			/* program counter */
	int flg;		/* flags passed to regcomp() and regexec() */
	int dep;		/* re_rec() depth */
};

/* regular expression tree; used for parsing */
struct rnode {
	struct ratom ra;	/* regular expression atom (RN_ATOM) */
	struct rnode *c1, *c2;	/* children */
	int mincnt, maxcnt;	/* number of repetitions */
	int grp;		/* group number */
	int rn;			/* node type (RN_*) */
};

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
	free(rnode);
}

static void ratom_copy(struct ratom *dst, struct ratom *src)
{
	dst->ra = src->ra;
	dst->s = NULL;
	if (src->s) {
		int len = strlen(src->s);
		dst->s = malloc(len + 1);
		memcpy(dst->s, src->s, len + 1);
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

static int brk_match(char *brk, int c, int icase)
{
	int beg, end;
	int i;
	int not = brk[0] == '^';
	char *p = not ? brk + 1 : brk;
	char *p0 = p;
	if (icase && c < 128 && isupper(c))
		c = tolower(c);
	while (*p && (p == p0 || *p != ']')) {
		if (p[0] == '[' && p[1] == ':') {
			for (i = 0; i < LEN(brk_classes); i++) {
				if (!strncmp(brk_classes[i][0], p + 1, cl_lens[i]))
					if (!brk_match(brk_classes[i][1], c, icase))
						return not;
			}
			p += brk_len(p);
			continue;
		}
		beg = uc_code(p);
		p += uc_len(p);
		end = beg;
		if (p[0] == '-' && p[1] && p[1] != ']') {
			p++;
			end = uc_code(p);
			p += uc_len(p);
		}
		if (icase)
		{
			if (beg < 128 && isupper(beg))
				beg = tolower(beg);
			if (end < 128 && isupper(end))
				end = tolower(end);
		}
		if (c >= beg && c <= end)
			return not;
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
		return brk_match(ra->s + 1, c, rs->flg & REG_ICASE);
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

static int re_insert(struct regex *p, int ri)
{
	p->p[p->n++].ri = ri;
	return p->n - 1;
}

static void rnode_emit(struct rnode *n, struct regex *p);

static void rnode_emitnorep(struct rnode *n, struct regex *p)
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

static void rnode_emit(struct rnode *n, struct regex *p)
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

int regcomp(regex_t *preg, char *pat, int flg)
{
	struct rnode *rnode = rnode_parse(&pat);
	struct regex *re;
	int n = rnode_count(rnode) + 3;
	int mark;
	if (!rnode)
		return 1;
	rnode_grpnum(rnode, 1);
	re = malloc(sizeof(*re));
	memset(re, 0, sizeof(*re));
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
	*preg = re;
	return 0;
}

void regfree(regex_t *preg)
{
	struct regex *re = *preg;
	int i;
	for (i = 0; i < re->n; i++)
		if (re->p[i].ri == RI_ATOM)
			free(re->p[i].ra.s);
	free(re->p);
	free(re);
}

static int re_rec(struct regex *re, struct rstate *rs)
{
	struct rinst *ri = NULL;
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
				rs->mark[ri->mark] = rs->s - rs->o;
			rs->pc++;
			continue;
		case RI_JUMP:
			rs->pc = ri->a1;
			continue;
		case RI_FORK:;
			struct rstate base = *rs;
			rs->pc = ri->a1;
			if (!re_rec(re, rs))
				return 0;
			*rs = base;
			rs->pc = ri->a2;
			continue;
		}
		break;
	}
	return ri->ri != RI_MATCH;
}

static int re_recmatch(struct regex *re, struct rstate *rs, int nsub, regmatch_t *psub)
{
	int i;
	for (i = 0; i < LEN(rs->mark); i++)
		rs->mark[i] = -1;
	rs->pc = 0;
	rs->dep = 0;
	if (!re_rec(re, rs)) {
		for (i = 0; i < nsub; i++) {
			psub[i].rm_so = i * 2 < LEN(rs->mark) ? rs->mark[i * 2] : -1;
			psub[i].rm_eo = i * 2 < LEN(rs->mark) ? rs->mark[i * 2 + 1] : -1;
		}
		return 0;
	}
	return 1;
}

int regexec(regex_t *preg, char *s, int nsub, regmatch_t psub[], int flg)
{
	struct regex *re = *preg;
	struct rstate rs;
	rs.flg = re->flg | flg;
	rs.o = s;
	nsub = flg & REG_NOSUB ? 0 : nsub;
	while (*s) {
		rs.s = s;
		if (!re_recmatch(re, &rs, nsub, psub))
			return 0;
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
	int regex_flg = REG_EXTENDED | (flg & RE_ICASE ? REG_ICASE : 0);
	int i;
	memset(rs, 0, sizeof(*rs));
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
