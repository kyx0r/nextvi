int xrow, xoff, xtop;		/* current row, column, and top row */
int xleft;			/* the first visible column */
int xquit;			/* exit if set */
int xvis;			/* visual mode */
int xai = 1;			/* autoindent option */
int xic = 1;			/* ignorecase option */
int xaw;			/* autowrite option */
int xhl = 1;			/* syntax highlight option */
int xhll;			/* highlight current line */
int xhlw;			/* highlight current word */
int xhlp;			/* highlight {} pair */
int xled = 1;			/* use the line editor */
int xtd = +1;			/* current text direction */
int xotd;			/* old text direction */
int xshape = 1;			/* perform letter shaping */
int xorder = 1;			/* change the order of characters */
int xkmap = 0;			/* the current keymap */
int xkmap_alt = 1;		/* the alternate keymap */
int xtabspc = 8;		/* number of spaces for tab */
int xqexit = 1;			/* exit insert via kj */
static rset *xkwdrs;		/* the last searched keyword rset */
static char xrep[EXLEN];	/* the last replacement */
static int xkwddir;		/* the last search direction */
static int xgdep;		/* global command recursion depth */
int intershell;			/* interactive shell */

#define NUM_BUFS 10
static struct buf {
	char ft[32];            /* file type */
	char *path;             /* file path */
	struct lbuf *lb;
	int row, off, top;
	short id, td;           /* buffer id and text direction */
	long mtime;             /* modification time */
} bufs[NUM_BUFS];
static struct buf tempbufs[2];

static int bufs_find(char *path)
{
	int i;
	for (i = 0; i < NUM_BUFS; i++)
		if (bufs[i].lb && !strcmp(bufs[i].path, path))
			return i;
	return -1;
}

static void bufs_free(int idx)
{
	if (bufs[idx].lb) {
		free(bufs[idx].path);
		lbuf_free(bufs[idx].lb);
	}
}

static long mtime(char *path)
{
	struct stat st;
	if (!stat(path, &st))
		return st.st_mtime;
	return -1;
}

static int bufs_open(char *path)
{
	int i;
	for (i = 0; i < NUM_BUFS - 1; i++)
		if (!bufs[i].lb)
			break;
	if (!bufs[i].lb)
		bufs[i].id = i;
	bufs_free(i);
	bufs[i].path = uc_dup(path);
	bufs[i].lb = lbuf_make();
	bufs[i].row = 0;
	bufs[i].off = 0;
	bufs[i].top = 0;
	bufs[i].td = +1;
	bufs[i].mtime = -1;
	strcpy(bufs[i].ft, syn_filetype(path));
	return i;
}

void temp_open(int i, char *name, char *ft)
{
	if (tempbufs[i].lb)
		return;
	tempbufs[i].path = uc_dup(name);
	tempbufs[i].lb = lbuf_make();
	tempbufs[i].row = 0;
	tempbufs[i].off = 0;
	tempbufs[i].top = 0;
	tempbufs[i].td = +1;
	tempbufs[i].mtime = -1;
	strcpy(tempbufs[i].ft, ft);
}

void temp_pos(int i, int row, int off, int top)
{
	tempbufs[i].row = row;
	tempbufs[i].off = off;
	tempbufs[i].top = top;
}

void temp_switch(int i)
{
	struct buf tmp;
	bufs[0].row = xrow;
	bufs[0].off = xoff;
	bufs[0].top = xtop;
	bufs[0].td = xtd;
	memcpy(&tmp, bufs, sizeof(tmp));
	memcpy(bufs, &tempbufs[i], sizeof(tmp));
	memcpy(&tempbufs[i], &tmp, sizeof(tmp));
	xrow = bufs[0].row;
	xoff = bufs[0].off;
	xtop = bufs[0].top;
	xtd = bufs[0].td;
}

void temp_write(int i, char *str)
{
	if (!*str)
		return;
	struct sbuf *sb = sbuf_make(256);
	struct lbuf *lb = tempbufs[i].lb;
	sbuf_str(sb, str);
	if (!lbuf_len(lb))
		lbuf_edit(lb, "\n", 0, 0);
	tempbufs[i].row++;
	lbuf_edit(lb, sbuf_buf(sb), tempbufs[i].row, tempbufs[i].row);
	tempbufs[i].off = lbuf_indents(lb, tempbufs[i].row);
	lbuf_saved(lb, 1);
	sbuf_free(sb);
}

