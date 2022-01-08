static int isword(const char *s)
{
	int c = (unsigned char) s[0];
	return isalnum(c) || c == '_' || c > 127;
}

enum
{
	/* Instructions which consume input bytes */
	CHAR = 1,
	CLASS,
	MATCH,
	ANY,
	/* Assert position */
	WBEG,
	WEND,
	BOL,
	EOL,
	/* Other (special) instructions */
	SAVE,
	/* Instructions which take relative offset as arg */
	JMP,
	SPLIT,
	RSPLIT,
};

/* Return codes for re_sizecode() and re_comp() */
enum {
	RE_SUCCESS = 0,
	RE_SYNTAX_ERROR = -2,
	RE_UNSUPPORTED_SYNTAX = -3,
};

typedef struct rsub rsub;
struct rsub
{
	int ref;
	rsub *freesub;
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
	int alt_stack[5000], altc = 0;

	for (; *re && *re != ')';) {
		switch (*re) {
		case '\\':
			re++;
			if (!*re) goto syntax_error; /* Trailing backslash */
			if (*re == '<' || *re == '>') {
				if (re - *re_loc > 2 && re[-2] == '\\')
					break;
				EMIT(PC++, *re == '<' ? WBEG : WEND);
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
			break;
		case '.':
			term = PC;
			EMIT(PC++, ANY);
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
			for (cnt = 0; *re != ']'; cnt++) {
				if (*re == '\\') re++;
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
				EMIT(PC++, sub);
			}
			int res = _compilecode(&re, prog, sizecode, flags);
			*re_loc = re;
			if (res < 0) return res;
			if (*re != ')') return RE_SYNTAX_ERROR;
			if (capture) {
				EMIT(PC++, SAVE);
				EMIT(PC++, sub + prog->presub + 1);
			}
			break;
		case '{':;
			int maxcnt = 0, mincnt = 0, i = 0, size = PC - term;
			re++;
			while (isdigit((unsigned char) *re))
				mincnt = mincnt * 10 + *re++ - '0';
			if (*re == ',') {
				re++;
				if (*re == '}') {
					EMIT(PC, RSPLIT);
					EMIT(PC+1, REL(PC, PC - size));
					PC += 2;
					maxcnt = mincnt;
				}
				while (isdigit((unsigned char) *re))
					maxcnt = maxcnt * 10 + *re++ - '0';
			} else
				maxcnt = mincnt;
			for (; i < mincnt-1; i++) {
				if (code)
					memcpy(&code[PC], &code[term], size*sizeof(int));
				PC += size;
			}
			for (i = maxcnt-mincnt; i > 0; i--) {
				EMIT(PC++, SPLIT);
				EMIT(PC++, REL(PC, PC+((size+2)*i)));
				if (code)
					memcpy(&code[PC], &code[term], size*sizeof(int));
				PC += size;
			}
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
			term = PC;
			break;
		case '|':
			if (alt_label)
				alt_stack[altc++] = alt_label;
			INSERT_CODE(start, 2, PC);
			EMIT(PC++, JMP);
			alt_label = PC++;
			EMIT(start, SPLIT);
			EMIT(start + 1, REL(start, PC));
			term = PC;
			break;
		case '^':
			EMIT(PC++, BOL);
			term = PC;
			break;
		case '$':
			EMIT(PC++, EOL);
			term = PC;
			break;
		}
		uc_len(c, re) re += c;
	}
	if (code && alt_label) {
		EMIT(alt_label, REL(alt_label, PC) + 1);
		for (int alts = altc; altc; altc--) {
			int at = alt_stack[alts-altc]+altc*2;
			EMIT(at, REL(at, PC) + 1);
		}
	}
	*re_loc = re;
	return RE_SUCCESS;
syntax_error:
	*re_loc = re;
	return RE_SYNTAX_ERROR;
}

int re_sizecode(const char *re)
{
	rcode dummyprog;
	dummyprog.unilen = 4;

	int res = _compilecode(&re, &dummyprog, 1, 0);
	if (res < 0) return res;
	/* If unparsed chars left */
	if (*re)
		return RE_SYNTAX_ERROR;
	return dummyprog.unilen;
}

int re_comp(rcode *prog, const char *re, int nsubs, int flags)
{
	prog->len = 0;
	prog->unilen = 0;
	prog->sub = 0;
	prog->presub = nsubs;
	prog->splits = 0;
	prog->flg = flags;

	int res = _compilecode(&re, prog, 0, flags);
	if (res < 0) return res;
	/* If unparsed chars left */
	if (*re) return RE_SYNTAX_ERROR;
	int icnt = 0, scnt = SPLIT;
	for (int i = 0; i < prog->unilen; i++)
		switch (prog->insts[i]) {
		case CLASS:
			i += prog->insts[i+2] * 2 + 2;
			icnt++;
			break;
		case SPLIT:
			prog->insts[i++] = scnt;
			scnt += 2;
			icnt++;
			break;
		case RSPLIT:
			prog->insts[i] = -scnt;
			scnt += 2;
		case JMP:
		case SAVE:
		case CHAR:
			i++;
		case ANY:
			icnt++;
		}
	prog->insts[prog->unilen++] = SAVE;
	prog->insts[prog->unilen++] = prog->sub + 1;
	prog->insts[prog->unilen++] = MATCH;
	prog->splits = (scnt - SPLIT) / 2 + SPLIT;
	prog->len = icnt+2;
	return RE_SUCCESS;
}

#define _return(state) { if (eol_ch) utf8_length[eol_ch] = 1; return state; } \

#define newsub(init, copy) \
if (freesub) \
	{ s1 = freesub; freesub = s1->freesub; copy } \
else \
	{ s1 = (rsub*)&nsubs[suboff+=rsubsize]; init } \

#define onclist(nn)
#define onnlist(nn) \
if (sdense[spc] < sparsesz) \
	if (sdense[sdense[spc] * 2] == (unsigned int)spc) \
		deccheck(nn) \
sdense[spc] = sparsesz; \
sdense[sparsesz++ * 2] = spc; \

#define decref(csub) \
if (--csub->ref == 0) { \
	csub->freesub = freesub; \
	freesub = csub; \
} \

#define rec_check(nn) \
if (si) { \
	npc = pcs[--si]; \
	nsub = subs[si]; \
	goto rec##nn; \
} \

#define deccheck(nn) { decref(nsub) rec_check(nn) continue; } \

#define fastrec(nn, list, listidx) \
nsub->ref++; \
spc = *npc; \
if ((unsigned int)spc < WBEG) { \
	list[listidx].sub = nsub; \
	list[listidx++].pc = npc; \
	npc = pcs[si]; \
	goto rec##nn; \
} \
subs[si++] = nsub; \
goto next##nn; \

#define saveclist() \
newsub(memcpy(s1->sub, nsub->sub, osubp);, \
memcpy(s1->sub, nsub->sub, osubp / 2);) \

#define savenlist() \
newsub(/*nop*/, /*nop*/) \
memcpy(s1->sub, nsub->sub, osubp); \

#define instclist(nn) \
else if (spc == BOL) { \
	if (flg & REG_NOTBOL || _sp != s) { \
		if (!si && !clistidx) \
			_return(0) \
		deccheck(nn) \
	} \
	npc++; goto rec##nn; \
} \

#define instnlist(nn) \
else if (spc == JMP) { \
	npc += 2 + npc[1]; \
	goto rec##nn; \
} \

#define clistmatch(n)
#define nlistmatch(n) \
if (spc == MATCH) \
	for (i++; i < clistidx; i++) { \
		npc = clist[i].pc; \
		nsub = clist[i].sub; \
		if (*npc == MATCH) \
			goto matched##n; \
		decref(nsub) \
	} \

#define addthread(n, nn, list, listidx) \
rec##nn: \
spc = *npc; \
if ((unsigned int)spc < WBEG) { \
	list[listidx].sub = nsub; \
	list[listidx++].pc = npc; \
	rec_check(nn) \
	list##match(n) \
	continue; \
} \
next##nn: \
if (spc > JMP) { \
	on##list(nn) \
	npc += 2; \
	pcs[si] = npc + npc[-1]; \
	fastrec(nn, list, listidx) \
} else if (spc == SAVE) { \
	if (nsub->ref > 1) { \
		nsub->ref--; \
		save##list() \
		nsub = s1; \
		nsub->ref = 1; \
	} \
	nsub->sub[npc[1]] = _sp; \
	npc += 2; \
	goto rec##nn; \
} else if (spc == WBEG) { \
	if (((sp != s || sp != _sp) && isword(sp)) \
			|| !isword(_sp)) \
		deccheck(nn) \
	npc++; goto rec##nn; \
} else if (spc < 0) { \
	spc = -spc; \
	on##list(nn) \
	npc += 2; \
	pcs[si] = npc; \
	npc += npc[-1]; \
	fastrec(nn, list, listidx) \
} else if (spc == WEND) { \
	if (isword(_sp)) \
		deccheck(nn) \
	npc++; goto rec##nn; \
} else if (spc == EOL) { \
	if (flg & REG_NOTEOL || *_sp != eol_ch) \
		deccheck(nn) \
	npc++; goto rec##nn; \
} inst##list(nn) \
deccheck(nn) \

#define swaplist() \
tmp = clist; \
clist = nlist; \
nlist = tmp; \
clistidx = nlistidx; \

#define deccont() { decref(nsub) continue; }

#define match(n, cpn) \
for (;; sp = _sp) { \
	uc_len(i, sp) uc_code(c, sp) cpn \
	_sp = sp+i;\
	nlistidx = 0, sparsesz = 0; \
	for (i = 0; i < clistidx; i++) { \
		npc = clist[i].pc; \
		nsub = clist[i].sub; \
		spc = *npc; \
		if (spc == CHAR) { \
			if (c != *(npc+1)) \
				deccont() \
			npc += 2; \
		} else if (spc == CLASS) { \
			const char *s = sp; \
			int cp = c; \
			int *pc = npc+1; \
			int is_positive = *pc++; \
			int cnt = *pc++; \
			while (cnt--) { \
				if (cp >= *pc && cp <= pc[1]) { \
					if (is_positive < -1 && cnt < -is_positive) \
					{ \
						uc_len(j, s) s += j; \
						uc_code(cp, s) \
						pc += 2; \
						is_positive++; \
						continue; \
					} \
					is_positive -= is_positive * 2; \
					break; \
				} \
				pc += 2; \
			} \
			if (is_positive > 0) \
				deccont() \
			npc += *(npc+2) * 2 + 3; \
		} else if (spc == MATCH) { \
			matched##n: \
			nlist[nlistidx++].pc = &mcont; \
			if (npc != &mcont) { \
				if (matched) { \
					decref(matched) \
					suboff = 0; \
				} \
				matched = nsub; \
			} \
			if (sp == _sp || nlistidx == 1) { \
				for (i = 0, j = i; i < nsubp; i+=2, j++) { \
					subp[i] = matched->sub[j]; \
					subp[i+1] = matched->sub[nsubp / 2 + j]; \
				} \
				_return(1) \
			} \
			swaplist() \
			goto _continue##n; \
		} else \
			npc++; \
		addthread(n, 2##n, nlist, nlistidx) \
	} \
	if (sp == _sp) \
		break; \
	swaplist() \
	jmp_start##n: \
	newsub(memset(s1->sub, 0, osubp);, /*nop*/) \
	s1->ref = 1; \
	s1->sub[0] = _sp; \
	npc = insts; nsub = s1; \
	addthread(n, 1##n, clist, clistidx) \
	_continue##n:; \
} \
_return(0) \

int re_pikevm(rcode *prog, const char *s, const char **subp, int nsubp, int flg)
{
	if (!*s)
		return 0;
	const char *sp = s, *_sp = s;
	int rsubsize = sizeof(rsub)+(sizeof(char*)*nsubp);
	int i, j, c, suboff = rsubsize, *npc, osubp = nsubp * sizeof(char*);
	int si = 0, clistidx = 0, nlistidx, spc, mcont = MATCH;
	int *insts = prog->insts, eol_ch = flg & REG_NEWLINE ? '\n' : 0;
	int *pcs[prog->splits];
	unsigned int sdense[prog->splits * 2], sparsesz;
	rsub *subs[prog->splits];
	char nsubs[rsubsize * (prog->len-prog->splits+14)];
	rsub *nsub, *s1, *matched = NULL, *freesub = NULL;
	rthread _clist[prog->len], _nlist[prog->len];
	rthread *clist = _clist, *nlist = _nlist, *tmp;
	flg = prog->flg | flg;
	if (eol_ch)
		utf8_length[eol_ch] = 0;
	if (flg & REG_ICASE)
		goto jmp_start1;
	goto jmp_start2;
	match(1, c = tolower(c);)
	match(2, /*nop*/)
}

static int re_groupcount(char *s)
{
	int n = *s == '(' && s[1] != '?' ? 1 : 0;
	while (*s++)
		if (s[0] == '(' && s[-1] != '\\' && s[1] != '?')
			n++;
	return n;
}

void rset_free(rset *rs)
{
	if (!rs)
		return;
	free(rs->regex);
	free(rs->setgrpcnt);
	free(rs->grp);
	free(rs);
}

rset *rset_make(int n, char **re, int flg)
{
	rset *rs = malloc(sizeof(*rs));
	sbuf *sb; sbuf_make(sb, 1024)
	rs->grp = malloc((n + 1) * sizeof(rs->grp[0]));
	rs->setgrpcnt = malloc((n + 1) * sizeof(rs->setgrpcnt[0]));
	rs->grpcnt = 2;
	rs->n = n;
	sbuf_chr(sb, '(')
	for (int i = 0; i < n; i++) {
		if (!re[i]) {
			rs->grp[i] = -1;
			continue;
		}
		if (sb->s_n > 1)
			sbuf_chr(sb, '|')
		sbuf_chr(sb, '(')
		sbuf_str(sb, re[i])
		sbuf_chr(sb, ')')
		rs->grp[i] = rs->grpcnt;
		rs->setgrpcnt[i] = re_groupcount(re[i]);
		rs->grpcnt += 1 + rs->setgrpcnt[i];
	}
	rs->grp[n] = rs->grpcnt;
	sbufn_chr(sb, ')')
	int sz = re_sizecode(sb->s) * sizeof(int);
	char *code = malloc(sizeof(rcode)+abs(sz));
	rs->regex = (rcode*)code;
	if (sz <= 3 || re_comp((rcode*)code, sb->s, rs->grpcnt-1, flg)) {
		rset_free(rs);
		rs = NULL;
	}
	sbuf_free(sb)
	return rs;
}

/* return the index of the matching regular expression or -1 if none matches */
int rset_find(rset *rs, char *s, int n, int *grps, int flg)
{
	regmatch_t subs[rs->grpcnt+1];
	regmatch_t *sub = subs+1;
	if (re_pikevm(rs->regex, s, (const char**)sub, rs->grpcnt * 2, flg))
	{
		subs[0].rm_eo = NULL; /* make sure sub[-1] never matches */
		for (int i = rs->n-1; i >= 0; i--) {
			if (sub[rs->grp[i]].rm_eo)
			{
				int grp, sgrp = rs->setgrpcnt[i] + 1;
				for (int gi = 0; gi < n; gi++) {
					grp = rs->grp[i] + gi;
					if (gi < sgrp && sub[grp].rm_eo
							&& sub[grp].rm_so) {
						grps[gi * 2] = sub[grp].rm_so - s;
						grps[gi * 2 + 1] = sub[grp].rm_eo - s;
					} else {
						grps[gi * 2] = -1;
						grps[gi * 2 + 1] = -1;
					}
				}
				return i;
			}
		}
	}
	return -1;
}

/* read a regular expression enclosed in a delimiter */
char *re_read(char **src)
{
	char *s = *src;
	int delim = (unsigned char) *s++;
	if (!delim)
		return NULL;
	sbuf *sb; sbuf_make(sb, 256)
	while (*s && *s != delim) {
		if (s[0] == '\\' && s[1])
			if (*(++s) != delim)
				sbuf_chr(sb, '\\')
		sbuf_chr(sb, (unsigned char) *s++)
	}
	*src = *s ? s + 1 : s;
	sbufn_done(sb)
}
