/* neatvi main header */

/* helper macros */
#define LEN(a)		(sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) < (b) ? (b) : (a))
#define p(s, ...)\
	{FILE *f = fopen("file", "a");\
	fprintf(f, s, ##__VA_ARGS__);\
	fclose(f);}\

/* utility funcs */
int isescape(char ch);
char *substr(const char *s1, const char *s2, int len1, int len2);
int dstrlen(const char *s, char delim);
char *itoa(int n, char s[]);
void reverse_in_place(char *str, int len);

/* main functions */
int hund(int argc, char **argv);
void vi();

/* signals */
int setup_signals(void);
int sig_hund(int sig);

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
void lbuf_opt(struct lbuf *lb, char *buf, int pos, int n_del);
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
/* motions */
int lbuf_findchar(struct lbuf *lb, char *cs, int cmd, int n, int *r, int *o);
int lbuf_search(struct lbuf *lb, char *kw, int dir, int *r, int *o, int *len);
int lbuf_paragraphbeg(struct lbuf *lb, int dir, int *row, int *off);
int lbuf_sectionbeg(struct lbuf *lb, int dir, int *row, int *off);
int lbuf_wordbeg(struct lbuf *lb, int big, int dir, int *row, int *off);
int lbuf_wordend(struct lbuf *lb, int big, int dir, int *row, int *off);
int lbuf_pair(struct lbuf *lb, int *row, int *off);

/* sbuf.c string buffer, variable-sized string */
struct sbuf *sbuf_make(void);
void sbuf_extend(struct sbuf *sbuf, int newsz);
void sbuf_free(struct sbuf *sb);
char *sbuf_done(struct sbuf *sb);
char *sbuf_buf(struct sbuf *sb);
void sbuf_chr(struct sbuf *sb, int c);
void sbuf_str(struct sbuf *sb, char *s);
char *sbuf_s(struct sbuf *sb);
void sbuf_mem(struct sbuf *sb, char *s, int len);
void sbuf_printf(struct sbuf *sbuf, char *s, ...);
int sbuf_len(struct sbuf *sb);
void sbuf_cut(struct sbuf *s, int len);

/* regex.c regular expression sets */
#define REG_EXTENDED		0x01
#define REG_NOSUB		0x02
#define REG_ICASE		0x04
#define REG_NEWLINE		0x08
#define REG_NOTBOL		0x10
#define REG_NOTEOL		0x20
#define NGRPS		64	/* maximum number of groups */
#define NREPS		128	/* maximum repetitions */
#define NDEPT		4096	/* re_match() depth limit */
typedef struct {
	long rm_so;
	long rm_eo;
} regmatch_t;
/* braket info */
struct rbrkinfo {
	int len;
	int not;
	int and;
	int *begs;
	int *ends;
};
/* regular expression atom */
struct ratom {
	int ra;			/* atom type (RA_*) */
	struct rbrkinfo *rbrk;	/* atom brk info */
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
typedef struct regex regex_t;
/* regular expression matching state */
struct rstate {
	char *s;		/* the current position in the string */
	int pc;			/* program counter */
};
/* regular expression tree; used for parsing */
struct rnode {
	struct ratom ra;	/* regular expression atom (RN_ATOM) */
	struct rnode *c1, *c2;	/* children */
	int mincnt, maxcnt;	/* number of repetitions */
	int grp;		/* group number */
	int rn;			/* node type (RN_*) */
};
int regcomp(regex_t *re, char *regex, int cflags);
int regexec(regex_t *re, char *str, int nmatch, regmatch_t pmatch[], int eflags);
int regerror(int errcode, regex_t *re, char *errbuf, int errbuf_size);
void regfree(regex_t *re);
/* rset */
#define RE_ICASE		1
#define RE_NOTBOL		2
#define RE_NOTEOL		4
/* regular expression set */
struct rset {
	regex_t regex;		/* the combined regular expression */
	int n;			/* number of regular expressions in this set */
	int *grp;		/* the group assigned to each subgroup */
	int *setgrpcnt;		/* number of groups in each regular expression */
	int grpcnt;		/* group count */
};
struct rset *rset_make(int n, char **pat, int flg);
int rset_find(struct rset *re, char *s, int n, int *grps, int flg);
void rset_free(struct rset *re);
char *re_read(char **src);

/* ren.c rendering lines */
extern int ren_torg;
void ren_done();
int *ren_position(char *s, char ***c, int *n);
int ren_next(char *s, int p, int dir);
int ren_eol(char *s, int dir);
int ren_pos(char *s, int off);
int ren_cursor(char *s, int pos);
int ren_noeol(char *s, int p);
int ren_off(char *s, int pos);
int ren_region(char *s, int c1, int c2, int *l1, int *l2, int closed);
char *ren_translate(char *s, char *ln);
int ren_cwid(char *s, int pos);
/* text direction */
int dir_context(char *s);
void dir_reorder(char *s, int *ord, char **chrs, int n);
void dir_init(void);
void dir_done(void);
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
extern struct bmap *blockmap;
void syn_setft(char *ft);
void syn_blswap(int scdir, int scdiff);
void syn_highlight(int *att, char *s, int n);
char *syn_filetype(char *path);
void syn_context(int att);
int syn_merge(int old, int new);
void syn_reloadft(char *ft, char *injectft, int i, char *reg);
void syn_init(void);
void syn_done(void);

/* uc.c utf-8 helper functions */
int uc_len(char *s);
int uc_wid(char *s, int cp);
int uc_slen(char *s);
int uc_code(char *s);
char *uc_chr(char *s, int off);
int uc_off(char *s, int off);
char *uc_sub(char *s, int beg, int end);
char *uc_dup(char *s);
int uc_isspace(char *s);
int uc_isprint(char *s);
int uc_isdigit(char *s);
int uc_isalpha(char *s);
int uc_kind(char *c);
int uc_isbell(char *c, int cp);
int uc_iscomb(char *c, int cp);
char **uc_chop(char *s, int *n);
char *uc_next(char *s);
char *uc_prev(char *beg, char *s);
char *uc_beg(char *beg, char *s);
char *uc_end(char *s);
char *uc_shape(char *beg, char *s);
char *uc_lastline(char *s);

/* term.c managing the terminal */
#define xrows		(term_rows())
#define xcols		(term_cols())
#define CSI_CLEAR_ALL "\x1b[2J", 4
#define CSI_CLEAR_LINE "\x1b[K", 3
#define CSI_CURSOR_TOP_LEFT "\x1b[H", 3
#define CSI_CURSOR_SHOW "\x1b[?25h", 6
#define CSI_CURSOR_HIDE "\x1b[?25l", 6
#define CSI_SCREEN_ALTERNATIVE "\x1b[?47h", 6
#define CSI_SCREEN_NORMAL "\x1b[?47l", 6
#define CSI_CURSOR_HIDE_TOP_LEFT "\x1b[?25l\x1b[H", 9
void term_init(void);
void term_done(void);
void term_clean(void);
void term_suspend(void);
void term_str(char *s);
void term_chr(int ch);
void term_pos(int r, int c);
void term_clear(void);
void term_kill(void);
void term_room(int n);
int term_rows(void);
int term_cols(void);
int term_read(void);
void term_record(void);
void term_commit(void);
char *term_att(int att, int old);
void term_push(char *s, int n);
char *term_cmd(int *n);
/* process management */
char *cmd_pipe(char *cmd, char *s, int iproc, int oproc);
int cmd_exec(char *cmd);
char* xgetenv(char* q[]); 

#define TK_CTL(x)	((x) & 037)
#define TK_INT(c)	((c) < 0 || (c) == TK_ESC || (c) == TK_CTL('c'))
#define TK_ESC		(TK_CTL('['))

/* led.c line-oriented input and output */
char *led_prompt(char *pref, char *post, char *insert, int *kmap);
char *led_input(char *pref, char *post, int *kmap, int cln);
void led_print(char *msg, int row);
void led_printmsg(char *s, int row);
char *led_read(int *kmap);
char *led_readchar(int c, int kmap);
int led_pos(char *s, int pos);
void led_done();

/* ex.c ex commands */
void hist_set(int i);
void hist_open();
void hist_switch();
void hist_write(char *str);
void hist_done();
char *hist_curstr();
void hist_pos(int row, int off, int top);
void ex(void);
void ex_command(char *cmd);
char *ex_read(char *msg);
void ex_print(char *line);
void ex_show(char *msg);
int ex_init(char **files);
void ex_done(void);
char *ex_path(void);
char *ex_filetype(void);
struct lbuf *ex_lbuf(void);
int ex_kwd(char **kwd, int *dir);
void ex_kwdset(char *kwd, int dir);
int ex_edit(char *path);
void ec_bufferi(int *id);

#define EXLEN	512		/* ex line length */
#define xb 	ex_lbuf()

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
	int att[16];		/* attributes of the matched groups */
	char *pat;		/* regular expression */
	int end;		/* the group ending this pattern */
	int patend;		/* the ending regex for multi-line patterns */
	/* patend is relative index from the parent index */
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
	char *s;	/* the source character */
	char *d;	/* the placeholder */
	int wid;	/* the width of the placeholder */
};
extern struct placeholder placeholders[];
extern int placeholderslen;
int conf_hlrev(void);
int conf_hlline(void);
int conf_mode(void);
char **conf_kmap(int id);
int conf_kmapfind(char *name);
char *conf_digraph(int c1, int c2);

/* vi.c */
char *vi_regget(int c, int *lnmode);
void vi_regput(int c, char *s, int lnmode);
/* file system */
void dir_calc(char *cur_dir);
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
extern int xotd;
extern int xshape;
extern int xorder;
extern int xhl;
extern int xhll;
extern int xhww;
extern int xkmap;
extern int xkmap_alt;
extern int xtabspc;
extern int xqexit;
extern int intershell;
extern char* fslink;
extern char fsincl[128];
extern int fstlen;
extern int fspos;
extern int fscount;
extern int vi_lnnum;
extern int vi_hidch;
extern int vi_mod;
extern int vi_insmov;