char *temp_curstr(int i, int sub)
{
	return lbuf_get(tempbufs[i].lb, tempbufs[i].row - sub);
}

char *temp_get(int i, int row)
{
	return lbuf_get(tempbufs[i].lb, row);
}

void temp_done(int i)
{
	if (tempbufs[i].lb) {
		free(tempbufs[i].path);
		lbuf_free(tempbufs[i].lb);
		tempbufs[i].lb = NULL;
	}
}

static void bufs_switch(int idx)
{
	struct buf tmp;
	bufs[0].row = xrow;
	bufs[0].off = xoff;
	bufs[0].top = xtop;
	bufs[0].td = xtd;
	memcpy(&tmp, &bufs[idx], sizeof(tmp));
	memmove(&bufs[1], &bufs[0], sizeof(tmp) * idx);
	memcpy(&bufs[0], &tmp, sizeof(tmp));
	xrow = bufs[0].row;
	xoff = bufs[0].off;
	xtop = bufs[0].top;
	xtd = bufs[0].td;
}

char *ex_path(void)
{
	return bufs[0].path;
}

struct lbuf *ex_lbuf(void)
{
	return bufs[0].lb;
}

char *ex_filetype(void)
{
	return bufs[0].ft;
}

/* replace % and # in paths and commands with current and alternate path names */
static char *ex_pathexpand(char *src, int spaceallowed)
{
	struct sbuf *sb = sbuf_make(1024);
	while (*src && (spaceallowed || (*src != ' ' && *src != '\t')))
	{
		if (*src == '%' || *src == '#') {
			int idx = *src == '#';
			if (!bufs[idx].path || !bufs[idx].path[0]) {
				ex_show("pathname \"%\" or \"#\" is not set\n");
				sbuf_free(sb);
				return NULL;
			}
			sbuf_str(sb, bufs[idx].path);
			src++;
		} else {
			if (*src == '\\' && src[1])
				src++;
			sbuf_chr(sb, *src++);
		}
	}
	return sbuf_done(sb);
}

/* the previous search keyword rset */
int ex_krs(rset **krs, int *dir)
{
	if (krs)
		*krs = xkwdrs;
	if (dir)
		*dir = xkwddir;
	return xkwddir == 0 || !xkwdrs;
}

/* set the previous search keyword rset */
void ex_krsset(char *kwd, int dir)
{
	if (kwd) {
		rset_free(xkwdrs);
		xkwdrs = rset_make(1, (char*[]){kwd}, xic ? REG_ICASE : 0);
		vi_regput('/', kwd, 0);
	}
	xkwddir = dir;
}

static int ex_search(char **pat)
{
	struct sbuf *kw;
	char *b = *pat;
	char *e = b;
	rset *re;
	int dir, row;
	kw = sbuf_make(64);
	while (*++e) {
		if (*e == **pat)
			break;
		sbuf_chr(kw, (unsigned char) *e);
		if (*e == '\\' && e[1])
			e++;
	}
	if (sbuf_len(kw))
		ex_krsset(sbuf_buf(kw), **pat == '/' ? 1 : -1);
	sbuf_free(kw);
	*pat = *e ? e + 1 : e;
	if (ex_krs(&re, &dir))
		return -1;
	row = xrow + dir;
	while (row >= 0 && row < lbuf_len(xb)) {
		if (rset_find(re, lbuf_get(xb, row), 0, NULL, 0) >= 0)
			break;
		row += dir;
	}
	return row >= 0 && row < lbuf_len(xb) ? row : -1;
}

static int ex_lineno(char **num)
{
	int n = xrow;
	switch ((unsigned char) **num) {
	case '.':
		*num += 1;
		break;
	case '$':
		n = lbuf_len(xb) - 1;
		*num += 1;
		break;
	case '\'':
		if (lbuf_jump(xb, (unsigned char) *++(*num), &n, NULL))
			return -1;
		*num += 1;
		break;
	case '/':
	case '?':
		n = ex_search(num);
		break;
	default:
		if (isdigit((unsigned char) **num)) {
			n = atoi(*num) - 1;
			while (isdigit((unsigned char) **num))
				*num += 1;
		}
	}
	while (**num == '-' || **num == '+') {
		n += atoi((*num)++);
		while (isdigit((unsigned char) **num))
			(*num)++;
	}
	return n;
}

