int xrow, xoff, xtop;		/* current row, column, and top row */
int xleft;			/* the first visible column */
int xquit;			/* exit if positive, force quit if negative */
int xvis;			/* visual mode */
int xai = 1;			/* autoindent option */
int xic = 1;			/* ignorecase option */
int xhl = 1;			/* syntax highlight option */
int xhll;			/* highlight current line */
int xhlw;			/* highlight current word */
int xhlp;			/* highlight {}[]() pair */
int xhlr;			/* highlight text in reverse direction */
int xled = 1;			/* use the line editor */
int xtd = +1;			/* current text direction */
int xshape = 1;			/* perform letter shaping */
int xorder = 1;			/* change the order of characters */
int xkmap;			/* the current keymap */
int xkmap_alt = 1;		/* the alternate keymap */
int xtabspc = 8;		/* number of spaces for tab */
int xish;			/* interactive shell */
int xgrp;			/* regex search group */
int xpac;			/* print autocomplete options */
int xkwdcnt;			/* number of search kwd changes */
int xbufcur;			/* number of active buffers */
int xgrec;			/* global vi/ex recursion depth */
struct buf *bufs;		/* main buffers */
struct buf tempbufs[2];		/* temporary buffers, for internal use */
struct buf *ex_buf;		/* current buffer */
struct buf *ex_pbuf;		/* prev buffer */
static struct buf *ex_tpbuf;	/* temp prev buffer */
sbuf *xacreg;			/* autocomplete db filter regex */
rset *xkwdrs;			/* the last searched keyword rset */
int xkwddir;			/* the last search direction */
int xmpt;			/* whether to prompt after printing > 1 lines in vi */
int xpr;			/* ex_cprint register */
int xsep = ':';			/* ex command separator */
char *xregs[256];		/* string registers */
static int xbufsmax;		/* number of buffers */
static int xbufsalloc = 10;	/* initial number of buffers */
static char xrep[EXLEN];	/* the last replacement */
static int xgdep;		/* global command recursion depth */

static int rstrcmp(const char *s1, const char *s2, int l1, int l2)
{
	if (l1 != l2 || !l1)
		return 1;
	for (int i = l1-1; i >= 0; i--)
		if (s1[i] != s2[i])
			return 1;
	return 0;
}

static int bufs_find(const char *path, int len)
{
	for (int i = 0; i < xbufcur; i++)
		if (!rstrcmp(bufs[i].path, path, bufs[i].plen, len))
			return i;
	return -1;
}

static void bufs_free(int idx)
{
	free(bufs[idx].path);
	lbuf_free(bufs[idx].lb);
}

static long mtime(char *path)
{
	struct stat st;
	if (!stat(path, &st))
		return st.st_mtime;
	return -1;
}

void bufs_switch(int idx)
{
	if (ex_buf != &bufs[idx]) {
		exbuf_save(ex_buf)
		if (istempbuf(ex_buf))
			ex_pbuf = &bufs[idx] == ex_pbuf ? ex_tpbuf : ex_pbuf;
		else
			ex_pbuf = ex_buf;
		ex_buf = &bufs[idx];
	}
	exbuf_load(ex_buf)
}

static int bufs_open(const char *path, int len)
{
	int i = xbufcur;
	if (i <= xbufsmax - 1)
		xbufcur++;
	else
		bufs_free(--i);
	bufs[i].path = uc_dup(path);
	bufs[i].lb = lbuf_make();
	bufs[i].plen = len;
	bufs[i].row = 0;
	bufs[i].off = 0;
	bufs[i].top = 0;
	bufs[i].td = +1;
	bufs[i].mtime = -1;
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
	tempbufs[i].ft = ft;
}

void temp_pos(int i, int row, int off, int top)
{
	if (row < 0)
		row = lbuf_len(tempbufs[i].lb)-1;
	tempbufs[i].row = row < 0 ? 0 : row;
	tempbufs[i].off = off;
	tempbufs[i].top = top;
}

void temp_switch(int i)
{
	if (ex_buf == &tempbufs[i]) {
		exbuf_save(ex_buf)
		ex_buf = ex_pbuf;
		ex_pbuf = ex_tpbuf;
	} else {
		if (!istempbuf(ex_buf)) {
			ex_tpbuf = ex_pbuf;
			ex_pbuf = ex_buf;
		}
		exbuf_save(ex_buf)
		ex_buf = &tempbufs[i];
	}
	exbuf_load(ex_buf)
	syn_setft(ex_ft);
}

void temp_write(int i, char *str)
{
	if (!*str)
		return;
	struct lbuf *lb = tempbufs[i].lb;
	if (lbuf_get(lb, tempbufs[i].row))
		tempbufs[i].row++;
	lbuf_edit(lb, str, tempbufs[i].row, tempbufs[i].row);
}

