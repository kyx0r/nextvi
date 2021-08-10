#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vi.h"

static int isword(const char *s)
{
	int c = (unsigned char) s[0];
	return isalnum(c) || c == '_' || c > 127;
}

enum
{
	// Instructions which consume input bytes (and thus fail if none left)
	CHAR = 1,
	ANY,
	CLASS,
	MATCH,
	// Assert position
	WBEG,
	WEND,
	BOL,
	EOL,
	// Instructions which take relative offset as arg
	JMP,
	SPLIT,
	RSPLIT,
	// Other (special) instructions
	SAVE,
};

// Return codes for re_sizecode() and re_comp()
enum {
	RE_SUCCESS = 0,
	RE_SYNTAX_ERROR = -2,
	RE_UNSUPPORTED_SYNTAX = -3,
};

typedef struct rsub rsub;
struct rsub
{
	int ref;
	const char *sub[];
};

typedef struct rthread rthread;
struct rthread
{
	int *pc;
	rsub *sub;
};

#define INSERT_CODE(at, num, pc) \
if (code) \
	memmove(code + at + num, code + at, (pc - at)*sizeof(int)); \
pc += num;
#define REL(at, to) (to - at - 2)
#define EMIT(at, byte) (code ? (code[at] = byte) : at)
#define PC (prog->unilen)