/* parse ex command addresses */
static int ex_region(char *loc, int *beg, int *end)
{
	int naddr = 0;
	if (!strcmp("%", loc)) {
		*beg = 0;
		*end = MAX(0, lbuf_len(xb));
		return 0;
	}
	if (!*loc) {
		*beg = xrow;
		*end = xrow == lbuf_len(xb) ? xrow : xrow + 1;
		return 0;
	}
	while (*loc) {
		int end0 = *end;
		*end = ex_lineno(&loc) + 1;
		*beg = naddr++ ? end0 - 1 : *end - 1;
		if (!naddr++)
			*beg = *end - 1;
		while (*loc && *loc != ';' && *loc != ',')
			loc++;
		if (!*loc)
			break;
		if (*loc == ';')
			xrow = *end - 1;
		loc++;
	}
	if (*beg < 0 || *beg >= lbuf_len(xb))
		return 1;
	if (*end < *beg || *end > lbuf_len(xb))
		return 1;
	return 0;
}

static int ec_write(char *loc, char *cmd, char *arg);

static int ex_modifiedbuffer(char *msg)
{
	if (!lbuf_modified(xb))
		return 0;
	if (xaw && ex_path()[0])
		return ec_write("", "w", "");
	if (msg)
		ex_show(msg);
	return 1;
}

void ec_bufferi(int *id)
{
	if (*id > NUM_BUFS)
		*id = 0;
	int i;
	for (i = 0; i < NUM_BUFS && bufs[i].lb; i++) {
		if (*id == bufs[i].id)
			break;
	}
	if (i < NUM_BUFS && bufs[i].lb)
		bufs_switch(i);
}

static int ec_buffer(char *loc, char *cmd, char *arg)
{
	int i, id;
	char ln[EXLEN];
	id = arg[0] ? atoi(arg) : -1;
	for (i = 0; i < NUM_BUFS && bufs[i].lb; i++) {
		if (id != -1) {
			if (id == bufs[i].id)
				break;
		} else {
			char c = i < 2 ? "%#"[i] : ' ';
			snprintf(ln, LEN(ln), "%i %c %s",
				(int)bufs[i].id, c, bufs[i].path);
			ex_print(ln);
		}
	}
	if (id != -1) {
		if (i < NUM_BUFS && bufs[i].lb)
			bufs_switch(i);
		else
			ex_show("no such buffer\n");
	}
	return 0;
}

static int ec_quit(char *loc, char *cmd, char *arg)
{
	if (!strchr(cmd, '!'))
		if (ex_modifiedbuffer("buffer modified\n"))
			return 1;
	xquit = 1;
	return 0;
}

#define readfile() \
fd = open(ex_path(), O_RDONLY); \
if (fd >= 0) { \
	int rd = lbuf_rd(xb, fd, 0, lbuf_len(xb)); \
	close(fd); \
	snprintf(msg, sizeof(msg), "\"%s\"  %d lines  [r]\n", \
			ex_path(), lbuf_len(xb)); \
	if (rd) \
		ex_show("read failed\n"); \
	else \
		ex_show(msg); \
} \
lbuf_saved(xb, path[0] != '\0'); \
bufs[0].mtime = mtime(ex_path()); \
xrow = MAX(0, MIN(xrow, lbuf_len(xb) - 1)); \
xoff = 0; \
xtop = MAX(0, MIN(xtop, lbuf_len(xb) - 1)); \

int ex_edit(char *path)
{
	char msg[128];
	int fd;
	if (path[0] == '.' && path[1] == '/')
		path += 2;
	if (path[0] && ((fd = bufs_find(path)) >= 0)) {
		bufs_switch(fd);
		return 1;
	}
	if (path[0] || !bufs[0].path)
		bufs_switch(bufs_open(path));
	readfile()
	return 0;
}

static int ec_edit(char *loc, char *cmd, char *arg)
{
	char msg[128];
	char *path;
	int fd;
	if (!strchr(cmd, '!'))
		if (xb && ex_modifiedbuffer("buffer modified\n"))
			return 1;
	if (!(path = ex_pathexpand(arg, 0)))
		return 1;
	if (path[0] && ((fd = bufs_find(path)) >= 0)) {
		bufs_switch(fd);
		free(path);
		return 0;
	}
	if (path[0] || !bufs[0].path)
		bufs_switch(bufs_open(path));
	readfile()
	free(path);
	return 0;
}