/* replace % and # with buffer names and !..! with command output */
static char *ex_pathexpand(char *src)
{
	sbuf_smake(sb, 1024)
	while (*src) {
		if (*src == '#' || *src == '%') {
			int n = -1;
			struct buf *pbuf = *src == '%' ? ex_buf : ex_pbuf;
			if ((src[1] ^ '0') < 10)
				pbuf = &bufs[n = atoi(&src[1])];
			if (pbuf >= &bufs[xbufcur] || !pbuf->path[0]) {
				ex_print("\"#\" or \"%\" is not set");
				free(sb->s);
				return NULL;
			}
			sbuf_str(sb, pbuf->path)
			src += n >= 0 ? snprintf(0, 0, "%+d", n) : 1;
		} else if (*src == '!') {
			int n = sb->s_n;
			src++;
			while (*src && *src != '!') {
				if (*src == '\\' && src[1] == '!')
					src++;
				sbuf_chr(sb, *src++)
			}
			src += *src ? 1 : 0;
			sbuf_null(sb)
			char *str = cmd_pipe(sb->s + n, NULL, 1);
			sbuf_cut(sb, n)
			if (str)
				sbuf_str(sb, str)
			free(str);
		} else {
			if (*src == '\\' &&
				(src[1] == '#' || src[1] == '%' || src[1] == '!'))
				src++;
			sbuf_chr(sb, *src++)
		}
	}
	sbufn_sret(sb)
}

/* set the current search keyword rset if the kwd or flags changed */
void ex_krsset(char *kwd, int dir)
{
	char *reg = xregs['/'];
	if (kwd && *kwd && ((!reg || !xkwdrs || strcmp(kwd, reg))
		|| ((xkwdrs->regex->flg & REG_ICASE) != xic))) {
		rset_free(xkwdrs);
		xkwdrs = rset_smake(kwd, xic ? REG_ICASE : 0);
		xkwdcnt++;
		vi_regputraw('/', kwd, 0, 0);
		xkwddir = dir;
	}
	if (dir == -2 || dir == 2)
		xkwddir = dir / 2;
}

static int ec_search(char *loc, char *cmd, char *arg);

static int ex_range(char **num, int n, int *row)
{
	switch ((unsigned char) **num) {
	case '.':
		++*num;
		break;
	case '$':
		n = row ? lbuf_eol(xb, *row) : lbuf_len(xb) - 1;
		++*num;
		break;
	case '\'':
		if (lbuf_jump(xb, (unsigned char) *++(*num),
				&n, row ? &n : NULL))
			return -1;
		++*num;
		break;
	case '/':
	case '?':
		n = ec_search(NULL, (char*)row, (char*)num);
		break;
	default:
		if (isdigit((unsigned char) **num)) {
			n = atoi(*num) - !row;
			while (isdigit((unsigned char) **num))
				++*num;
		}
	}
	while (**num == '-' || **num == '+') {
		n += atoi((*num)++);
		while (isdigit((unsigned char) **num))
			++*num;
	}
	return n;
}

/* parse ex command addresses */
#define ex_region(loc, beg, end) ex_oregion(loc, beg, end, NULL, NULL)
static int ex_oregion(char *loc, int *beg, int *end, int *o1, int *o2)
{
	int naddr = 0;
	if (!strcmp("%", loc) || !lbuf_len(xb)) {
		*beg = 0;
		*end = MAX(0, lbuf_len(xb));
		return 0;
	}
	if (!*loc) {
		*beg = xrow;
		*end = MIN(lbuf_len(xb), xrow + 1);
		return 0;
	}
	while (*loc) {
		int end0 = *end;
		if (*loc == ';' || *loc == ',') {
			loc++;
			if (loc[-1] == ',')
				goto skip;
			if (o1 && *o2 >= 0)
				*o1 = *o2;
			xoff = ex_range(&loc, xoff, naddr ? beg : &xrow);
			if (o2)
				*o2 = xoff;
		} else {
			skip:
			*end = ex_range(&loc, xrow, NULL) + 1;
			*beg = naddr++ ? end0 - 1 : *end - 1;
		}
		while (*loc && *loc != ';' && *loc != ',')
			loc++;
	}
	if (*beg < 0 && *end == 0)
		*beg = 0;
	if (*beg < 0 || *beg >= lbuf_len(xb))
		return 1;
	if (*end < *beg || *end > lbuf_len(xb))
		return 1;
	return 0;
}

