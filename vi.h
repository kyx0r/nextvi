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
name = value; \

#define restore(name) \
name = tmp##name; \

/* utility funcs */
int dstrlen(const char *s, char delim);
char *itoa(int n, char s[]);

/* main functions */
void vi(int init);

/* sbuf string buffer, variable-sized string */
#define NEXTSZ(o, r)	MAX(o * 2, o + r)
typedef struct sbuf {
	char *s;	/* allocated buffer */
	int s_n;	/* length of the string stored in s[] */
	int s_sz;	/* size of memory allocated for s[] */
} sbuf;

#define sbuf_extend(sb, newsz) \
{ \
	char *s = sb->s; \
	sb->s_sz = newsz; \
	sb->s = malloc(sb->s_sz); \
	memcpy(sb->s, s, sb->s_n); \
	free(s); \
} \

#define sbuf_make(sb, newsz) \
{ \
	sb = malloc(sizeof(*sb)); \
	sb->s_sz = newsz; \
	sb->s = malloc(newsz); \
	sb->s_n = 0; \
} \

#define sbuf_chr(sb, c) \
{ \
	if (sb->s_n + 1 == sb->s_sz) \
		sbuf_extend(sb, NEXTSZ(sb->s_sz, 1)) \
	sb->s[sb->s_n++] = c; \
} \

#define sbuf_(sb, x, len, func) \
if (sb->s_n + len >= sb->s_sz) \
	sbuf_extend(sb, NEXTSZ(sb->s_sz, len + 1)) \
mem##func(sb->s + sb->s_n, x, len); \
sb->s_n += len; \

#define sbuf_free(sb) { free(sb->s); free(sb); }
#define sbuf_set(sb, ch, len) { sbuf_(sb, ch, len, set) }
#define sbuf_mem(sb, s, len) { sbuf_(sb, s, len, cpy) }
#define sbuf_str(sb, s) { const char *p = s; while(*p) sbuf_chr(sb, *p++) }
#define sbuf_cut(sb, len) { sb->s_n = len; }
/* sbuf functions that NULL terminate strings */
#define sbuf_null(sb) { sb->s[sb->s_n] = '\0'; }
#define sbufn_done(sb) { char *s = sb->s; sbuf_null(sb) free(sb); return s; }
#define sbufn_make(sb, newsz) { sbuf_make(sb, newsz) sbuf_null(sb) }
#define sbufn_set(sb, ch, len) { sbuf_set(sb, ch, len) sbuf_null(sb) }
#define sbufn_mem(sb, s, len) { sbuf_mem(sb, s, len) sbuf_null(sb) }
#define sbufn_str(sb, s) { sbuf_str(sb, s) sbuf_null(sb) }
#define sbufn_cut(sb, len) { sbuf_cut(sb, len) sbuf_null(sb) }
#define sbufn_chr(sb, c) { sbuf_chr(sb, c) sbuf_null(sb) }

/* regex.c regular expression sets */
#define REG_ICASE	0x01
#define REG_NEWLINE	0x02	/* Unlike posix, controls termination by '\n' */
#define REG_NOTBOL	0x04
#define REG_NOTEOL	0x08
typedef struct {
	char *rm_so;
	char *rm_eo;
} regmatch_t;
typedef struct {
	int unilen;	/* number of integers in insts */
	int len;	/* number of atoms/instructions */
	int sub;	/* interim val = save count; final val = nsubs size */
	int presub;	/* interim val = save count; final val = 1 rsub size */
	int splits;	/* number of split insts */
	int sparsesz;	/* sdense size */
	int flg;	/* stored flags */
	int insts[];	/* re code */
} rcode;
/* regular expression set */
typedef struct {
	rcode *regex;		/* the combined regular expression */
	int n;			/* number of regular expressions in this set */
	int *grp;		/* the group assigned to each subgroup */
	int *setgrpcnt;		/* number of groups in each regular expression */
	int grpcnt;		/* group count */
} rset;
rset *rset_make(int n, char **pat, int flg);
int rset_find(rset *re, char *s, int n, int *grps, int flg);
void rset_free(rset *re);
char *re_read(char **src);