static int ec_editapprox(char *loc, char *cmd, char *arg)
{
	int len, i;
	int inst = 0;
	char *path;
	if (!fslink)
	{
		char path[1024];
		strcpy(path, ".");
		dir_calc(path);
	}
	if (!arg)
		return 0;
	inst = atoi(arg);
	for (int pos = 0; pos < fstlen;)
	{
		path = &fslink[pos+sizeof(int)];
		len = *(int*)((char*)fslink+pos) + sizeof(int);
		pos += len;
		len -= sizeof(int)+2;
		for (i = len; i > 0 && path[i] != '/'; i--){}
		if (!i)
			return 0;
		if (strstr(&path[i+1], arg)) {
			if (!inst) {
				ex_edit(path);
				break;
			}
			inst--;
		}
	}
	return 1;
}

static int ec_read(char *loc, char *cmd, char *arg)
{
	char msg[EXLEN+32];
	int beg, end;
	char *path;
	char *obuf;
	int n = lbuf_len(xb);
	path = arg[0] ? arg : ex_path();
	if (ex_region(loc, &beg, &end))
		return 1;
	if (arg[0] == '!') {
		int pos = MIN(xrow + 1, lbuf_len(xb));
		char *ecmd = ex_pathexpand(arg, 1);
		if (!ecmd)
			return 1;
		obuf = cmd_pipe(arg + 1, NULL, 0, 1);
		if (obuf)
			lbuf_edit(xb, obuf, pos, pos);
		free(obuf);
		free(ecmd);
	} else {
		int fd = open(path, O_RDONLY);
		int pos = lbuf_len(xb) ? end : 0;
		if (fd < 0) {
			ex_show("read failed\n");
			return 1;
		}
		if (lbuf_rd(xb, fd, pos, pos)) {
			ex_show("read failed\n");
			close(fd);
			return 1;
		}
		close(fd);
	}
	xrow = end + lbuf_len(xb) - n - 1;
	snprintf(msg, sizeof(msg), "\"%s\"  %d lines  [r]\n",
			path, lbuf_len(xb) - n);
	ex_show(msg);
	return 0;
}

static int ec_write(char *loc, char *cmd, char *arg)
{
	char msg[EXLEN+32];
	char *path;
	char *ibuf;
	int beg, end;
	path = arg[0] ? arg : ex_path();
	if (cmd[0] == 'x' && !lbuf_modified(xb))
		return ec_quit("", cmd, "");
	if (ex_region(loc, &beg, &end))
		return 1;
	if (!loc[0]) {
		beg = 0;
		end = lbuf_len(xb);
	}
	if (arg[0] == '!') {
		char *ecmd = ex_pathexpand(arg, 1);
		if (!ecmd)
			return 1;
		ibuf = lbuf_cp(xb, beg, end);
		ex_print(NULL);
		cmd_pipe(arg + 1, ibuf, 1, 0);
		free(ecmd);
		free(ibuf);
	} else {
		int fd;
		if (!strchr(cmd, '!') && bufs[0].path &&
				!strcmp(bufs[0].path, path) &&
				mtime(bufs[0].path) > bufs[0].mtime) {
			ex_show("write failed: file changed\n");
			return 1;
		}
		if (!strchr(cmd, '!') && arg[0] && mtime(arg) >= 0) {
			ex_show("write failed: file exists\n");
			return 1;
		}
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, conf_mode());
		if (fd < 0) {
			ex_show("write failed: cannot create file\n");
			return 1;
		}
		if (lbuf_wr(xb, fd, beg, end)) {
			ex_show("write failed\n");
			close(fd);
			return 1;
		}
		close(fd);
	}
	snprintf(msg, sizeof(msg), "\"%s\"  %d lines  [w]\n",
			path, end - beg);
	ex_show(msg);
	if (!ex_path()[0]) {
		free(bufs[0].path);
		bufs[0].path = uc_dup(path);
	}
	if (!strcmp(ex_path(), path))
		lbuf_saved(xb, 0);
	if (!strcmp(ex_path(), path))
		bufs[0].mtime = mtime(path);
	if (cmd[0] == 'x' || (cmd[0] == 'w' && cmd[1] == 'q'))
		ec_quit("", cmd, "");
	return 0;
}