static int ec_search(char *loc, char *cmd, char *arg)
{
	int dir, off, obeg, beg = -1, end = lbuf_len(xb);
	char **re = !loc ? (char**)arg : &arg;
	dir = **re == '/' ? 2 : -2;
	char *e = re_read(re);
	if (!e)
		return -1;
	ex_krsset(e, dir);
	free(e);
	if (!xkwdrs)
		return -1;
	if (!loc) {
		beg = cmd ? *(int*)cmd : xrow + (xkwddir > 0);
		off = cmd ? xoff : 0;
		if (lbuf_search(xb, xkwdrs, xkwddir, &beg,
				&off, end, MIN(dir, 0)))
			return -1;
	} else if (!ex_region(loc, &beg, &end)) {
		off = xoff;
		obeg = beg;
		if (xrow < beg || xrow > end) {
			off = 0;
			beg = xkwddir > 0 ? beg : end++;
		} else
			beg = xrow;
		if (lbuf_search(xb, xkwdrs, xkwddir, &beg,
				&off, end, xkwddir))
			return -1;
		if (beg < obeg)
			return -1;
		xrow = beg;
		xoff = off;
	}
	return cmd ? off : beg;
}

static int ec_buffer(char *loc, char *cmd, char *arg)
{
	if (!arg[0]) {
		char ln[EXLEN];
		for (int i = 0; i < xbufcur; i++) {
			char c = ex_buf == bufs+i ? '%' : ' ';
			c = ex_pbuf == bufs+i ? '#' : c;
			snprintf(ln, LEN(ln), "%d %c %s", i,
				c + (char)lbuf_modified(bufs[i].lb), bufs[i].path);
			ex_print(ln);
		}
	} else if (atoi(arg) < 0) {
		if (abs(atoi(arg)) <= LEN(tempbufs))
			temp_switch(abs(atoi(arg))-1);
		else
			ex_print("no such buffer");
	} else if (atoi(arg) < xbufcur) {
		bufs_switchwft(atoi(arg))
	} else
		ex_print("no such buffer");
	return 0;
}

static int ec_quit(char *loc, char *cmd, char *arg)
{
	for (int i = 0; !strchr(cmd, '!') && i < xbufcur; i++)
		if ((xquit < 0 || xgrec < 2) && lbuf_modified(bufs[i].lb)) {
			ex_print("buffers modified");
			return 1;
		}
	if (!xquit)
		xquit = !strchr(cmd, '!') ? 1 : -1;
	return 0;
}

void ex_bufpostfix(struct buf *p, int clear)
{
	p->mtime = mtime(p->path);
	p->ft = syn_filetype(p->path);
	lbuf_saved(p->lb, clear);
}

#define readfile(errchk, init) \
fd = open(ex_path, O_RDONLY); \
if (fd >= 0) { \
	errchk lbuf_rd(xb, fd, 0, lbuf_len(xb), init); \
	close(fd); \
} \

int ex_edit(const char *path, int len)
{
	int fd;
	if (path[0] == '.' && path[1] == '/') {
		path += 2;
		len -= 2;
	}
	if (path[0] && ((fd = bufs_find(path, len)) >= 0)) {
		bufs_switch(fd);
		return 1;
	}
	bufs_switch(bufs_open(path, len));
	readfile(/**/, 1)
	return 0;
}

static int ec_edit(char *loc, char *cmd, char *arg)
{
	char msg[128];
	int fd, len, rd = 0, cd = 0;
	if (arg[0] == '.' && arg[1] == '/')
		cd = 2;
	len = strlen(arg+cd);
	if (len && ((fd = bufs_find(arg+cd, len)) >= 0)) {
		bufs_switchwft(fd)
		return 0;
	} else if (xbufcur == xbufsmax && !strchr(cmd, '!') &&
			lbuf_modified(bufs[xbufsmax - 1].lb)) {
		ex_print("last buffer modified");
		return 1;
	} else if (len || !xbufcur || !strchr(cmd, '!')) {
		bufs_switch(bufs_open(arg+cd, len));
		cd = 3; /* XXX: quick hack to indicate new lbuf */
	}
	readfile(rd =, cd == 3)
	if (cd == 3 || (!rd && fd >= 0)) {
		ex_bufpostfix(ex_buf, arg[0]);
		syn_setft(ex_ft);
	}
	snprintf(msg, sizeof(msg), "\"%s\" %dL [%c]",
			*ex_path ? ex_path : "unnamed", lbuf_len(xb),
			fd < 0 || rd ? 'f' : 'r');
	if (!(xvis & 8))
		ex_print(msg);
	return fd < 0 || rd;
}

