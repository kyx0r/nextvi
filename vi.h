/*
Nextvi main header
==================
The purpose of this file is to provide high level overview
of entire Nextvi. Due to absence of any build system some of
these definitions may not be required to successfully compile
Nextvi. They are kept here for your benefit and organization.
If something is listed here, it must be used across multiple
files and thus is never static.
*/

/* helper macros */
#define LEN(a)		(int)(sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) < (b) ? (b) : (a))
/* for debug; printf() but to file */
#define p(s, ...)\
	{FILE *f = fopen("file", "a");\
	fprintf(f, s, ##__VA_ARGS__);\
	fclose(f);}\

/* ease up ridiculous global stuffing */
#define preserve(type, name, value) \
type tmp##name = name; \
value \

#define restore(name) \
name = tmp##name; \

/* utility funcs */
void *emalloc(size_t size);
void *erealloc(void *p, size_t size);
int dstrlen(const char *s, char delim);
char *itoa(int n, char s[]);
void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }

/* main functions */
void vi(int init);
void ex(void);

/* sbuf string buffer, variable-sized string */
#define NEXTSZ(o, r)	o + r + ((o + r) >> 1)
typedef struct sbuf {
	char *s;	/* allocated buffer */
	int s_n;	/* length of the string stored in s[] */
	int s_sz;	/* size of memory allocated for s[] */
} sbuf;

#define sbuf_extend(sb, newsz) \
{ \
	char *__s_ = sb->s; \
	sb->s_sz = newsz; \
	sb->s = emalloc(sb->s_sz); \
	memcpy(sb->s, __s_, sb->s_n); \
	free(__s_); \
} \

#define _sbuf_make(sb, newsz, alloc) \
{ \
	alloc \
	sb->s_sz = newsz; \
	sb->s = emalloc(newsz); \
	sb->s_n = 0; \
} \

#define sbuf_chr(sb, c) \
{ \
	if (sb->s_n + 1 >= sb->s_sz) \
		sbuf_extend(sb, NEXTSZ(sb->s_sz, 1)) \
	sb->s[sb->s_n++] = c; \
} \

#define sbuf_(sb, x, len, func) \
if (sb->s_n + len >= sb->s_sz) \
	sbuf_extend(sb, NEXTSZ(sb->s_sz, len)) \
mem##func(sb->s + sb->s_n, x, len); \
sb->s_n += len; \

#define sbuf_smake(sb, newsz) sbuf _##sb, *sb = &_##sb; _sbuf_make(sb, newsz,)
#define sbuf_make(sb, newsz) { _sbuf_make(sb, newsz, sb = emalloc(sizeof(*sb));) }
#define sbuf_free(sb) { free(sb->s); free(sb); }
#define sbuf_set(sb, ch, len) { sbuf_(sb, ch, len, set) }
#define sbuf_mem(sb, s, len) { sbuf_(sb, s, len, cpy) }
#define sbuf_str(sb, s) { const char *__p_ = s; while(*__p_) sbuf_chr(sb, *__p_++) }
#define sbuf_cut(sb, len) { sb->s_n = len; }
/* sbuf functions that NULL terminate strings */
#define sbuf_null(sb) { sb->s[sb->s_n] = '\0'; }
#define sbufn_set(sb, ch, len) { sbuf_set(sb, ch, len) sbuf_null(sb) }
#define sbufn_mem(sb, s, len) { sbuf_mem(sb, s, len) sbuf_null(sb) }
#define sbufn_str(sb, s) { sbuf_str(sb, s) sbuf_null(sb) }
#define sbufn_cut(sb, len) { sbuf_cut(sb, len) sbuf_null(sb) }
#define sbufn_chr(sb, c) { sbuf_chr(sb, c) sbuf_null(sb) }
#define sbufn_ret(sb, str) { sbuf_null(sb) return str; }