/* lbuf.c line buffer, managing a number of lines */
struct lbuf *lbuf_make(void);
void lbuf_free(struct lbuf *lbuf);
int lbuf_rd(struct lbuf *lbuf, int fd, int beg, int end);
int lbuf_wr(struct lbuf *lbuf, int fd, int beg, int end);
void lbuf_edit(struct lbuf *lbuf, char *s, int beg, int end);
char *lbuf_cp(struct lbuf *lbuf, int beg, int end);
char *lbuf_get(struct lbuf *lbuf, int pos);
char **lbuf_buf(struct lbuf *lb);
int lbuf_len(struct lbuf *lbuf);
int lbuf_opt(struct lbuf *lb, char *buf, int pos, int n_del);
void lbuf_mark(struct lbuf *lbuf, int mark, int pos, int off);
int lbuf_jump(struct lbuf *lbuf, int mark, int *pos, int *off);
int lbuf_undo(struct lbuf *lbuf);
int lbuf_redo(struct lbuf *lbuf);
int lbuf_modified(struct lbuf *lb);
void lbuf_saved(struct lbuf *lb, int clear);
int lbuf_indents(struct lbuf *lb, int r);
int lbuf_eol(struct lbuf *lb, int r);
void lbuf_globset(struct lbuf *lb, int pos, int dep);
int lbuf_globget(struct lbuf *lb, int pos, int dep);
int lbuf_findchar(struct lbuf *lb, char *cs, int cmd, int n, int *r, int *o);
int lbuf_search(struct lbuf *lb, rset *re, int dir, int *r, int *o, int *len);
/* motions */
int lbuf_paragraphbeg(struct lbuf *lb, int dir, int *row, int *off);
int lbuf_sectionbeg(struct lbuf *lb, int dir, int *row, int *off);
int lbuf_wordbeg(struct lbuf *lb, int big, int dir, int *row, int *off);
int lbuf_wordend(struct lbuf *lb, int big, int dir, int *row, int *off);
int lbuf_pair(struct lbuf *lb, int *row, int *off);

/* ren.c rendering lines */
typedef struct {
	char **ren_lastchrs;
	char *ren_laststr;	/* to prevent redundant computations, ensure pointer uniqueness */
	int *ren_lastpos;
	int ren_lastn;
} ren_state;
extern ren_state *rstate;
void ren_done(void);
int *ren_position(char *s, char ***c, int *n);
int ren_next(char *s, int p, int dir);
int ren_eol(char *s, int dir);
int ren_pos(char *s, int off);
int ren_cursor(char *s, int pos);
int ren_noeol(char *s, int p);
int ren_off(char *s, int pos);
int ren_region(char *s, int c1, int c2, int *l1, int *l2, int closed);
char *ren_translate(char *s, char *ln, int pos, int end);
int ren_cwid(char *s, int pos);
/* text direction */
int dir_context(char *s);
void dir_init(void);
/* syntax highlighting */
#define SYN_BD		0x010000
#define SYN_IT		0x020000
#define SYN_RV		0x040000
#define SYN_FGMK(f)	(0x100000 | (f))
#define SYN_BGMK(b)	(0x200000 | ((b) << 8))
#define SYN_FLG		0xff0000
#define SYN_FGSET(a)	((a) & 0x1000ff)
#define SYN_BGSET(a)	((a) & 0x20ff00)
#define SYN_FG(a)	((a) & 0xff)
#define SYN_BG(a)	(((a) >> 8) & 0xff)
extern int syn_reload;
extern int syn_blockhl;
char *syn_setft(char *ft);
void syn_scdir(int scdir);
void syn_highlight(int *att, char *s, int n);
char *syn_filetype(char *path);
int syn_merge(int old, int new);
void syn_reloadft(void);
int syn_addhl(char *reg, int func, int reload);
void syn_init(void);