static int ec_editapprox(char *loc, char *cmd, char *arg)
{
	sbuf_smake(sb, 128)
	char ln[EXLEN];
	char *path, *arg1 = arg+dstrlen(arg, ' ');
	struct lbuf *lb = tempbufs[1].lb;
	int c = 0, i, inst = *arg1 ? atoi(arg1) : -1;
	*arg1 = '\0';
	for (int pos = 0; pos < lbuf_len(lb); pos++) {
		path = lb->ln[pos];
		for (i = lbuf_s(path)->len; i > 0 && path[i] != '/'; i--);
		if (!i)
			continue;
		if (strstr(&path[i+1], arg)) {
			sbuf_mem(sb, &path, (int)sizeof(path))
			snprintf(ln, LEN(ln), "%d %s", c++, path);
			ex_print(ln);
		}
	}
	if (inst < 0 && c > 1)
		inst = term_read() - '0';
	if ((inst >= 0 && inst < c) || c == 1) {
		path = *((char**)sb->s + (c == 1 ? 0 : inst));
		path[lbuf_s(path)->len] = '\0';
		ec_edit(loc, cmd, path);
		path[lbuf_s(path)->len] = '\n';
	}
	xmpt = xmpt >= 0 ? 0 : xmpt;
	free(sb->s);
	return 0;
}

static int ec_setpath(char *loc, char *cmd, char *arg)
{
	free(ex_path);
	ex_path = uc_dup(arg);
	ex_buf->plen = strlen(arg);
	return 0;
}

static int ec_read(char *loc, char *cmd, char *arg)
{
	char msg[EXLEN+32];
	int beg, end, fd = -1;
	char *path;
	char *obuf;
	int n = lbuf_len(xb);
	int pos = MIN(xrow + 1, lbuf_len(xb));
	struct lbuf *lb = lbuf_make(), *pxb = xb;
	path = arg[0] ? arg : ex_path;
	if (arg[0] == '!') {
		obuf = cmd_pipe(arg + 1, NULL, 1);
		if (obuf)
			lbuf_edit(lb, obuf, 0, 0);
		free(obuf);
	} else {
		if ((fd = open(path, O_RDONLY)) < 0) {
			strcpy(msg, "open failed");
			goto err;
		}
		if (lbuf_rd(lb, fd, 0, 0, 0)) {
			strcpy(msg, "read failed");
			goto err;
		}
	}
	xb = lb;
	xrow = 0;
	if (ex_region(loc, &beg, &end)) {
		strcpy(msg, "bad region");
		goto err;
	}
	obuf = lbuf_cp(lb, beg, end);
	if (*obuf)
		lbuf_edit(pxb, obuf, pos, pos);
	snprintf(msg, sizeof(msg), "\"%s\" %dL [r]",
			path, lbuf_len(pxb) - n);
	free(obuf);
	err:
	lbuf_free(lb);
	xrow = pos;
	xb = pxb;
	if (fd >= 0)
		close(fd);
	ex_print(msg);
	return 0;
}

static int ex_pipeout(char *cmd, char *buf)
{
	if (!(xvis & 4)) {
		term_chr('\n');
		xmpt = xmpt >= 0 ? 2 : xmpt;
	}
	return !!cmd_pipe(cmd, buf, 0);
}

static int ec_write(char *loc, char *cmd, char *arg)
{
	char msg[EXLEN+32];
	char *path;
	char *ibuf;
	int beg, end;
	path = arg[0] ? arg : ex_path;
	if (cmd[0] == 'x' && !lbuf_modified(xb))
		return ec_quit("", cmd, "");
	if (ex_region(loc, &beg, &end))
		return 1;
	if (!loc[0]) {
		beg = 0;
		end = lbuf_len(xb);
	}
	if (arg[0] == '!') {
		ibuf = lbuf_cp(xb, beg, end);
		ex_pipeout(arg + 1, ibuf);
		free(ibuf);
	} else {
		int fd;
		if (!strchr(cmd, '!')) {
			if (!strcmp(ex_path, path) && mtime(path) > ex_buf->mtime) {
				ex_print("write failed: file changed");
				return 1;
			}
			if (arg[0] && mtime(path) >= 0) {
				ex_print("write failed: file exists");
				return 1;
			}
		}
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, conf_mode);
		if (fd < 0) {
			ex_print("write failed: cannot create file");
			return 1;
		}
		if (lbuf_wr(xb, fd, beg, end)) {
			ex_print("write failed");
			close(fd);
			return 1;
		}
		close(fd);
		snprintf(msg, sizeof(msg), "\"%s\" %dL [w]",
				path, end - beg);
		ex_print(msg);
	}
	if (strcmp(ex_path, path))
		ec_setpath(NULL, NULL, path);
	lbuf_saved(xb, 0);
	ex_buf->mtime = mtime(path);
	if (cmd[0] == 'x' || (cmd[0] == 'w' && cmd[1] == 'q'))
		ec_quit("", cmd, "");
	return 0;
}

static int ec_termexec(char *loc, char *cmd, char *arg)
{
	if (*arg)
		term_exec(arg, strlen(arg), cmd[0])
	return 0;
}