/* regex.c regular expression sets */
#define REG_ICASE	0x01
#define REG_NEWLINE	0x02	/* Unlike posix, controls termination by '\n' */
#define REG_NOTBOL	0x04
#define REG_NOTEOL	0x08
typedef struct {
	char *rm_so;
	char *rm_eo;
} regmatch_t;
typedef struct rcode rcode;
struct rcode {
	rcode **la;		/* lookahead expressions */
	int laidx;		/* lookahead index */
	int unilen;		/* number of integers in insts */
	int len;		/* number of atoms/instructions */
	int sub;		/* interim val = save count; final val = nsubs size */
	int presub;		/* interim val = save count; final val = 1 rsub size */
	int splits;		/* number of split insts */
	int sparsesz;		/* sdense size */
	int flg;		/* stored flags */
	int insts[];		/* re code */
};
/* regular expression set */
typedef struct {
	rcode *regex;		/* the combined regular expression */
	int *grp;		/* the group assigned to each subgroup */
	int *setgrpcnt;		/* number of groups in each regular expression */
	int n;			/* number of regular expressions in this set */
	int grpcnt;		/* group count */
} rset;
rset *rset_make(int n, char **pat, int flg);
rset *rset_smake(char *pat, int flg)
	{ char *ss[1] = {pat}; return rset_make(1, ss, flg); }
int rset_find(rset *re, char *s, int *grps, int flg);
void rset_free(rset *re);
char *re_read(char **src);

/* lbuf.c line buffer, managing a number of lines */
#define NMARKS_BASE		28	/* ('z' - 'a' + 2) */
#define NMARKS			30	/* adj: '`* nonadj: [] */
struct lopt {
	char **ins;		/* inserted lines */
	char **del;		/* deleted lines */
	int *mark;		/* saved marks */
	int pos, pos_off;	/* modification location */
	int n_ins, n_del;	/* modification range */
	int seq;		/* operation number */
	int ref;		/* ins/del ref exists on lbuf */
};
struct linfo {
	int len;
	int grec;
};
struct lbuf {
	char **ln;			/* buffer lines */
	struct lopt *hist;		/* buffer history */
	int mark[NMARKS * 2];		/* mark rows & offs */
	int tmp_mark[4];		/* aux mark state */
	int ln_n;			/* number of lines in ln[] */
	int ln_sz;			/* size of ln[] */
	int useq;			/* current operation sequence */
	int modified;			/* modification state */
	int saved;			/* save state */
	int hist_sz;			/* size of hist[] */
	int hist_n;			/* current history head in hist[] */
	int hist_u;			/* current undo head in hist[] */
};
#define lbuf_len(lb) lb->ln_n
#define lbuf_s(ln) ((struct linfo*)(ln - sizeof(struct linfo)))
#define lbuf_i(lb, pos) ((struct linfo*)(lb->ln[pos] - sizeof(struct linfo)))
struct lbuf *lbuf_make(void);
void lbuf_free(struct lbuf *lb);
int lbuf_rd(struct lbuf *lb, int fd, int beg, int end);
int lbuf_wr(struct lbuf *lb, int fd, int beg, int end);
void lbuf_edit(struct lbuf *lb, char *s, int beg, int end, int o1, int o2);
void lbuf_region(struct lbuf *lb, sbuf *sb, int r1, int o1, int r2, int o2);
char *lbuf_joinsb(struct lbuf *lb, int r1, int r2, sbuf *i, int *o1, int *o2);
char *lbuf_get(struct lbuf *lb, int pos);
void lbuf_smark(struct lbuf *lb, struct lopt *lo, int beg, int o1);
void lbuf_emark(struct lbuf *lb, struct lopt *lo, int end, int o2);
struct lopt *lbuf_opt(struct lbuf *lb, char *buf, int beg, int o1, int n_del);
void lbuf_mark(struct lbuf *lb, int mark, int pos, int off);
int lbuf_jump(struct lbuf *lb, int mark, int *pos, int *off);
int lbuf_undo(struct lbuf *lb, int *row, int *off);
int lbuf_redo(struct lbuf *lb, int *row, int *off);
void lbuf_saved(struct lbuf *lb, int clear);
int lbuf_indents(struct lbuf *lb, int r);
int lbuf_eol(struct lbuf *lb, int r, int state);
int lbuf_next(struct lbuf *lb, int dir, int *r, int *o);
int lbuf_findchar(struct lbuf *lb, char *cs, int cmd, int n, int *r, int *o);
int lbuf_search(struct lbuf *lb, rset *re, int dir, int *r,
			int *o, int ln_n, int skip);