static int ec_insert(char *loc, char *cmd, char *arg)
{
	struct sbuf *sb;
	char *s;
	int beg, end;
	int n;
	if (ex_region(loc, &beg, &end) && (beg != 0 || end != 0))
		return 1;
	sb = sbuf_make(64);
	while ((s = ex_read(""))) {
		if (!strcmp(".", s)) {
			free(s);
			break;
		}
		sbuf_str(sb, s);
		sbuf_chr(sb, '\n');
		free(s);
	}
	if (cmd[0] == 'a')
		if (beg + 1 <= lbuf_len(xb))
			beg++;
	if (cmd[0] != 'c')
		end = beg;
	n = lbuf_len(xb);
	lbuf_edit(xb, sbuf_buf(sb), beg, end);
	xrow = MIN(lbuf_len(xb) - 1, end + lbuf_len(xb) - n - 1);
	sbuf_free(sb);
	return 0;
}

static int ec_print(char *loc, char *cmd, char *arg)
{
	int beg, end;
	int i;
	if (!cmd[0] && !loc[0])
		if (xrow >= lbuf_len(xb))
			return 1;
	if (ex_region(loc, &beg, &end))
		return 1;
	for (i = beg; i < end; i++)
		ex_print(lbuf_get(xb, i));
	xrow = end;
	xoff = 0;
	return 0;
}

static int ec_null(char *loc, char *cmd, char *arg)
{
	int beg, end;
	if (!xvis)
		return ec_print(loc, cmd, arg);
	if (ex_region(loc, &beg, &end))
		return 1;
	xrow = MAX(beg, end - 1);
	xoff = 0;
	return 0;
}

static void ex_yank(int reg, int beg, int end)
{
	char *buf = lbuf_cp(xb, beg, end);
	vi_regput(reg, buf, 1);
	free(buf);
}

static int ec_delete(char *loc, char *cmd, char *arg)
{
	int beg, end;
	if (ex_region(loc, &beg, &end) || !lbuf_len(xb))
		return 1;
	ex_yank(arg[0], beg, end);
	lbuf_edit(xb, NULL, beg, end);
	xrow = beg;
	return 0;
}

static int ec_yank(char *loc, char *cmd, char *arg)
{
	int beg, end;
	if (ex_region(loc, &beg, &end) || !lbuf_len(xb))
		return 1;
	ex_yank(arg[0], beg, end);
	return 0;
}

static int ec_put(char *loc, char *cmd, char *arg)
{
	int beg, end;
	int lnmode;
	char *buf;
	int n = lbuf_len(xb);
	buf = vi_regget(arg[0], &lnmode);
	if (!buf || ex_region(loc, &beg, &end))
		return 1;
	lbuf_edit(xb, buf, end, end);
	xrow = MIN(lbuf_len(xb) - 1, end + lbuf_len(xb) - n - 1);
	return 0;
}

static int ec_lnum(char *loc, char *cmd, char *arg)
{
	char msg[128];
	int beg, end;
	if (ex_region(loc, &beg, &end))
		return 1;
	sprintf(msg, "%d\n", end);
	ex_print(msg);
	return 0;
}

static int ec_undo(char *loc, char *cmd, char *arg)
{
	return lbuf_undo(xb);
}

static int ec_redo(char *loc, char *cmd, char *arg)
{
	return lbuf_redo(xb);
}

static int ec_mark(char *loc, char *cmd, char *arg)
{
	int beg, end;
	if (ex_region(loc, &beg, &end))
		return 1;
	lbuf_mark(xb, arg[0], end - 1, 0);
	return 0;
}

static void replace(struct sbuf *dst, char *rep, char *ln, int *offs)
{
	while (rep[0]) {
		if (rep[0] == '\\' && rep[1]) {
			if (rep[1] >= '0' && rep[1] <= '9') {
				int grp = (rep[1] - '0') * 2;
				int len = offs[grp + 1] - offs[grp];
				sbuf_mem(dst, ln + offs[grp], len);
			} else {
				sbuf_chr(dst, (unsigned char) rep[1]);
			}
			rep++;
		} else {
			sbuf_chr(dst, (unsigned char) rep[0]);
		}
		rep++;
	}
}