static int ec_insert(char *loc, char *cmd, char *arg)
{
	sbuf *sb;
	char *s;
	int beg, end;
	int n;
	if (ex_region(loc, &beg, &end))
		return 1;
	sbufn_make(sb, 64)
	if (*arg)
		term_push(arg, strlen(arg));
	while ((s = ex_read(NULL))) {
		if (xvis & 2 && !strcmp(".", s))
			break;
		sbuf_str(sb, s)
		sbufn_chr(sb, '\n')
		free(s);
	}
	free(s);
	if (cmd[0] == 'a' && (beg + 1 <= lbuf_len(xb)))
		beg++;
	if (cmd[0] != 'c')
		end = beg;
	if (vi_insmov != TK_CTL('c')) {
		n = lbuf_len(xb);
		lbuf_edit(xb, sb->s, beg, end);
		xrow = MIN(lbuf_len(xb) - 1, end + lbuf_len(xb) - n - 1);
	} else
		vi_insmov = 0;
	sbuf_free(sb)
	return 0;
}

static int ec_print(char *loc, char *cmd, char *arg)
{
	int i, beg, end, o1 = -1, o2 = -1;
	char *o;
	if (!cmd[0] && !loc[0] && arg[0]) {
		ex_print("unknown command");
		return 1;
	}
	if (ex_oregion(loc, &beg, &end, &o1, &o2))
		return 1;
	if (!cmd[0] && loc[0]) {
		xrow = MAX(beg, end - 1);
		return 0;
	}
	for (i = beg; i < end; i++) {
		o = NULL;
		if (o1 >= 0 && o2 >= 0) {
			if (beg == end-1)
				o = uc_sub(lbuf_get(xb, i), o1, o2);
			else if (i == beg)
				o = uc_sub(lbuf_get(xb, i), o1, -1);
			else if (i == end-1)
				o = uc_sub(lbuf_get(xb, i), 0, o2);
		}
		ex_print(o ? o : lbuf_get(xb, i));
		free(o);
	}
	xrow = MAX(beg, end - (cmd[0] || loc[0]));
	return 0;
}

static int ec_delete(char *loc, char *cmd, char *arg)
{
	int beg, end;
	if (ex_region(loc, &beg, &end) || !lbuf_len(xb))
		return 1;
	char *buf = lbuf_cp(xb, beg, end);
	vi_regput((unsigned char) arg[0], buf, 1);
	free(buf);
	lbuf_edit(xb, NULL, beg, end);
	xrow = beg;
	return 0;
}

static int ec_yank(char *loc, char *cmd, char *arg)
{
	int beg, end;
	if (cmd[2] == '!') {
		vi_regputraw(arg[0], "", 0, 0);
		return 0;
	}
	if (ex_region(loc, &beg, &end) || !lbuf_len(xb))
		return 1;
	char *buf = lbuf_cp(xb, beg, end);
	vi_regputraw(arg[0], buf, 1, isupper((unsigned char) arg[0]) || arg[1]);
	free(buf);
	return 0;
}

static int ec_put(char *loc, char *cmd, char *arg)
{
	int beg, end, i = 0;
	char *buf;
	int n = lbuf_len(xb);
	if (arg[i] == '!' && arg[i+1] && arg[i+1] != ' ')
		buf = xregs[0];
	else
		buf = xregs[(unsigned char) arg[i++]];
	if (!buf || ex_region(loc, &beg, &end))
		return 1;
	for (; arg[i] && arg[i] != '!'; i++){}
	if (arg[i] == '!' && arg[i+1])
		return ex_pipeout(arg + i + 1, buf);
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
	sprintf(msg, "%d", end);
	ex_print(msg);
	return 0;
}

static int ec_undoredo(char *loc, char *cmd, char *arg)
{
	int n = !arg[0] ? 1 : atoi(arg);
	if (arg[0] == '$')
		n = -1;
	for (int ret = 0; n && !ret; n--)
		ret = cmd[0] == 'u' ? lbuf_undo(xb) : lbuf_redo(xb);
	return 0;
}

static int ec_save(char *loc, char *cmd, char *arg)
{
	lbuf_saved(xb, *arg);
	return 0;
}

static int ec_mark(char *loc, char *cmd, char *arg)
{
	int beg, end;
	if (ex_region(loc, &beg, &end))
		return 1;
	lbuf_mark(xb, (unsigned char) arg[0], end - 1, xoff);
	return 0;
}

static void replace(sbuf *dst, char *rep, char *ln, int *offs)
{
	while (rep[0]) {
		if (rep[0] == '\\' && rep[1]) {
			if (rep[1] >= '0' && rep[1] <= '9') {
				int grp = (rep[1] - '0') * 2;
				int len = offs[grp + 1] - offs[grp];
				sbuf_mem(dst, ln + offs[grp], len)
			} else
				sbuf_chr(dst, (unsigned char) rep[1])
			rep++;
		} else
			sbuf_chr(dst, (unsigned char) rep[0])
		rep++;
	}
}