#define lbuf_dedup(lb, str, n) \
{ for (int i = 0; i < lbuf_len(lb);) { \
	char *s = lbuf_get(lb, i); \
	if (n == lbuf_s(s)->len && !memcmp(str, s, n)) \
		lbuf_edit(lb, NULL, i, i + 1, 0, 0); \
	else \
		i++; \
}} \

/* regions */
int lbuf_sectionbeg(struct lbuf *lb, int dir, int *row, int *off, int ch);
int lbuf_wordbeg(struct lbuf *lb, int big, int dir, int *row, int *off);
int lbuf_wordend(struct lbuf *lb, int big, int dir, int *row, int *off);
int lbuf_pair(struct lbuf *lb, int *row, int *off);

/* ren.c rendering lines */
typedef struct {
	char **chrs;
	char *s;	/* to prevent redundant computations, ensure pointer uniqueness */
	int *wid;
	int *col;
	int *pos;
	int n;
	int cmax;
	int ctx;
	int holelen;
	char nullhole[4];
} ren_state;
extern ren_state rstates[3];
extern ren_state *rstate;
#define RS(n, func) { rstate = rstates+n; rstate->s = NULL; func; rstate -= n; }
ren_state *ren_position(char *s);
int ren_next(char *s, int p, int dir);
int ren_eol(char *s, int dir);
int ren_pos(char *s, int off);
int ren_cursor(char *s, int pos);
int ren_noeol(char *s, int p);
int ren_off(char *s, int p);
char *ren_translate(char *s, char *ln);
/* text direction */
int dir_context(char *s);
void dir_init(void);
/* syntax highlighting */
#define SYN_BD		0x10000
#define SYN_IT		0x20000
#define SYN_RV		0x40000
#define SYN_FGMK(f)	(0x100000 | (f))
#define SYN_BGMK(b)	(0x200000 | (b << 8))
#define SYN_FLG		0xff0000
#define SYN_FGSET(a)	(a & 0x1000ff)
#define SYN_BGSET(a)	(a & 0x20ff00)
#define SYN_FG(a)	(a & 0xff)
#define SYN_BG(a)	((a >> 8) & 0xff)
#define SYN_BS		0x400000
#define SYN_BE		0x800000
#define SYN_BSE		0xc00000
#define SYN_BP		0x1000000
#define SYN_IGN		0x2000000
#define SYN_SATT	0x4000000
#define SYN_EATT	0x8000000
#define SYN_ATT		0xc000000
#define SYN_BSSET(a)	(a & SYN_BS)
#define SYN_BESET(a)	(a & SYN_BE)
#define SYN_BSESET(a)	(a & SYN_BSE)
#define SYN_BPSET(a)	(a & SYN_BP)
#define SYN_IGNSET(a)	(a & SYN_IGN)
#define SYN_SATTSET(a)	(a & SYN_SATT)
#define SYN_EATTSET(a)	(a & SYN_EATT)
#define SYN_ATTSET(a)	(a & SYN_ATT)
extern int syn_blockhl;
char *syn_setft(char *ft);
void syn_scdir(int scdir);
void syn_highlight(int *att, char *s, int n);
char *syn_filetype(char *path);
int syn_merge(int old, int new);
void syn_reloadft(int hl);
int syn_findhl(int id);
int syn_addhl(char *reg, int id);
void syn_init(void);

/* uc.c utf-8 helper functions */
extern unsigned char utf8_length[256];
extern int zwlen, def_zwlen;
extern int bclen, def_bclen;
/* return the length of a utf-8 character */
#define uc_len(s) utf8_length[(unsigned char)s[0]]
/* the unicode codepoint of the given utf-8 character */
#define uc_code(dst, s, l) \
dst = (unsigned char)s[0]; \
l = utf8_length[dst]; \
if (l == 1); \
else if (l == 2) \
	dst = ((dst & 0x1f) << 6) | (s[1] & 0x3f); \