static int _compilecode(const char **re_loc, rcode *prog, int sizecode, int flags)
{
	const char *re = *re_loc;
	int *code = sizecode ? NULL : prog->insts;
	int start = PC, term = PC;
	int alt_label = 0, c;

	for (; *re && *re != ')';) {
		switch (*re) {
		case '\\':
			re++;
			if (!*re) goto syntax_error; // Trailing backslash
			if (*re == '<' || *re == '>') {
				EMIT(PC++, *re == '<' ? WBEG : WEND);
				prog->len++;
				term = PC;
				break;
			}
		default:
			term = PC;
			EMIT(PC++, CHAR);
			uc_code(c, re)
			if (flags & REG_ICASE)
				c = tolower(c);
			EMIT(PC++, c);
			prog->len++;
			break;
		case '.':
			term = PC;
			EMIT(PC++, ANY);
			prog->len++;
			break;
		case '[':;
			int cnt, l, not = 0;
			term = PC;
			re++;
			EMIT(PC++, CLASS);
			if (*re == '^') {
				not = -1;
				re++;
			}
			PC += 2;
			prog->len++;
			for (cnt = 0; *re != ']'; cnt++) {
				if (!*re) goto syntax_error;
				uc_len(l, re)
				if (not && *re == '&' && re[l] == '&') {
					not = -(cnt+2); re += 2;
				}
				uc_code(c, re)
				if (flags & REG_ICASE)
					c = tolower(c);
				EMIT(PC++, c);
				if (re[l] == '-' && re[l+1] != ']')
					re += l+1;
				uc_code(c, re)
				if (flags & REG_ICASE)
					c = tolower(c);
				EMIT(PC++, c);
				uc_len(c, re) re += c;
			}
			not = not < -1 ? -(not+cnt+2) : not;
			EMIT(term + 1, not < 0 ? not : 1);
			EMIT(term + 2, cnt);
			break;
		case '(':;
			term = PC;
			int sub;
			int capture = 1;
			re++;
			if (*re == '?') {
				re++;
				if (*re == ':') {
					capture = 0;
					re++;
				} else {
					*re_loc = re;
					return RE_UNSUPPORTED_SYNTAX;
				}
			}
			if (capture) {
				sub = ++prog->sub;
				EMIT(PC++, SAVE);
				EMIT(PC++, 2 * sub);
				prog->len++;
			}
			int res = _compilecode(&re, prog, sizecode, flags);
			*re_loc = re;
			if (res < 0) return res;
			if (*re != ')') return RE_SYNTAX_ERROR;
			if (capture) {
				EMIT(PC++, SAVE);
				EMIT(PC++, 2 * sub + 1);
				prog->len++;
			}
			break;
		case '{':;
			int maxcnt = 0, mincnt = 0,
			i = 0, icnt = 0, size;
			re++;
			while (isdigit((unsigned char) *re))
				mincnt = mincnt * 10 + *re++ - '0';
			if (*re == ',') {
				re++;
				if (*re == '}')
					maxcnt = 256;
				while (isdigit((unsigned char) *re))
					maxcnt = maxcnt * 10 + *re++ - '0';
			} else
				maxcnt = mincnt;
			for (size = PC - term; i < mincnt-1; i++) {
				if (code)
					memcpy(&code[PC], &code[term], size*sizeof(int));
				PC += size;
			}
			for (i = maxcnt-mincnt; i > 0; i--)
			{
				prog->splits++;
				EMIT(PC++, SPLIT);
				EMIT(PC++, REL(PC, PC+((size+2)*i)));
				if (code)
					memcpy(&code[PC], &code[term], size*sizeof(int));
				PC += size;
			}
			if (code) {
				for (i = 0; i < size; i++)
					switch (code[term+i]) {
					case CLASS:
						i += code[term+i+2] * 2 + 1;
					case JMP:
					case SPLIT:
					case RSPLIT:
					case SAVE:
					case CHAR:
						i++;
						icnt++;
					}
			}
			prog->len += maxcnt * icnt;
			break;
		case '?':
			if (PC == term) goto syntax_error;
			INSERT_CODE(term, 2, PC);
			if (re[1] == '?') {
				EMIT(term, RSPLIT);
				re++;
			} else
				EMIT(term, SPLIT);
			EMIT(term + 1, REL(term, PC));
			prog->len++;
			prog->splits++;
			term = PC;
			break;
		case '*':
			if (PC == term) goto syntax_error;
			INSERT_CODE(term, 2, PC);
			EMIT(PC, JMP);
			EMIT(PC + 1, REL(PC, term));
			PC += 2;
			if (re[1] == '?') {
				EMIT(term, RSPLIT);
				re++;
			} else
				EMIT(term, SPLIT);
			EMIT(term + 1, REL(term, PC));
			prog->splits++;
			prog->len += 2;
			term = PC;
			break;
		case '+':
			if (PC == term) goto syntax_error;
			if (re[1] == '?') {
				EMIT(PC, SPLIT);
				re++;
			} else
				EMIT(PC, RSPLIT);
			EMIT(PC + 1, REL(PC, term));
			PC += 2;
			prog->splits++;
			prog->len++;
			term = PC;
			break;
		case '|':
			if (alt_label)
				EMIT(alt_label, REL(alt_label, PC) + 1);
			INSERT_CODE(start, 2, PC);
			EMIT(PC++, JMP);
			alt_label = PC++;
			EMIT(start, SPLIT);
			EMIT(start + 1, REL(start, PC));
			prog->splits++;
			prog->len += 2;
			term = PC;
			break;
		case '^':
			EMIT(PC++, BOL);
			prog->len++;
			term = PC;
			break;
		case '$':
			EMIT(PC++, EOL);
			prog->len++;
			term = PC;
			break;
		}
		uc_len(c, re) re += c;
	}
	if (alt_label)
		EMIT(alt_label, REL(alt_label, PC) + 1);
	*re_loc = re;
	return RE_SUCCESS;
syntax_error:
	*re_loc = re;
	return RE_SYNTAX_ERROR;
}

int re_sizecode(const char *re)
{
	rcode dummyprog;
	dummyprog.unilen = 3;

	int res = _compilecode(&re, &dummyprog, /*sizecode*/1, 0);
	if (res < 0) return res;
	// If unparsed chars left
	if (*re) return RE_SYNTAX_ERROR;

	return dummyprog.unilen;
}

int re_comp(rcode *prog, const char *re, int flags)
{
	prog->len = 0;
	prog->unilen = 0;
	prog->sub = 0;
	prog->splits = 0;
	prog->gen = 1;
	prog->flg = flags;

	int res = _compilecode(&re, prog, /*sizecode*/0, flags);
	if (res < 0) return res;
	// If unparsed chars left
	if (*re) return RE_SYNTAX_ERROR;

	prog->insts[prog->unilen++] = SAVE;
	prog->insts[prog->unilen++] = 1;
	prog->insts[prog->unilen++] = MATCH;
	prog->len += 2;

	return RE_SUCCESS;
}