static int ec_substitute(char *loc, char *cmd, char *arg)
{
	int beg, end;
	char *pat = NULL, *rep = NULL;
	char *s = arg;
	int i, first = -1, last;
	if (ex_region(loc, &beg, &end))
		return 1;
	pat = re_read(&s);
	ex_krsset(pat, +1);
	if (pat && *s) {
		s--;
		rep = re_read(&s);
	}
	if (pat || rep)
		snprintf(xrep, sizeof(xrep), "%s", rep ? rep : "");
	free(pat);
	free(rep);
	if (!xkwdrs)
		return 1;
	int offs[xkwdrs->grpcnt * 2];
	for (i = beg; i < end; i++) {
		char *ln = lbuf_get(xb, i);
		sbuf *r = NULL;
		while (rset_find(xkwdrs, ln, offs, REG_NEWLINE) >= 0) {
			if (offs[xgrp] < 0) {
				ln += offs[1] > 0 ? offs[1] : 1;
				continue;
			} else if (!r)
				sbuf_make(r, 256)
			sbuf_mem(r, ln, offs[xgrp])
			replace(r, xrep, ln, offs);
			ln += offs[xgrp + 1];
			if (!offs[xgrp + 1])	/* zero-length match */
				sbuf_chr(r, (unsigned char)*ln++)
			if (*ln == '\n' || !*ln || !strchr(s, 'g'))
				break;
		}
		if (r) {
			if (first < 0) {
				first = i;
				lbuf_emark(xb, lbuf_opt(xb, NULL, xrow, 0, 0), 0, 0);
			}
			sbufn_str(r, ln)
			lbuf_edit(xb, r->s, i, i + 1);
			sbuf_free(r)
			last = i;
		}
	}
	if (first >= 0)
		lbuf_emark(xb, lbuf_opt(xb, NULL, xrow, 0, 0), first, last);
	return 0;
}

static int ec_exec(char *loc, char *cmd, char *arg)
{
	int beg, end;
	char *text, *rep;
	if (!loc[0])
		return ex_pipeout(arg, NULL);
	if (ex_region(loc, &beg, &end))
		return 1;
	text = lbuf_cp(xb, beg, end);
	rep = cmd_pipe(arg, text, 1);
	if (rep)
		lbuf_edit(xb, rep, beg, end);
	free(text);
	free(rep);
	return 0;
}

static int ec_ft(char *loc, char *cmd, char *arg)
{
	ex_ft = syn_setft(arg[0] ? arg : ex_ft);
	ex_print(ex_ft);
	if (led_attsb) {
		sbuf_free(led_attsb)
		led_attsb = NULL;
	}
	syn_reload = 1;
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

static int ec_glob(char *loc, char *cmd, char *arg)
{
	int beg, end, not;
	char *pat, *s = arg;
	int i;
	rset *rs;
	if (!loc[0] && !xgdep)
		loc = "%";
	if (ex_region(loc, &beg, &end))
		return 1;
	not = !!strchr(cmd, '!');
	pat = re_read(&s);
	if (pat)
		rs = rset_smake(pat, xic ? REG_ICASE : 0);
	free(pat);
	if (!pat || !rs)
		return 1;
	xgdep = !xgdep ? 1 : xgdep * 2;
	for (i = beg; i < end; i++)
		lbuf_i(xb, i)->grec |= xgdep;
	for (i = beg; i < lbuf_len(xb);) {
		char *ln = lbuf_get(xb, i);
		lbuf_s(ln)->grec &= ~xgdep;
		if ((rset_find(rs, ln, NULL, REG_NEWLINE) < 0) == not) {
			xrow = i;
			if (ex_exec(s))
				break;
			i = MIN(i, xrow);
		}
		while (i < lbuf_len(xb) && !(lbuf_i(xb, i)->grec & xgdep))
			i++;
	}
	rset_free(rs);
	xgdep /= 2;
	return 0;
}

static struct option {
	char *name;
	int *var;
} options[] = {
	{"ai", &xai},
	{"ic", &xic},
	{"td", &xtd},
	{"shape", &xshape},
	{"order", &xorder},
	{"hl", &xhl},
	{"hll", &xhll},
	{"hlw", &xhlw},
	{"hlp", &xhlp},
	{"hlr", &xhlr},
	{"tbs", &xtabspc},
	{"ish", &xish},
	{"grp", &xgrp},
	{"pac", &xpac},
	{"led", &xled},
	{"vis", &xvis},
	{"mpt", &xmpt},
	{"pr", &xpr},
	{"sep", &xsep},
};

static char *cutword(char *s, char *d)
{
	while (isspace((unsigned char)*s))
		s++;
	while (*s && !isspace((unsigned char)*s))
		*d++ = *s++;
	while (isspace((unsigned char)*s))
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
				if (!(val = atoi(r+1)))
					if (!isdigit((unsigned char)r[1]))
						val = (unsigned char)r[1];
			} else
				val = 1;
			strcpy(opt, tok);
		}
		for (i = 0; i < LEN(options); i++) {
			struct option *o = &options[i];
			if (!strcmp(o->name, opt)) {
				*o->var = val;
				return 0;
			}
		}
		ex_print("unknown option");
		return 1;
	}
	return 0;
}