else if (l == 3) \
	dst = ((dst & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f); \
else if (l == 4) \
	dst = ((dst & 0x07) << 18) | ((s[1] & 0x3f) << 12) | \
		((s[2] & 0x3f) << 6) | (s[3] & 0x3f); \
else \
	dst = 0; \

int uc_wid(int c);
int uc_slen(char *s);
char *uc_chrn(char *s, int off, int *n);
char *uc_chr(char *s, int off) { int n; return uc_chrn(s, off, &n); }
int uc_off(char *s, int off);
char *uc_subl(char *s, int beg, int end, int *rlen, int *rn);
char *uc_sub(char *s, int beg, int end)
	{ int l; return uc_subl(s, beg, end, &l, &l); }
char *uc_dup(const char *s);
#define uc_isspace(s) ((unsigned char)*s < 0x7f && isspace((unsigned char)*s))
#define uc_isprint(s) ((unsigned char)*s > 0x7f || isprint((unsigned char)*s))
#define uc_isdigit(s) ((unsigned char)*s < 0x7f && isdigit((unsigned char)*s))
#define uc_isalpha(s) ((unsigned char)*s > 0x7f || isalpha((unsigned char)*s))
int uc_kind(char *c);
int uc_isbell(int c);
int uc_acomb(int c);
char *uc_beg(char *beg, char *s);
char *uc_shape(char *beg, char *s, int c);

/* term.c managing the terminal */
extern sbuf *term_sbuf;
extern int term_record;
extern int xrows, xcols;
extern unsigned int ibuf_pos, ibuf_cnt, ibuf_sz, icmd_pos;
extern unsigned char *ibuf, icmd[4096];
extern unsigned int texec, tn;
#define term_write(s, n) if (xled) write(1, s, n);
void term_init(void);
void term_done(void);
void term_clean(void);
void term_suspend(void);
void term_chr(int ch);
void term_pos(int r, int c);
void term_kill(void);
void term_room(int n);
int term_read(void);
void term_commit(void);
char *term_att(int att);
void term_push(char *s, unsigned int n);
void term_back(int c);
#define term_dec() ibuf_pos--; icmd_pos--;
#define term_exec(s, n, type) \
{ \
	preserve(int, ibuf_cnt,) \
	preserve(int, ibuf_pos, ibuf_pos = ibuf_cnt;) \
	term_push(s, n); \
	preserve(int, texec, texec = type;) \
	tn = 0; \
	vi(0); \
	tn = 0; \
	restore(texec) \
	if (xquit > 0) \
		xquit = 0; \
	restore(ibuf_pos) \
	restore(ibuf_cnt) \
} \

/* process management */
sbuf *cmd_pipe(char *cmd, sbuf *ibuf, int oproc, int *status);
char *xgetenv(char* q[]);

#define TK_ESC		TK_CTL('[')
#define TK_CTL(x)	(x & 037)
#define TK_INT(c)	(!c || c == TK_ESC || c == TK_CTL('c'))

/* led.c line-oriented input and output */
typedef struct {
	char *s;
	int off;
	int att;
} led_att;
extern sbuf *led_attsb;
void led_modeswap(void);
void led_prompt(sbuf *sb, char *insert, int *kmap, int *key, int ps, int hist);
void led_input(sbuf *sb, char **post, int postn, int row, int lsh);
void led_render(char *s0, int cbeg, int cend);
#define _led_render(msg, row, col, beg, end, kill) \
{ \
	int record = term_record; \
	term_record = 1; \
	term_pos(row, col); \
	kill \
	led_render(msg, beg, end); \
	if (!record) \
		term_commit(); \
} \

#define led_prender(msg, row, col, beg, end) _led_render(msg, row, col, beg, end,)
#define led_crender(msg, row, col, beg, end) _led_render(msg, row, col, beg, end, term_kill();)
char *led_read(int *kmap, int c);
int led_pos(char *s, int pos);
void led_done(void);

/* ex.c ex commands */
struct buf {
	char *ft;			/* file type */
	char *path;			/* file path */
	struct lbuf *lb;
	int plen, row, off, top;
	long mtime;			/* modification time */
	signed char td;			/* text direction */
};
/* ex options */
extern int xleft;
extern int xquit;
extern int xvis;
extern int xai;
extern int xic;
extern int xhl;
extern int xhll;
extern int xhlw;
extern int xhlp;
extern int xhlr;
extern int xled;
extern int xtd;
extern int xshape;
extern int xorder;
extern int xtbs;
extern int xish;
extern int xgrp;
extern int xpac;
extern int xmpt;
extern int xpr;
extern int xsep;
extern int xlim;
extern int xseq;
/* global variables */
extern int xrow, xoff, xtop;
extern int xbufcur;
extern int xgrec;
extern int xkmap;
extern int xkmap_alt;
extern int xkwddir;
extern int xkwdcnt;
extern sbuf *xacreg;
extern rset *xkwdrs;
extern sbuf *xregs[256];
extern struct buf *bufs;
extern struct buf tempbufs[2];
extern struct buf *ex_buf;
extern struct buf *ex_pbuf;
#define istempbuf(buf) (buf - bufs < 0 || buf - bufs >= xbufcur)
#define xb_path ex_buf->path
#define xb_ft ex_buf->ft
#define xb ex_buf->lb
#define exbuf_load(buf) \
	xrow = buf->row; \
	xoff = buf->off; \
	xtop = buf->top; \
	xtd = buf->td; \

#define exbuf_save(buf) \
	buf->row = xrow; \
	buf->off = xoff; \
	buf->top = xtop; \
	buf->td = xtd; \

void temp_open(int i, char *name, char *ft);
void temp_switch(int i);
void temp_write(int i, char *str);
void temp_pos(int i, int row, int off, int top);
void *ex_exec(const char *ln);
#define ex_command(ln) { ex_exec(ln); ex_regput(':', ln, 0); }
void ex_cprint(char *line, char *ft, int r, int c, int ln);
#define ex_print(line, ft) \
{ preserve(int, xleft, xleft = 0;) RS(2, ex_cprint(line, ft, -1, 0, 1)); restore(xleft) }
void ex_init(char **files, int n);
void ex_bufpostfix(struct buf *p, int clear);
int ex_krs(rset **krs, int *dir);
void ex_krsset(char *kwd, int dir);
int ex_edit(const char *path, int len);
void ex_regput(unsigned char c, const char *s, int append);
void bufs_switch(int idx);
#define bufs_switchwft(idx) \
{ if (&bufs[idx] != ex_buf) { bufs_switch(idx); syn_setft(xb_ft); } } \

/* conf.c configuration variables */
/* map file names to file types */
extern const int conf_mode;
struct filetype {
	char *ft;		/* file type */
	char *pat;		/* file name pattern */
};
extern struct filetype fts[];
extern const int ftslen;
/* syntax highlighting patterns */
struct highlight {
	char *ft;		/* the filetype of this pattern */
	char *pat;		/* regular expression */
	int *att;		/* attributes of the matched groups */
	unsigned char set;	/* subset index */
	unsigned char id;	/* id of this hl */
};
extern struct highlight hls[];
extern const int hlslen;
/* direction context patterns; specifies the direction of a whole line */
struct dircontext {
	char *pat;
	int dir;
};
extern struct dircontext dctxs[];
extern const int dctxlen;
/* direction marks; the direction of a few words in a line */
struct dirmark {
	char *pat;
	int ctx;	/* the direction context for this mark; 0 means any */
	int dir[8];	/* the direction of a matched text group */
};
extern struct dirmark dmarks[];
extern const int dmarkslen;
/* character placeholders */
struct placeholder {
	int cp[2];	/* the source character codepoint */
	char d[8];	/* the placeholder */
	int wid;	/* the width of the placeholder */
	int l;		/* the length of the codepoint */
};
extern struct placeholder _ph[];
extern struct placeholder *ph;
extern int phlen;
extern const int conf_hlrev;
char **conf_kmap(int id);
int conf_kmapfind(char *name);
char *conf_digraph(int c1, int c2);

/* vi.c */
extern int vi_hidch;
extern int vi_insmov;
extern int vi_lncol;
extern char vi_msg[512];
/* file system */
extern rset *fsincl;
extern char *fs_exdir;
void dir_calc(char *path);