#define _return(state) \
{ prog->gen = gen + 1; return state; } \

#define newsub() \
s1 = freesub; \
if (s1) \
	freesub = (rsub*)s1->sub[0]; \
else \
	s1 = (rsub*)&nsubs[rsubsize * subidx++]; \

#define decref(csub) \
if (--csub->ref == 0) { \
	csub->sub[0] = (char*)freesub; \
	freesub = csub; \
} \

#define deccheck(nn) \
{ decref(sub) goto rec_check##nn; } \

#define fastrec(nn, list, listidx) \
if (*pc < WBEG) { \
	list[listidx].sub = sub; \
	list[listidx++].pc = pc; \
	pc = pcs[i]; \
	goto rec##nn; \
} \
subs[i++] = sub; \
goto next##nn; \

#define addthread(nn, list, listidx, _pc, _sub) \
{ \
	int i = 0, *pc = _pc; \
	rsub *sub = _sub; \
	rec##nn: \
	if (*pc < WBEG) { \
		list[listidx].sub = sub; \
		list[listidx++].pc = pc; \
		rec_check##nn: \
		if (i) { \
			pc = pcs[--i]; \
			sub = subs[i]; \
			goto rec##nn; \
		} \
		continue; \
	} \
	next##nn: \
	switch(*pc) { \
	case JMP: \
		pc += 2 + pc[1]; \
		goto rec##nn; \
	case SPLIT: \
		if(plist[pc - insts] == gen) \
			deccheck(nn) \
		plist[pc - insts] = gen; \
		sub->ref++; \
		pc += 2; \
		pcs[i] = pc + pc[-1]; \
		fastrec(nn, list, listidx) \
	case RSPLIT: \
		if(plist[pc - insts] == gen) \
			deccheck(nn) \
		plist[pc - insts] = gen; \
		sub->ref++; \
		pc += 2; \
		pcs[i] = pc; \
		pc += pc[-1]; \
		fastrec(nn, list, listidx) \
	case SAVE: \
		if (sub->ref > 1) { \
			newsub() \
			for (j = 0; j < nsubp; j++) \
				s1->sub[j] = sub->sub[j]; \
			sub->ref--; \
			sub = s1; \
			sub->ref = 1; \
		} \
		sub->sub[pc[1]] = _sp; \
		pc += 2; \
		goto rec##nn; \
	case WBEG: \
		if ((sp != s && isword(sp)) || !isword(_sp)) \
			deccheck(nn) \
		pc++; goto rec##nn; \
	case WEND: \
		if (isword(_sp)) \
			deccheck(nn) \
		pc++; goto rec##nn; \
	case BOL: \
		if (flg & REG_NOTBOL || (_sp != s && *sp != '\n')) { \
			if (!i && !listidx) \
				_return(0) \
			deccheck(nn) \
		} \
		pc++; goto rec##nn; \
	case EOL: \
		if (flg & REG_NOTEOL || (*_sp && *_sp != '\n')) \
			deccheck(nn) \
		pc++; goto rec##nn; \
	} \
} \

#define match(n, cpn, neol) \
for(;; sp = _sp) { \
	gen++; uc_len(i, sp) uc_code(c, sp) cpn \
	_sp = sp+i;\
	for(i = 0; i < clistidx; i++) { \
		npc = clist[i].pc; \
		nsub = clist[i].sub; \
		switch(*npc++) { \
		case CHAR: \
			if(c != *npc++) \
				break; \
		case ANY: \
		addthread##n: \
			addthread(2##n, nlist, nlistidx, npc, nsub) \
		case CLASS:; \
			const char *s = _sp; \
			int cp = c; \
			int *pc = npc; \
			int is_positive = *pc++; \
			int cnt = *pc++; \
			while (cnt--) { \
				if (cp >= *pc && cp <= pc[1]) { \
					if (is_positive < -1 && cnt <= -(is_positive+2)) \
					{ \
						uc_len(j, s) s += j; \
						uc_code(cp, s) \
						pc += 2; \
						continue; \
					} \
					is_positive -= is_positive * 2; \
					break; \
				} \
				pc += 2; \
			} \
			if (is_positive > 0) \
				break; \
			npc += *(npc+1) * 2 + 2; \
			goto addthread##n; \
		case MATCH: \
			if (matched) { \
				decref(matched) \
				subidx = 0; \
			} \
			matched = nsub; \
			goto break_for##n; \
		} \
		decref(nsub) \
	} \
	break_for##n: \
	if (neol !c) \
		break; \
	tmp = clist; \
	clist = nlist; \
	nlist = tmp; \
	clistidx = nlistidx; \
	nlistidx = 0; \
	if (!matched) { \
		jmp_start##n: \
		newsub() \
		s1->ref = 1; \
		for (i = 1; i < nsubp; i++) \
			s1->sub[i] = NULL; \
		s1->sub[0] = _sp; \
		addthread(1##n, clist, clistidx, insts, s1) \
	} else if (!clistidx) \
		break; \
} \
if(matched) { \
	for(i = 0; i < nsubp; i++) \
		subp[i] = matched->sub[i]; \
	_return(1) \
} \
_return(0) \

int re_pikevm(rcode *prog, const char *s, const char **subp, int nsubp, int flg)
{
	int i, j, c, gen, subidx = 1, *npc;
	int rsubsize = sizeof(rsub)+(sizeof(char*)*nsubp);
	int clistidx = 0, nlistidx = 0;
	const char *sp = s, *_sp = s;
	int *insts = prog->insts, *plist = insts+prog->unilen;
	int *pcs[prog->splits];
	rsub *subs[prog->splits];
	char nsubs[rsubsize * (prog->len+3 - prog->splits)];
	rsub *nsub, *s1, *matched = NULL, *freesub = NULL;
	rthread _clist[prog->len], _nlist[prog->len];
	rthread *clist = _clist, *nlist = _nlist, *tmp;
	gen = prog->gen;
	flg = prog->flg | flg;
	if (flg & REG_ICASE && flg & REG_NOTEOL)
		goto jmp_start1;
	if (flg & REG_ICASE)
		goto jmp_start2;
	if (flg & REG_NOTEOL)
		goto jmp_start3;
	goto jmp_start4;
	match(1, c = tolower(c);, /*nop*/)
	match(2, c = tolower(c);, c == '\n' ||)
	match(3, /*nop*/, /*nop*/)
	match(4, /*nop*/, c == '\n' ||)
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
	char *s = sbuf_buf(sb);
	int sz = re_sizecode(s) * sizeof(int);
	if (sz <= 3)
		goto error;
	char *code = malloc((sizeof(rcode)+sz) * 2);
	memset(code+sizeof(rcode)+sz, 0, sizeof(rcode)+sz);
	if (re_comp((rcode*)code, s, regex_flg)) {
		error:
		free(rs->grp);
		free(rs->setgrpcnt);
		free(rs);
		sbuf_free(sb);
		return NULL;
	}
	rs->regex = (rcode*)code;
	sbuf_free(sb);
	return rs;
}

/* return the index of the matching regular expression or -1 if none matches */
int rset_find(struct rset *rs, char *s, int n, int *grps, int flg)
{
	int i, grp, set = -1;
	if (rs->grpcnt <= 2 || !*s)
		return set;
	const char *sub[rs->grpcnt * 2];
	regmatch_t *subs = (regmatch_t*)sub;
	if (re_pikevm(rs->regex, s, sub, rs->grpcnt * 2, flg))
	{
		for (i = rs->n-1; i >= 0; i--) {
			if (rs->grp[i] >= 0 && subs[rs->grp[i]].rm_eo)
			{
				set = i;
				int sgrp = rs->setgrpcnt[set] + 1;
				for (i = 0; i < n; i++) {
					grp = rs->grp[set] + i;
					if (i < sgrp && subs[grp].rm_eo) {
						grps[i * 2] = subs[grp].rm_so - s;
						grps[i * 2 + 1] = subs[grp].rm_eo - s;
					} else {
						grps[i * 2 + 0] = -1;
						grps[i * 2 + 1] = -1;
					}
				}
				break;
			}
		}
	}
	return set;
}

void rset_free(struct rset *rs)
{
	if (!rs)
		return;
	free(rs->regex);
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