static int ec_setdir(char *loc, char *cmd, char *arg)
{
	free(fs_exdir);
	fs_exdir = uc_dup(*arg ? arg : ".");
	if (cmd[1] == 'd')
		dir_calc(fs_exdir);
	return 0;
}

static int ec_chdir(char *loc, char *cmd, char *arg)
{
	char oldpath[4096];
	char newpath[4096];
	char *opath;
	int i, c, plen;
	oldpath[sizeof(oldpath)-1] = '\0';
	if (!getcwd(oldpath, sizeof(oldpath)))
		goto err;
	if (*arg && chdir(arg))
		goto err;
	if (!getcwd(newpath, sizeof(newpath)))
		goto err;
	plen = strlen(oldpath);
	if (oldpath[plen-1] != '/')
		oldpath[plen++] = '/';
	for (i = 0; i < xbufcur; i++) {
		if (!bufs[i].path[0])
			continue;
		if (bufs[i].path[0] == '/') {
			opath = bufs[i].path;
		} else {
			opath = oldpath;
			strncpy(opath+plen, bufs[i].path, sizeof(oldpath)-plen-1);
		}
		for (c = 0; opath[c] && opath[c] == newpath[c]; c++);
		if (newpath[c] || !opath[c])
			c = 0;
		else if (opath[c] == '/')
			c++;
		opath = uc_dup(opath+c);
		free(bufs[i].path);
		bufs[i].path = opath;
		bufs[i].plen = strlen(opath);
	}
	return 0;
	err:
	ex_print("chdir error");
	return 1;
}

static int ec_setincl(char *loc, char *cmd, char *arg)
{
	rset_free(fsincl);
	if (*arg)
		fsincl = rset_smake(arg, xic ? REG_ICASE : 0);
	else
		fsincl = NULL;
	return 0;
}

static int ec_setacreg(char *loc, char *cmd, char *arg)
{
	if (xacreg)
		sbuf_free(xacreg)
	if (*arg) {
		sbuf_make(xacreg, 128)
		sbufn_str(xacreg, arg)
	} else
		xacreg = NULL;
	return 0;
}

static int ec_setbufsmax(char *loc, char *cmd, char *arg)
{
	xbufsmax = *arg ? atoi(arg) : xbufsalloc;
	if (xbufsmax <= 0)
		return 1;
	int bufidx = ex_buf - bufs;
	int pbufidx = ex_pbuf - bufs;
	int tpbufidx = ex_tpbuf - bufs;
	int istemp = !ex_buf ? 0 : istempbuf(ex_buf);
	for (; xbufcur > xbufsmax; xbufcur--)
		bufs_free(xbufcur - 1);
	bufs = erealloc(bufs, sizeof(struct buf) * xbufsmax);
	if (!istemp)
		ex_buf = bufidx >= &bufs[xbufsmax] - bufs ? bufs : bufs+bufidx;
	ex_pbuf = pbufidx >= &bufs[xbufsmax] - bufs ? bufs : bufs+pbufidx;
	ex_tpbuf = tpbufidx >= &bufs[xbufsmax] - bufs ? bufs : bufs+tpbufidx;
	return 0;
}

static int ec_regprint(char *loc, char *cmd, char *arg)
{
	static char buf[5] = "  ";
	xleft = (xcols / 2) * (*arg ? atoi(arg) : 0);
	preserve(int, xtd, 2)
	for (int i = 1; i < LEN(xregs); i++) {
		if (xregs[i] && i != tolower(xpr)) {
			*buf = i;
			ex_cprint(buf, -1, 0, 0);
			ex_cprint(xregs[i], -1, xleft ? 0 : 2, 1);
		}
	}
	restore(xtd)
	return 0;
}

static int ec_setenc(char *loc, char *cmd, char *arg)
{
	if (cmd[0] == 'p') {
		if (!*arg) {
			if (ph != _ph)
				free(ph);
			phlen = LEN(_ph);
			ph = _ph;
			return 0;
		} else if (ph == _ph) {
			ph = NULL;
			phlen = 0;
		}
		ph = erealloc(ph, sizeof(struct placeholder) * (phlen + 1));
		ph[phlen].cp[0] = strtol(arg, &arg, 0);
		ph[phlen].cp[1] = strtol(arg, &arg, 0);
		ph[phlen].wid = strtol(arg, &arg, 0);
		ph[phlen].l = strtol(arg, &arg, 0);
		if (strlen(arg) && strlen(arg) < LEN(ph[0].d))
			strcpy(ph[phlen++].d, arg);
		return 0;
	}
	if (cmd[1] == 'z')
		zwlen = !zwlen ? def_zwlen : 0;
	else if (cmd[1] == 'b')
		bclen = !bclen ? def_bclen : 0;
	else if (utf8_length[0xc0] == 1) {
		memset(utf8_length+0xc0, 2, 0xe0 - 0xc0);
		memset(utf8_length+0xe0, 3, 0xf0 - 0xe0);
		memset(utf8_length+0xf0, 4, 0xf8 - 0xf0);
	} else
		memset(utf8_length+1, 1, 255);
	return 0;
}