/* uc.c utf-8 helper functions */
extern unsigned char utf8_length[256];
/* return the length of a utf-8 character */
#define uc_len(dst, s) dst = utf8_length[(unsigned char)s[0]];
/* the unicode codepoint of the given utf-8 character */
#define uc_code(dst, s) \
dst = (unsigned char)s[0]; \
if (dst < 192); \
else if (dst < 224) \
	dst = ((dst & 0x1f) << 6) | (s[1] & 0x3f); \
else if (dst < 240) \
	dst = ((dst & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f); \
else if (dst < 248) \
	dst = ((dst & 0x07) << 18) | ((s[1] & 0x3f) << 12) | \
		((s[2] & 0x3f) << 6) | (s[3] & 0x3f); \
else \
	dst = 0; \

int uc_wid(int c);
int uc_slen(char *s);
char *uc_chr(char *s, int off);
int uc_off(char *s, int off);
char *uc_sub(char *s, int beg, int end);
char *uc_dup(const char *s);
int uc_isspace(char *s);
int uc_isprint(char *s);
int uc_isdigit(char *s);
int uc_isalpha(char *s);
int uc_kind(char *c);
int uc_isbell(int c);
int uc_acomb(int c);
char **uc_chop(char *s, int *n);
char *uc_next(char *s);
char *uc_prev(char *beg, char *s);
char *uc_beg(char *beg, char *s);
char *uc_end(char *s);
char *uc_shape(char *beg, char *s);
char *uc_lastline(char *s);

/* term.c managing the terminal */
extern sbuf *term_sbuf;
extern int term_record;
extern int xrows, xcols;
extern unsigned int ibuf_pos, ibuf_cnt, icmd_pos;
void term_init(void);
void term_done(void);
void term_clean(void);
void term_suspend(void);
void term_out(char *s);
void term_chr(int ch);
void term_pos(int r, int c);
void term_kill(void);
void term_room(int n);
int term_rows(void);
int term_cols(void);
int term_read(void);
void term_commit(void);
char *term_att(int att);
void term_push(char *s, unsigned int n);
char *term_cmd(int *n);
#define term_exec(s, n, precode, postcode) \
{ \
	int pbuf_cnt = ibuf_cnt; \
	int pbuf_pos = ibuf_pos; \
	ibuf_pos = pbuf_cnt; \
	precode \
	term_push(s, n); \
	postcode \
	vi(0); \
	ibuf_cnt = pbuf_cnt; \
	ibuf_pos = pbuf_pos; \
	xquit = 0; \
} \

/* process management */
char *cmd_pipe(char *cmd, char *ibuf, int oproc);
int cmd_exec(char *cmd);
char *xgetenv(char* q[]);

#define TK_CTL(x)	((x) & 037)
#define TK_INT(c)	((c) < 0 || (c) == TK_ESC || (c) == TK_CTL('c'))
#define TK_ESC		(TK_CTL('['))

/* led.c line-oriented input and output */
char *led_prompt(char *pref, char *post, char *insert, int *kmap);
char *led_input(char *pref, char *post, int *kmap, int row);
void led_render(char *s0, int row, int cbeg, int cend);
#define led_print(msg, row) led_render(msg, row, xleft, xleft + xcols)
#define led_reprint(msg, row) { rstate->ren_laststr = NULL; led_print(msg, row); }
char *led_read(int *kmap, int c);
int led_pos(char *s, int pos);
void led_done(void);

/* ex.c ex commands */
struct buf {
	char *ft;			/* file type */
	char *path;			/* file path */
	struct lbuf *lb;
	int row, off, top;
	long mtime;			/* modification time */
	signed char td;			/* text direction */
};
extern int xbufcur;
extern struct buf *ex_buf;
extern struct buf *ex_pbuf;
extern struct buf *bufs;
extern struct buf tempbufs[2];
#define EXLEN	512	/* ex line length */
#define ex_path ex_buf->path
#define ex_ft ex_buf->ft
#define xb ex_buf->lb
void temp_open(int i, char *name, char *ft);
void temp_switch(int i);
#define temp_sswitch(i) preserve(struct buf*, ex_pbuf, ex_pbuf) temp_switch(i);
#define temp_pswitch(i) temp_switch(i); restore(ex_pbuf)
void temp_write(int i, char *str);
void temp_done(int i);
void temp_pos(int i, int row, int off, int top);
void ex(void);
int ec_setdir(char *loc, char *cmd, char *arg);
int ex_exec(const char *ln);
#define ex_command(ln) { ex_exec(ln); vi_regput(':', ln, 0); }
char *ex_read(char *msg);
void ex_print(char *line);
void ex_show(char *msg);
void ex_init(char **files, int n);
void ex_bufpostfix(int i, int clear);
int ex_krs(rset **krs, int *dir);
void ex_krsset(char *kwd, int dir);
int ex_edit(const char *path);
void ec_bufferi(int id);
void bufs_switch(int idx);
#define bufs_switchwft(idx) \
{ if (&bufs[idx] != ex_buf) { bufs_switch(idx); syn_setft(ex_ft); } } \

/* conf.c configuration variables */
/* map file names to file types */
struct filetype {
	char *ft;		/* file type */
	char *pat;		/* file name pattern */
};
extern struct filetype fts[];
extern int ftslen;
/* syntax highlighting patterns */
struct highlight {
	char *ft;		/* the filetype of this pattern */
	char *pat;		/* regular expression */
	int att[16];		/* attributes of the matched groups */
	signed char end[16];	/* the group ending this pattern;
				if set on multi-line the block emits all other matches in rset
				else defines hl continuation for the group:
				positive value - continue at rm_so
				zero (default) - continue at rm_eo
				negative value - continue at sp+1 */
	char blkend;		/* the ending group for multi-line patterns */
	char func;		/* if func > 0 some function will use this hl based on this id */
};
extern struct highlight hls[];
extern int hlslen;
/* direction context patterns; specifies the direction of a whole line */
struct dircontext {
	int dir;
	char *pat;
};
extern struct dircontext dctxs[];
extern int dctxlen;
/* direction marks; the direction of a few words in a line */
struct dirmark {
	int ctx;	/* the direction context for this mark; 0 means any */
	int dir;	/* the direction of the matched text */
	int grp;	/* the nested subgroup; 0 means no groups */
	char *pat;
};
extern struct dirmark dmarks[];
extern int dmarkslen;
/* character placeholders */
struct placeholder {
	int cp;		/* the source character codepoint */
	char *d;	/* the placeholder */
	int wid;	/* the width of the placeholder */
};
extern struct placeholder placeholders[];
extern int placeholderslen;
int conf_hlrev(void);
int conf_mode(void);
char **conf_kmap(int id);
int conf_kmapfind(char *name);
char *conf_digraph(int c1, int c2);

/* vi.c */
char *vi_regget(int c, int *lnmode);
void vi_regput(int c, const char *s, int lnmode);
/* file system */
void dir_calc(char *path);
/* global variables */
extern int xrow;
extern int xoff;
extern int xtop;
extern int xleft;
extern int xvis;
extern int xled;
extern int xquit;
extern int xic;
extern int xai;
extern int xtd;
extern int xshape;
extern int xorder;
extern int xhl;
extern int xhll;
extern int xhlw;
extern int xhlp;
extern int xhlr;
extern int xkmap;
extern int xkmap_alt;
extern int xtabspc;
extern int xqexit;
extern int xish;
extern int xgrp;
extern int xpac;
extern int xkwdcnt;
extern int xkwddir;
extern rset *xkwdrs;
extern sbuf *xacreg;
extern rset *fsincl;
extern char *fs_exdir;
extern int vi_hidch;
extern int vi_insmov;