static int ec_substitute(char *loc, char *cmd, char *arg)
{
	rset *re;
	int offs[32];
	int beg, end;
	char *pat = NULL, *rep = NULL;
	char *s = arg;
	int i;
	if (ex_region(loc, &beg, &end))
		return 1;
	pat = re_read(&s);
	if (pat && pat[0])
		ex_krsset(pat, +1);
	if (pat && *s) {
		s--;
		rep = re_read(&s);
	}
	if (pat || rep)
		snprintf(xrep, sizeof(xrep), "%s", rep ? rep : "");
	free(pat);
	free(rep);
	if (ex_krs(&re, NULL))
		return 1;
	// if the change is bigger than display size
	// set savepoint where command was issued.
	if (end - beg > xrows)
		lbuf_opt(xb, NULL, xrow, 0);
	for (i = beg; i < end; i++) {
		char *ln = lbuf_get(xb, i);
		struct sbuf *r = NULL;
		while (rset_find(re, ln, LEN(offs) / 2, offs, 0) >= 0) {
			if (!r)
				r = sbuf_make(offs[0]+offs[1]+1);
			sbuf_mem(r, ln, offs[0]);
			replace(r, xrep, ln, offs);
			ln += offs[1];
			if (!*ln || !strchr(s, 'g'))
				break;
			if (offs[1] <= 0)	/* zero-length match */
				sbuf_chr(r, (unsigned char) *ln++);
		}
		if (r)
		{
			sbuf_str(r, ln);
			lbuf_edit(xb, sbuf_buf(r), i, i + 1);
			sbuf_free(r);
		}
	}
	if (end - beg > xrows)
		lbuf_opt(xb, NULL, xrow, 0);
	return 0;
}

static int ec_exec(char *loc, char *cmd, char *arg)
{
	int beg, end;
	char *text, *rep, *ecmd;
	ex_modifiedbuffer(NULL);
	if (!(ecmd = ex_pathexpand(arg, 1)))
		return 1;
	if (!loc[0]) {
		int ret;
		ex_print(NULL);
		ret = cmd_exec(ecmd);
		free(ecmd);
		return ret;
	}
	if (ex_region(loc, &beg, &end)) {
		free(ecmd);
		return 1;
	}
	text = lbuf_cp(xb, beg, end);
	rep = cmd_pipe(ecmd, text, 1, 1);
	if (rep)
		lbuf_edit(xb, rep, beg, end);
	free(ecmd);
	free(text);
	free(rep);
	return 0;
}

static int ec_exec_inter(char *loc, char *cmd, char *arg)
{
	intershell = 1;
	return ec_exec(loc, cmd, arg);
}

static int ec_make(char *loc, char *cmd, char *arg)
{
	char make[EXLEN];
	char *target;
	ex_modifiedbuffer(NULL);
	if (!(target = ex_pathexpand(arg, 0)))
		return 1;
	sprintf(make, "make %s", target);
	free(target);
	ex_print(NULL);
	if (cmd_exec(make))
		return 1;
	return 0;
}

static int ec_ft(char *loc, char *cmd, char *arg)
{
	if (arg[0])
		snprintf(bufs[0].ft, sizeof(bufs[0].ft), "%s", arg);
	else
		ex_print(ex_filetype());
	return 0;
}

static int ec_cmap(char *loc, char *cmd, char *arg)
{
	if (arg[0])
		xkmap_alt = conf_kmapfind(arg);
	else
		ex_print(conf_kmap(xkmap)[0]);
	if (arg[0] && !strchr(cmd, '!'))
		xkmap = xkmap_alt;
	return 0;
}

static int ex_exec(char *ln);