static struct excmd {
	char *name;
	int (*ec)(char *loc, char *cmd, char *arg);
} excmds[] = { /* commands must be sorted longest of its kind topmost */
	{"@", ec_termexec},
	{"&", ec_termexec},
	{"!", ec_exec},
	{"bp", ec_setpath},
	{"bs", ec_save},
	{"bx", ec_setbufsmax},
	{"b", ec_buffer},
	{"pu", ec_put},
	{"ph", ec_setenc},
	{"p", ec_print},
	{"ac", ec_setacreg},
	{"a", ec_insert},
	{"ea!", ec_editapprox},
	{"ea", ec_editapprox},
	{"e!", ec_edit},
	{"e", ec_edit},
	{"ft", ec_ft},
	{"fd", ec_setdir},
	{"fp", ec_setdir},
	{"f", ec_search},
	{"inc", ec_setincl},
	{"i", ec_insert},
	{"d", ec_delete},
	{"g!", ec_glob},
	{"g", ec_glob},
	{"k", ec_mark},
	{"q!", ec_quit},
	{"q", ec_quit},
	{"reg", ec_regprint},
	{"rd", ec_undoredo},
	{"r", ec_read},
	{"wq!", ec_write},
	{"wq", ec_write},
	{"w!", ec_write},
	{"w", ec_write},
	{"uc", ec_setenc},
	{"uz", ec_setenc},
	{"ub", ec_setenc},
	{"u", ec_undoredo},
	{"se", ec_set},
	{"s", ec_substitute},
	{"x!", ec_write},
	{"x", ec_write},
	{"ya!", ec_yank},
	{"ya", ec_yank},
	{"cm!", ec_cmap},
	{"cm", ec_cmap},
	{"cd", ec_chdir},
	{"c", ec_insert},
	{"=", ec_lnum},
	{"", ec_print}, /* do not remove */
};

/* parse ex command until xsep or null. */
static const char *ex_parse(const char *src, char *loc, int *idx, char *arg)
{
	int i, j;
	while (*src == xsep || *src == ' ' || *src == '\t')
		src++;
	while (*src && strchr(" \t0123456789+-.,/?$';%", *src)) {
		if (*src == '\'' && src[1])
			*loc++ = *src++;
		if (*src == '/' || *src == '?') {
			j = *src;
			do {
				if (*src == '\\' && src[1])
					*loc++ = *src++;
				*loc++ = *src++;
			} while (*src && *src != j);
			if (*src)
				*loc++ = *src++;
		} else
			*loc++ = *src++;
	}
	for (i = 0; i < LEN(excmds); i++) {
		for (j = 0; excmds[i].name[j]; j++)
			if (!src[j] || src[j] != excmds[i].name[j])
				break;
		if (!excmds[i].name[j]) {
			*idx = i;
			src += j;
			break;
		}
	}
	if (*src == ' ' || *src == '\t')
		src++;
	while (*src && *src != xsep) {
		if (*src == '\\' && src[1] == xsep)
			src++;
		*arg++ = *src++;
	}
	*loc = '\0';
	*arg = '\0';
	return src;
}

/* execute a single ex command */
int ex_exec(const char *ln)
{
	int ret = 0, idx = 0, len = strlen(ln) + 1;
	char loc[len], arg[len];
	while (*ln) {
		ln = ex_parse(ln, loc, &idx, arg);
		char *ecmd = ex_pathexpand(arg);
		if (ecmd)
			ret = excmds[idx].ec(loc, excmds[idx].name, ecmd);
		free(ecmd);
	}
	return ret;
}

/* ex main loop */
void ex(void)
{
	vi_lncol = 0;
	xgrec++;
	while (!xquit) {
		char *ln = ex_read(":");
		if (ln) {
			ex_command(ln)
			free(ln);
			lbuf_modified(xb);
		}
	}
	xgrec--;
}

void ex_init(char **files, int n)
{
	xbufsalloc = MAX(n, xbufsalloc);
	ec_setbufsmax(NULL, NULL, "");
	char *s = files[0] ? files[0] : "";
	do {
		ec_edit("", "e", s);
		s = *(++files);
	} while (--n > 0);
	xmpt = 0;
	xvis &= ~8;
	if ((s = getenv("EXINIT")))
		ex_command(s)
}
