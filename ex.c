int xrow, xoff, xtop;		/* current row, column, and top row */
int xleft;			/* the first visible column */
int xquit;			/* exit if set */
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
int xish = 1;			/* interactive shell */
int xgrp = 2;			/* regex search group */
int xpac;			/* print autocomplete options */
int xkwdcnt;			/* number of search kwd changes */
int xbufcur;			/* number of active buffers */
struct buf *bufs;		/* main buffers */
struct buf tempbufs[2];		/* temporary buffers, for internal use */
struct buf *ex_buf;		/* current buffer */
struct buf *ex_pbuf;		/* prev buffer */
static struct buf *ex_tpbuf;	/* temp prev buffer */
sbuf *xacreg;			/* autocomplete db filter regex */
rset *xkwdrs;			/* the last searched keyword rset */
int xkwddir;			/* the last search direction */
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

void temp_done(int i)
{
	if (tempbufs[i].lb) {
		free(tempbufs[i].path);
		lbuf_free(tempbufs[i].lb);
		tempbufs[i].lb = NULL;
	}
}

/* replace % and # with buffer names and !..! with command output */
static char *ex_pathexpand(char *src)
{
	sbuf *sb; sbuf_make(sb, 1024)
	while (*src) {
		if (*src == '#' || *src == '%') {
			int n = -1;
			struct buf *pbuf = *src == '%' ? ex_buf : ex_pbuf;
			if ((src[1] ^ '0') < 10)
				pbuf = &bufs[n = atoi(&src[1])];
			if (pbuf >= &bufs[xbufcur] || !pbuf->path[0]) {
				ex_show("\"#\" or \"%\" is not set");
				sbuf_free(sb)
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
	sbufn_done(sb)
}

/* set the current search keyword rset if the kwd or flags changed */
void ex_krsset(char *kwd, int dir)
{
	char *reg = vi_regget('/', &(int){0});
	if (kwd && *kwd && ((!reg || !xkwdrs || strcmp(kwd, reg))
		|| ((xkwdrs->regex->flg & REG_ICASE) != xic))) {
		rset_free(xkwdrs);
		xkwdrs = rset_make(1, (char*[]){kwd}, xic ? REG_ICASE : 0);
		xkwdcnt++;
		vi_regput('/', kwd, 0);
		xkwddir = dir;
	}
	if (dir == -2 || dir == 2)
		xkwddir = dir / 2;
}

static int ex_search(char **pat)
{
	sbuf *kw;
	int row;
	char *e = *pat;
	sbufn_make(kw, 64)
	while (*++e) {
		if (*e == **pat)
			break;
		sbufn_chr(kw, (unsigned char) *e)
		if (*e == '\\' && e[1])
			e++;
	}
	ex_krsset(kw->s, **pat == '/' ? 2 : -2);
	sbuf_free(kw)
	*pat = *e ? e + 1 : e;
	if (!xkwdrs)
		return -1;
	row = xrow + xkwddir;
	while (row >= 0 && row < lbuf_len(xb)) {
		if (rset_find(xkwdrs, lbuf_get(xb, row), 0, NULL, REG_NEWLINE) >= 0)
			break;
		row += xkwddir;
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
	if (!strcmp("%", loc) || !lbuf_len(xb)) {
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
	if (*beg < 0 && *end == 0)
		*beg = 0;
	if (*beg < 0 || *beg >= lbuf_len(xb))
		return 1;
	if (*end < *beg || *end > lbuf_len(xb))
		return 1;
	return 0;
}

static int ec_buffer(char *loc, char *cmd, char *arg)
{
	if (!arg[0]) {
		char ln[EXLEN];
		for (int i = 0; i < xbufcur; i++) {
			char c = ex_buf == bufs+i ? '%' : ' ';
			c = ex_pbuf == bufs+i ? '#' : c;
			snprintf(ln, LEN(ln), "%i %c %s", i,
				c + lbuf_modified(bufs[i].lb), bufs[i].path);
			ex_print(ln);
		}
	} else if (atoi(arg) < 0) {
		if (abs(atoi(arg)) <= LEN(tempbufs))
			temp_switch(abs(atoi(arg))-1);
		else
			ex_show("no such buffer");
	} else if (atoi(arg) < xbufcur) {
		bufs_switchwft(atoi(arg))
	} else
		ex_show("no such buffer");
	return 0;
}

static int ec_quit(char *loc, char *cmd, char *arg)
{
	for (int i = 0; !strchr(cmd, '!') && i < xbufcur; i++)
		if (lbuf_modified(bufs[i].lb)) {
			ex_show("buffers modified");
			return 1;
		}
	xquit = 1;
	return 0;
}

void ex_bufpostfix(struct buf *p, int clear)
{
	p->mtime = mtime(p->path);
	p->ft = syn_filetype(p->path);
	lbuf_saved(p->lb, clear);
}

#define readfile(errchk) \
fd = open(ex_path, O_RDONLY); \
if (fd >= 0) { \
	errchk lbuf_rd(xb, fd, 0, lbuf_len(xb)); \
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
	readfile(/**/)
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
		ex_show("last buffer modified");
		return 1;
	} else if (len || !xbufcur || !strchr(cmd, '!')) {
		bufs_switch(bufs_open(arg+cd, len));
		cd = 3; /* XXX: sigh... */
	}
	readfile(rd =)
	if (cd == 3 || (!rd && fd >= 0)) {
		ex_bufpostfix(ex_buf, arg[0]);
		syn_setft(ex_ft);
	}
	snprintf(msg, sizeof(msg), "\"%s\"  %d lines  [%c]",
			*ex_path ? ex_path : "unnamed", lbuf_len(xb),
			fd < 0 || rd ? 'f' : 'r');
	ex_show(msg);
	return fd < 0 || rd;
}

static int ec_editapprox(char *loc, char *cmd, char *arg)
{
	int len, i, inst;
	char *path, *arg1;
	arg1 = arg+dstrlen(arg, ' ');
	inst = atoi(arg1);
	*arg1 = '\0';
	for (int pos = 0; pos < lbuf_len(tempbufs[1].lb); pos++) {
		path = lbuf_get(tempbufs[1].lb, pos);
		len = *(int*)(path - sizeof(int));
		for (i = len; i > 0 && path[i] != '/'; i--);
		if (!i)
			continue;
		path[len] = '\0';
		if (strstr(&path[i+1], arg)) {
			if (!inst) {
				ec_edit(loc, cmd, path);
				path[len] = '\n';
				break;
			}
			inst--;
		}
		path[len] = '\n';
	}
	return 1;
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
	int beg, end;
	char *path;
	char *obuf;
	int n = lbuf_len(xb);
	path = arg[0] ? arg : ex_path;
	if (ex_region(loc, &beg, &end))
		return 1;
	if (arg[0] == '!') {
		int pos = MIN(xrow + 1, lbuf_len(xb));
		obuf = cmd_pipe(arg + 1, NULL, 1);
		if (obuf)
			lbuf_edit(xb, obuf, pos, pos);
		free(obuf);
	} else {
		int fd = open(path, O_RDONLY);
		int pos = lbuf_len(xb) ? end : 0;
		if (fd < 0) {
			ex_show("read failed");
			return 1;
		}
		if (lbuf_rd(xb, fd, pos, pos)) {
			ex_show("read failed");
			close(fd);
			return 1;
		}
		close(fd);
	}
	xrow = end + lbuf_len(xb) - n - 1;
	snprintf(msg, sizeof(msg), "\"%s\"  %d lines  [r]",
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
		ex_print(NULL);
		cmd_pipe(arg + 1, ibuf, 0);
		free(ibuf);
	} else {
		int fd;
		if (!strchr(cmd, '!') && !strcmp(ex_path, path) &&
				mtime(ex_path) > ex_buf->mtime) {
			ex_show("write failed: file changed");
			return 1;
		}
		if (!strchr(cmd, '!') && arg[0] && mtime(arg) >= 0) {
			ex_show("write failed: file exists");
			return 1;
		}
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, conf_mode());
		if (fd < 0) {
			ex_show("write failed: cannot create file");
			return 1;
		}
		if (lbuf_wr(xb, fd, beg, end)) {
			ex_show("write failed");
			close(fd);
			return 1;
		}
		close(fd);
		snprintf(msg, sizeof(msg), "\"%s\"  %d lines  [w]",
				path, end - beg);
		ex_show(msg);
	}
	if (!strcmp(ex_path, path)) {
		lbuf_saved(xb, 0);
		ex_buf->mtime = mtime(path);
	}
	if (cmd[0] == 'x' || (cmd[0] == 'w' && cmd[1] == 'q'))
		ec_quit("", cmd, "");
	return 0;
}

static int ec_termpush(char *loc, char *cmd, char *arg)
{
	if (*arg)
		term_exec(arg, strlen(arg), /*nop*/, term_push("qq", 3);)
	return 0;
}

static int ec_insert(char *loc, char *cmd, char *arg)
{
	sbuf *sb;
	char *s;
	int beg, end;
	int n;
	if (ex_region(loc, &beg, &end) && (beg != 0 || end != 0))
		return 1;
	sbufn_make(sb, 64)
	while ((s = ex_read(""))) {
		if (!strcmp(".", s)) {
			free(s);
			break;
		}
		sbuf_str(sb, s)
		sbufn_chr(sb, '\n')
		free(s);
	}
	if (cmd[0] == 'a' && (beg + 1 <= lbuf_len(xb)))
		beg++;
	if (cmd[0] != 'c')
		end = beg;
	if (vi_insmov != TK_CTL('c')) {
		n = lbuf_len(xb);
		lbuf_edit(xb, sb->s, beg, end);
		xrow = MIN(lbuf_len(xb) - 1, end + lbuf_len(xb) - n - 1);
	}
	sbuf_free(sb)
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
	if (xvis & 4)
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
	ex_yank((unsigned char) arg[0], beg, end);
	lbuf_edit(xb, NULL, beg, end);
	xrow = beg;
	return 0;
}

static int ec_yank(char *loc, char *cmd, char *arg)
{
	int beg, end;
	if (ex_region(loc, &beg, &end) || !lbuf_len(xb))
		return 1;
	ex_yank((unsigned char) arg[0], beg, end);
	return 0;
}

static int ec_put(char *loc, char *cmd, char *arg)
{
	int beg, end;
	int lnmode;
	char *buf;
	int n = lbuf_len(xb);
	buf = vi_regget((unsigned char) arg[0], &lnmode);
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
	sprintf(msg, "%d", end);
	ex_show(msg);
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
	lbuf_mark(xb, (unsigned char) arg[0], end - 1, 0);
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
	int beg, end, grp = xgrp != 2 ? xgrp : 32;
	int offs[grp];
	char *pat = NULL, *rep = NULL;
	char *s = arg;
	int i;
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
	/* if the change is bigger than display size
	set savepoint where command was issued. */
	if (end - beg > xrows)
		lbuf_opt(xb, NULL, xrow, 0);
	for (i = beg; i < end; i++) {
		char *ln = lbuf_get(xb, i);
		sbuf *r = NULL;
		while (rset_find(xkwdrs, ln, grp / 2, offs, REG_NEWLINE) >= 0) {
			if (offs[xgrp - 2] < 0) {
				ln += offs[1] > 0 ? offs[1] : 1;
				continue;
			} else if (!r)
				sbuf_make(r, 256)
			sbuf_mem(r, ln, offs[xgrp - 2])
			replace(r, xrep, ln, offs);
			ln += offs[xgrp - 1];
			if (!offs[xgrp - 1])	/* zero-length match */
				sbuf_chr(r, (unsigned char)*ln++)
			if (*ln == '\n' || !*ln || !strchr(s, 'g'))
				break;
		}
		if (r) {
			sbufn_str(r, ln)
			lbuf_edit(xb, r->s, i, i + 1);
			sbuf_free(r)
		}
	}
	if (end - beg > xrows)
		lbuf_opt(xb, NULL, xrow, 0);
	return 0;
}

static int ec_exec(char *loc, char *cmd, char *arg)
{
	int beg, end;
	char *text, *rep;
	if (!loc[0]) {
		int ret;
		ex_print(NULL);
		ret = cmd_exec(arg);
		return ret;
	}
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
	ex_show(ex_ft);
	syn_reload = 1;
	return 0;
}

static int ec_cmap(char *loc, char *cmd, char *arg)
{
	if (arg[0])
		xkmap_alt = conf_kmapfind(arg);
	else
		ex_show(conf_kmap(xkmap)[0]);
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
	not = strchr(cmd, '!') || cmd[0] == 'v';
	pat = re_read(&s);
	if (pat)
		rs = rset_make(1, (char*[]){pat}, xic ? REG_ICASE : 0);
	free(pat);
	if (!pat || !rs)
		return 1;
	xgdep++;
	for (i = beg + 1; i < end; i++)
		lbuf_globset(xb, i, xgdep);
	i = beg;
	while (i < lbuf_len(xb)) {
		char *ln = lbuf_get(xb, i);
		if ((rset_find(rs, ln, 0, NULL, REG_NEWLINE) < 0) == not) {
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
	rset_free(rs);
	xgdep--;
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
			if (!strcmp(o->name, opt)) {
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
	free(fs_exdir);
	fs_exdir = uc_dup(*arg ? arg : ".");
	if (cmd[1] == 'd')
		dir_calc(fs_exdir);
	return 0;
}

static int ec_chdir(char *loc, char *cmd, char *arg)
{
	if (*arg && chdir(arg))
		ex_show("chdir error");
	return 0;
}

static int ec_setincl(char *loc, char *cmd, char *arg)
{
	rset_free(fsincl);
	if (*arg)
		fsincl = rset_make(1, (char*[]){arg}, xic ? REG_ICASE : 0);
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
	for (; xbufcur > xbufsmax; xbufcur--)
		bufs_free(xbufcur - 1);
	int bufidx = ex_buf - bufs;
	int pbufidx = ex_pbuf - bufs;
	bufs = realloc(bufs, sizeof(struct buf) * xbufsmax);
	ex_buf = bufidx >= &bufs[xbufsmax] - bufs ? bufs : bufs+bufidx;
	ex_pbuf = pbufidx >= &bufs[xbufsmax] - bufs ? bufs : bufs+pbufidx;
	return 0;
}

static struct excmd {
	char *name;
	int (*ec)(char *loc, char *cmd, char *arg);
} excmds[] = {
	{"b", ec_buffer},
	{"bp", ec_setpath},
	{"bs", ec_save},
	{"p", ec_print},
	{"a", ec_insert},
	{"ea", ec_editapprox},
	{"ea!", ec_editapprox},
	{"i", ec_insert},
	{"d", ec_delete},
	{"c", ec_insert},
	{"e", ec_edit},
	{"e!", ec_edit},
	{"g", ec_glob},
	{"g!", ec_glob},
	{"=", ec_lnum},
	{"k", ec_mark},
	{"tp", ec_termpush},
	{"pu", ec_put},
	{"q", ec_quit},
	{"q!", ec_quit},
	{"r", ec_read},
	{"v", ec_glob},
	{"w", ec_write},
	{"w!", ec_write},
	{"wq", ec_write},
	{"wq!", ec_write},
	{"u", ec_undo},
	{"rd", ec_redo},
	{"se", ec_set},
	{"s", ec_substitute},
	{"x", ec_write},
	{"x!", ec_write},
	{"ya", ec_yank},
	{"!", ec_exec},
	{"ft", ec_ft},
	{"cm", ec_cmap},
	{"cm!", ec_cmap},
	{"fd", ec_setdir},
	{"fp", ec_setdir},
	{"cd", ec_chdir},
	{"inc", ec_setincl},
	{"bx", ec_setbufsmax},
	{"ac", ec_setacreg},
	{"", ec_null},
};

static int ex_idx(const char *cmd)
{
	for (int i = 0; i < LEN(excmds); i++)
		if (!strcmp(excmds[i].name, cmd))
			return i;
	return -1;
}

/* parse ex command until | or eol. */
static const char *ex_parse(const char *src, char *loc, char *cmd, char *arg)
{
	while (*src == ':' || *src == ' ' || *src == '\t')
		src++;
	while (*src && strchr(" \t0123456789+-.,/?$';%", *src)) {
		if (*src == '/' || *src == '?') {
			int d = *src;
			do {
				if (*src == '\\' && src[1])
					*loc++ = *src++;
				*loc++ = *src++;
			} while (*src && *src != d);
			if (*src)
				*loc++ = *src++;
		} else
			*loc++ = *src++;
	}
	while (*src && isalpha((unsigned char)*src))
		*cmd++ = *src++;
	if (*src == '!' || *src == '=')
		*cmd++ = *src++;
	while (*src == ' ' || *src == '\t')
		src++;
	while (*src && *src != '|') {
		if (*src == '\\' && src[1] == '|')
			src++;
		*arg++ = *src++;
	}
	*loc = '\0';
	*cmd = '\0';
	*arg = '\0';
	return *src == '|' ? src+1 : src;
}

/* execute a single ex command */
int ex_exec(const char *ln)
{
	int ret = 0, len = strlen(ln) + 1;
	char loc[len], cmd[len], arg[len];
	while (*ln) {
		ln = ex_parse(ln, loc, cmd, arg);
		char *ecmd = ex_pathexpand(arg);
		int idx = ex_idx(cmd);
		if (idx >= 0 && ecmd)
			ret = excmds[idx].ec(loc, cmd, ecmd);
		free(ecmd);
	}
	return ret;
}

/* ex main loop */
void ex(void)
{
	while (!xquit) {
		char *ln = ex_read(":");
		if (ln) {
			ex_command(ln)
			free(ln);
			lbuf_modified(xb);
		}
	}
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
	if (!(xvis & 2) && (s = getenv("EXINIT")))
		ex_command(s)
}