static int ec_glob(char *loc, char *cmd, char *arg)
{
	rset *re;
	int offs[32];
	int beg, end, not;
	char *pat, *s = arg;
	int i;
	if (!loc[0] && !xgdep)
		strcpy(loc, "%");
	if (ex_region(loc, &beg, &end))
		return 1;
	not = strchr(cmd, '!') || cmd[0] == 'v';
	pat = re_read(&s);
	if (pat && pat[0])
		ex_krsset(pat, +1);
	free(pat);
	if (ex_krs(&re, NULL))
		return 1;
	xgdep++;
	for (i = beg + 1; i < end; i++)
		lbuf_globset(xb, i, xgdep);
	i = beg;
	while (i < lbuf_len(xb)) {
		char *ln = lbuf_get(xb, i);
		if ((rset_find(re, ln, LEN(offs) / 2, offs, 0) < 0) == not) {
			xrow = i;
			if (ex_exec(s))
				break;
			i = MIN(i, xrow);
		}
		while (i < lbuf_len(xb) && !lbuf_globget(xb, i, xgdep))
			i++;
	}
	for (i = 0; i < lbuf_len(xb); i++)
		lbuf_globget(xb, i, xgdep);
	xgdep--;
	return 0;
}

static struct option {
	char *abbr;
	char *name;
	int *var;
} options[] = {
	{"ai", "autoindent", &xai},
	{"aw", "autowrite", &xaw},
	{"ic", "ignorecase", &xic},
	{"td", "textdirection", &xtd},
	{"shape", "shape", &xshape},
	{"order", "xorder", &xorder},
	{"hl", "highlight", &xhl},
	{"hll", "highlightline", &xhll},
	{"hlw", "highlightword", &xhlw},
	{"hlp", "highlightpair", &xhlp},
	{"tbs", "tabspace", &xtabspc},
	{"qe", "quickexit", &xqexit},
};

static char *cutword(char *s, char *d)
{
	while (isspace(*s))
		s++;
	while (*s && !isspace(*s))
		*d++ = *s++;
	while (isspace(*s))
		s++;
	*d = '\0';
	return s;
}

static int ec_set(char *loc, char *cmd, char *arg)
{
	char tok[EXLEN];
	char opt[EXLEN];
	char *s = arg;
	int val = 0;
	int i;
	if (*s) {
		s = cutword(s, tok);
		/* if prefix "no" before option */
		if (tok[0] == 'n' && tok[1] == 'o') {
			strcpy(opt, tok + 2);
			val = 0;
		} else {
			char *r = strchr(tok, '=');
			if (r) {
				*r = '\0';
				strcpy(opt, tok);
				val = atoi(r + 1);
			} else {
				strcpy(opt, tok);
				val = 1;
			}
		}
		for (i = 0; i < LEN(options); i++) {
			struct option *o = &options[i];
			if (!strcmp(o->abbr, opt) || !strcmp(o->name, opt)) {
				*o->var = val;
				return 0;
			}
		}
		ex_show("unknown option");
		return 1;
	}
	return 0;
}

static int ec_setdir(char *loc, char *cmd, char *arg)
{
	if (*arg) {
		if (fslink) {
			free(fslink);
			fslink = NULL;
			fstlen = 0;
			fspos = 0;
			fscount = 0;
		}
		dir_calc(arg);
	}
	return 0;
}

static int ec_chdir(char *loc, char *cmd, char *arg)
{
	if (*arg)
		if (chdir(arg))
			ex_show("chdir error");
	return 0;
}

static int ec_setincl(char *loc, char *cmd, char *arg)
{
	if (fslink) {
		free(fslink);
		fslink = NULL;
		fstlen = 0;
		fspos = 0;
		fscount = 0;
	}
	rset_free(fsincl);
	if (!*arg)
		fsincl = NULL;
	else
		fsincl = rset_make(1, (char*[]){arg}, xic ? REG_ICASE : 0);
	return 0;
}

static struct excmd {
	char *abbr;
	char *name;
	int (*ec)(char *loc, char *cmd, char *arg);
} excmds[] = {
	{"b", "buffer", ec_buffer},
	{"p", "print", ec_print},
	{"a", "append", ec_insert},
	{"ea", "ea", ec_editapprox},
	{"i", "insert", ec_insert},
	{"d", "delete", ec_delete},
	{"c", "change", ec_insert},
	{"e", "edit", ec_edit},
	{"e!", "edit!", ec_edit},
	{"g", "global", ec_glob},
	{"g!", "global!", ec_glob},
	{"=", "=", ec_lnum},
	{"k", "mark", ec_mark},
	{"pu", "put", ec_put},
	{"q", "quit", ec_quit},
	{"q!", "quit!", ec_quit},
	{"r", "read", ec_read},
	{"v", "vglobal", ec_glob},
	{"w", "write", ec_write},
	{"w!", "write!", ec_write},
	{"wq", "wq", ec_write},
	{"wq!", "wq!", ec_write},
	{"u", "undo", ec_undo},
	{"rd", "redo", ec_redo},
	{"se", "set", ec_set},
	{"s", "substitute", ec_substitute},
	{"x", "xit", ec_write},
	{"x!", "xit!", ec_write},
	{"ya", "yank", ec_yank},
	{"z", "z", ec_exec_inter},
	{"!", "!", ec_exec},
	{"make", "make", ec_make},
	{"ft", "filetype", ec_ft},
	{"cm", "cmap", ec_cmap},
	{"cm!", "cmap!", ec_cmap},
	{"fd", "fsdir", ec_setdir},
	{"cd", "chdir", ec_chdir},
	{"inc", "inc", ec_setincl},
	{"", "", ec_null},
};

static int ex_idx(char *cmd)
{
	int i;
	for (i = 0; i < LEN(excmds); i++)
		if (!strcmp(excmds[i].abbr, cmd) || !strcmp(excmds[i].name, cmd))
			return i;
	return -1;
}

/* read ex command addresses */
static char *ex_loc(char *src, char *loc)
{
	while (*src == ':' || *src == ' ' || *src == '\t')
		src++;
	while (*src && !isalpha((unsigned char) *src) && *src != '=' && *src != '!')
	{
		if (*src == '\'')
			*loc++ = *src++;
		if (*src == '/' || *src == '?') {
			int d = *src;
			*loc++ = *src++;
			while (*src && *src != d) {
				if (*src == '\\' && src[1])
					*loc++ = *src++;
				*loc++ = *src++;
			}
		}
		if (*src)
			*loc++ = *src++;
	}
	*loc = '\0';
	return src;
}

/* read ex command name */
static char *ex_cmd(char *src, char *cmd)
{
	char *cmd0 = cmd;
	while (*src == ' ' || *src == '\t')
		src++;
	while (isalpha((unsigned char) *src) && cmd < cmd0 + 16)
		if ((*cmd++ = *src++) == 'k' && cmd == cmd0 + 1)
			break;
	if (*src == '!' || *src == '=')
		*cmd++ = *src++;
	*cmd = '\0';
	return src;
}

/* read ex command argument for excmd command */
static char *ex_arg(char *src, char *dst)
{
	while (*src == ' ' || *src == '\t')
		src++;
	while (*src && (*src != '|' || src[-1] == '\\'))
	{
		if (!strncmp(src, "\\\\|", 3))
			src += 2;
		*dst++ = *src++;
	}
	*dst = '\0';
	return src;
}

/* execute a single ex command */
static int ex_exec(char *ln)
{
	char loc[EXLEN], cmd[EXLEN], arg[EXLEN];
	int ret = 0;
	if (strlen(ln) >= EXLEN) {
		ex_show("command too long");
		return 1;
	}
	while (*ln) {
		int idx;
		ln = ex_loc(ln, loc);
		ln = ex_cmd(ln, cmd);
		idx = ex_idx(cmd);
		ln = ex_arg(ln, arg);
		if (idx >= 0)
			ret = excmds[idx].ec(loc, cmd, arg);
	}
	return ret;
}

/* execute a single ex command */
void ex_command(char *ln)
{
	ex_exec(ln);
	lbuf_modified(xb);
	vi_regput(':', ln, 0);
}

/* ex main loop */
void ex(void)
{
	while (!xquit) {
		char *ln = ex_read(":");
		if (ln)
			ex_command(ln);
		free(ln);
	}
}

int ex_init(char **files)
{
	char arg[EXLEN];
	char *s = arg;
	char *r = files[0] ? files[0] : "";
	while (*r && s + 2 < arg + sizeof(arg)) {
		if (*r == ' ' || *r == '%' || *r == '#')
			*s++ = '\\';
		*s++ = *r++;
	}
	*s = '\0';
	if (ec_edit("", "e", arg))
		return 1;
	if (getenv("EXINIT"))
		ex_command(getenv("EXINIT"));
	return 0;
}

void ex_done(void)
{
	for (int i = 0; i < NUM_BUFS; i++)
		bufs_free(i);
	rset_free(xkwdrs);
}
