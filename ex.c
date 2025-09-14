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
int xtbs = 8;			/* number of spaces for tab */
int xish;			/* interactive shell */
int xgrp;			/* regex search group */
int xpac;			/* print autocomplete options */
int xmpt;			/* whether to prompt after printing > 1 lines in vi */
int xpr;			/* ex_cprint register */
int xsep = ':';			/* ex command separator */
int xlim = -1;			/* rendering cutoff for non cursor lines */
int xseq = 1;			/* undo/redo sequence */

int xrow, xoff, xtop;		/* current row, column, and top row */
int xbufcur;			/* number of active buffers */
int xgrec;			/* global vi/ex recursion depth */
int xkmap;			/* the current keymap */
int xkmap_alt = 1;		/* the alternate keymap */
int xkwddir;			/* the last search direction */
int xkwdcnt;			/* number of search kwd changes */
sbuf *xacreg;			/* autocomplete db filter regex */
rset *xkwdrs;			/* the last searched keyword rset */
sbuf *xregs[256];		/* string registers */
struct buf *bufs;		/* main buffers */
struct buf tempbufs[2];		/* temporary buffers, for internal use */
struct buf *ex_buf;		/* current buffer */
struct buf *ex_pbuf;		/* prev buffer */
static struct buf *ex_tpbuf;	/* temp prev buffer */
static int xbufsmax;		/* number of buffers */
static int xbufsalloc = 10;	/* initial number of buffers */
static int xgdep;		/* global command recursion depth */
static char xuerr[] = "unreported error";
static char xserr[] = "syntax error";
static char xrerr[] = "invalid range";

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
	syn_setft(xb_ft);
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
static char *ex_pathexpand(sbuf *sb, char *src)
{
	while (*src) {
		if (*src == '%') {
			int n = -1;
			struct buf *pbuf = ex_buf;
			if (src[1] == '#') {
				pbuf = ex_pbuf;
				src++;
			} else if ((src[1] ^ '0') < 10)
				pbuf = &bufs[n = atoi(&src[1])];
			if (pbuf >= &bufs[xbufcur] || !pbuf->path[0]) {
				ex_print("\"#\" or \"%\" is not set", msg_ft)
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
			char *str = cmd_pipe(sb->s + n, NULL, NULL, 1);
			sbuf_cut(sb, n)
			if (str)
				sbuf_str(sb, str)
			free(str);
		} else {
			if (*src == '\\' && (src[1] == '%' || src[1] == '!'))
				src++;
			sbuf_chr(sb, *src++)
		}
	}
	sbufn_sret(sb)
}

/* set the current search keyword rset if the kwd or flags changed */
void ex_krsset(char *kwd, int dir)
{
	sbuf *reg = xregs['/'];
	if (kwd && *kwd && ((!reg || !xkwdrs || strcmp(kwd, reg->s))
			|| ((xkwdrs->regex->flg & REG_ICASE) != xic))) {
		rset_free(xkwdrs);
		xkwdrs = rset_smake(kwd, xic ? REG_ICASE : 0);
		xkwdcnt++;
		ex_regput('/', kwd, 0);
		xkwddir = dir;
	}
	if (dir == -2 || dir == 2)
		xkwddir = dir / 2;
}

static char *ex_reread(char **re, int *dir)
{
	*dir = **re == '/' ? 2 : -2;
	if (*dir < 0 && **re != '?')
		return xserr;
	char *e = re_read(re);
	if (!e)
		return xserr;
	ex_krsset(e, *dir);
	free(e);
	if (!xkwdrs)
		return xserr;
	return NULL;
}

static int ex_range(char **num, int n, int *row)
{
	int dir, off, beg, end;
	switch ((unsigned char) **num) {
	case '.':
		++*num;
		break;
	case '$':
		n = row ? lbuf_eol(xb, *row, 2) : lbuf_len(xb) - 1;
		++*num;
		break;
	case '\'':
		if (lbuf_jump(xb, (unsigned char) *++(*num),
				&n, row ? &n : &dir))
			return -1;
		++*num;
		break;
	case '/':
	case '?':
		off = row ? n : 0;
		if (off < 0 || ex_reread(num, &dir))
			return -1;
		beg = row ? *row : xrow + (xkwddir > 0);
		end = row ? beg+1 : lbuf_len(xb);
		if (lbuf_search(xb, xkwdrs, xkwddir, &beg,
				&off, end, MIN(dir, 0)))
			return -1;
		n = row ? off : beg;
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
#define ex_vregion(loc, beg, end) ex_region(loc, beg, end, NULL, NULL)
static int ex_region(char *loc, int *beg, int *end, int *o1, int *o2)
{
	if (!strcmp("%", loc) || !lbuf_len(xb)) {
		*beg = 0;
		*end = MAX(0, lbuf_len(xb));
		return 0;
	}
	int vaddr = 0, haddr = 0, ooff = xoff, row;
	while (*loc) {
		if (*loc == ';' || *loc == ',') {
			loc++;
			if (loc[-1] == ',')
				goto skip;
			row = vaddr ? vaddr % 2 ? *beg : *end-1 : xrow;
			ooff = ex_range(&loc, ooff, &row);
			if (o2 && haddr++ % 2)
				*o2 = ooff;
			else if (o1)
				*o1 = ooff;
		} else {
			skip:
			if (vaddr++ % 2)
				*end = ex_range(&loc, xrow, NULL) + 1;
			else
				*beg = ex_range(&loc, xrow, NULL);
		}
		while (*loc && *loc != ';' && *loc != ',')
			loc++;
	}
	if (haddr) {
		if (ooff < 0)
			return 1;
		xoff = ooff;
	}
	if (!vaddr) {
		*beg = xrow;
		*end = MIN(lbuf_len(xb), xrow + 1);
		return 0;
	}
	if (*beg < 0 || *beg >= lbuf_len(xb))
		return 1;
	if (vaddr < 2)
		*end = *beg + 1;
	else if (*end <= *beg || *end > lbuf_len(xb))
		return 1;
	return 0;
}

static void *ec_find(char *loc, char *cmd, char *arg)
{
	int dir, off, obeg, beg, end;
	char *err = ex_reread(&arg, &dir);
	if (err)
		return err;
	if (ex_vregion(loc, &beg, &end))
		return xrerr;
	off = xoff;
	obeg = beg;
	if (xrow < beg || xrow > end) {
		off = 0;
		beg = xkwddir > 0 ? beg : end++;
	} else
		beg = xrow;
	if (lbuf_search(xb, xkwdrs, xkwddir, &beg,
			&off, end, cmd[1] == '+' ? xkwddir : MIN(dir, 0)))
		return xuerr;
	if (beg < obeg)
		return xuerr;
	xrow = beg;
	xoff = off;
	return NULL;
}

static void *ec_buffer(char *loc, char *cmd, char *arg)
{
	if (!arg[0]) {
		char ln[512];
		for (int i = 0; i < xbufcur; i++) {
			char c = ex_buf == bufs+i ? '%' : ' ';
			c = ex_pbuf == bufs+i ? '#' : c;
			snprintf(ln, LEN(ln), "%d %c %s", i,
				c + (char)bufs[i].lb->modified, bufs[i].path);
			ex_print(ln, msg_ft)
		}
		return NULL;
	} else if (atoi(arg) < 0) {
		if (abs(atoi(arg)) <= LEN(tempbufs)) {
			temp_switch(abs(atoi(arg))-1);
			return NULL;
		}
	} else if (atoi(arg) < xbufcur) {
		bufs_switchwft(atoi(arg))
		return NULL;
	}
	return "no such buffer";
}

static void *ec_quit(char *loc, char *cmd, char *arg)
{
	for (int i = 0; !strchr(cmd, '!') && i < xbufcur; i++)
		if ((xquit < 0 || xgrec < 2) && bufs[i].lb->modified)
			return "buffers modified";
	if (!xquit)
		xquit = !strchr(cmd, '!') ? 1 : -1;
	return NULL;
}

void ex_bufpostfix(struct buf *p, int clear)
{
	p->mtime = mtime(p->path);
	p->ft = syn_filetype(p->path);
	lbuf_saved(p->lb, clear);
}

#define readfile(errchk, init) \
fd = open(xb_path, O_RDONLY); \
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
	readfile(, 1)
	return 0;
}

static void *ec_edit(char *loc, char *cmd, char *arg)
{
	char msg[128];
	int fd, len, rd = 0, cd = 0;
	if (arg[0] == '.' && arg[1] == '/')
		cd = 2;
	len = strlen(arg+cd);
	if (len && ((fd = bufs_find(arg+cd, len)) >= 0)) {
		bufs_switchwft(fd)
		return NULL;
	} else if (xbufcur == xbufsmax && !strchr(cmd, '!') &&
			bufs[xbufsmax - 1].lb->modified) {
		return "last buffer modified";
	} else if (len || !xbufcur || !strchr(cmd, '!')) {
		bufs_switch(bufs_open(arg+cd, len));
		cd = 3; /* XXX: quick hack to indicate new lbuf */
	}
	readfile(rd =, cd == 3)
	if (cd == 3 || (!rd && fd >= 0)) {
		ex_bufpostfix(ex_buf, arg[0]);
		syn_setft(xb_ft);
	}
	snprintf(msg, sizeof(msg), "\"%s\" %dL [%c]",
			*xb_path ? xb_path : "unnamed", lbuf_len(xb),
			fd < 0 || rd ? 'f' : 'r');
	if (!(xvis & 8))
		ex_print(msg, bar_ft)
	return fd < 0 || rd ? xuerr : NULL;
}

static void *ec_editapprox(char *loc, char *cmd, char *arg)
{
	sbuf_smake(sb, 128)
	char ln[512];
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
			ex_print(ln, msg_ft)
		}
	}
	if (inst < 0 && c > 1) {
		inst = term_read();
		inst = inst == '\n' ? 0 : inst - '0';
	}
	if ((inst >= 0 && inst < c) || c == 1) {
		path = *((char**)sb->s + (c == 1 ? 0 : inst));
		path[lbuf_s(path)->len] = '\0';
		ec_edit(loc, cmd, path);
		path[lbuf_s(path)->len] = '\n';
	}
	xmpt = xmpt >= 0 ? 0 : xmpt;
	free(sb->s);
	return NULL;
}

static void *ec_setpath(char *loc, char *cmd, char *arg)
{
	free(xb_path);
	xb_path = uc_dup(arg);
	ex_buf->plen = strlen(arg);
	return NULL;
}

static void *ec_read(char *loc, char *cmd, char *arg)
{
	char msg[512];
	int beg, end, fd = -1;
	char *path, *obuf, *ret = NULL;
	int n = lbuf_len(xb);
	int pos = MIN(xrow + 1, lbuf_len(xb));
	struct lbuf *lb = lbuf_make(), *pxb = xb;
	path = arg[0] ? arg : xb_path;
	if (arg[0] == '!') {
		obuf = cmd_pipe(arg + 1, NULL, NULL, 1);
		if (obuf)
			lbuf_edit(lb, obuf, 0, 0);
		free(obuf);
	} else {
		if ((fd = open(path, O_RDONLY)) < 0) {
			ret = "open failed";
			goto err;
		}
		if (lbuf_rd(lb, fd, 0, 0, 0)) {
			ret = "read failed";
			goto err;
		}
	}
	xb = lb;
	xrow = 0;
	if (ex_vregion(loc, &beg, &end)) {
		ret = xrerr;
		goto err;
	}
	obuf = lbuf_cp(lb, beg, end);
	if (*obuf)
		lbuf_edit(pxb, obuf, pos, pos);
	snprintf(msg, sizeof(msg), "\"%s\" %dL [r]",
			path, lbuf_len(pxb) - n);
	ex_print(msg, bar_ft)
	free(obuf);
	err:
	lbuf_free(lb);
	xrow = pos;
	xb = pxb;
	if (fd >= 0)
		close(fd);
	return ret;
}

static void *ex_pipeout(char *cmd, char *buf)
{
	int ret = 0;
	if (!(xvis & 4)) {
		term_chr('\n');
		xmpt = xmpt >= 0 ? 2 : xmpt;
	}
	cmd_pipe(cmd, buf, &ret, 0);
	return ret ? xuerr : NULL;
}

static void *ec_write(char *loc, char *cmd, char *arg)
{
	char msg[512];
	char *path;
	char *ibuf;
	int beg, end;
	path = arg[0] ? arg : xb_path;
	if (cmd[0] == 'x' && !xb->modified)
		return ec_quit("", cmd, "");
	if (ex_vregion(loc, &beg, &end))
		return xrerr;
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
			if (!strcmp(xb_path, path) && mtime(path) > ex_buf->mtime)
				return "write failed: file changed";
			if (arg[0] && mtime(path) >= 0)
				return "write failed: file exists";
		}
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, conf_mode);
		if (fd < 0)
			return "write failed: cannot create file";
		if (lbuf_wr(xb, fd, beg, end)) {
			close(fd);
			return "write failed";
		}
		close(fd);
		snprintf(msg, sizeof(msg), "\"%s\" %dL [w]",
				path, end - beg);
		ex_print(msg, bar_ft)
		if (strcmp(xb_path, path))
			ec_setpath(NULL, NULL, path);
		lbuf_saved(xb, 0);
		ex_buf->mtime = mtime(path);
	}
	if (cmd[0] == 'x' || (cmd[0] == 'w' && cmd[1] == 'q'))
		ec_quit("", cmd, "");
	return NULL;
}

static void *ec_termexec(char *loc, char *cmd, char *arg)
{
	if (*arg)
		term_exec(arg, strlen(arg), cmd[0])
	return NULL;
}

void ex_cprint(char *line, char *ft, int r, int c, int ln)
{
	syn_blockhl = 0;
	if (!(xvis & 4)) {
		if (xmpt == 1)
			term_chr('\n');
		if (isupper(xpr) && xmpt == 1 && !strchr(line, '\n'))
			ex_regput(xpr, "\n", 1);
		term_pos(xrows, led_pos(vi_msg, 0));
		snprintf(vi_msg+c, sizeof(vi_msg)-c, "%s", line);
	}
	if (xpr)
		ex_regput(xpr, line, 1);
	if (xvis & 2) {
		term_write(line, dstrlen(line, '\n'))
		term_write("\n", 1)
		return;
	}
	syn_setft(ft);
	led_crender(line, r, c, xleft, xleft + xcols - c)
	syn_setft(xb_ft);
	if (ln && (xvis & 4 || xmpt > 0)) {
		term_chr('\n');
		if (isupper(xpr) && !strchr(line, '\n'))
			ex_regput(xpr, "\n", 1);
	}
	xmpt += !(xvis & 4) && xmpt >= 0 && (ln || xmpt);
}

static int ex_read(sbuf *sb, char *msg, char *ft, int ps, int hist)
{
	if (!(xvis & 2)) {
		preserve(int, xleft, xleft = 0;)
		int n = sb->s_n, key;
		sbuf_str(sb, msg)
		syn_setft(ft);
		led_prompt(sb, NULL, &xkmap, &key, ps, hist);
		syn_setft(xb_ft);
		restore(xleft)
		if (key != '\n')
			return 1;
		else if (!*msg || strcmp(sb->s + n, msg))
			term_chr('\n');
		return 0;
	}
	for (int c; !xquit && (c = term_read()) != '\n';)
		sbuf_chr(sb, c)
	if (xquit)
		return 1;
	sbuf_null(sb)
	return 0;
}

static char *ex_lbufstr(char *s, char *e, char *se, char *i, int in)
{
	char *p = emalloc(lbuf_s(s)->len + 2 + in);
	memcpy(p, s, e - s);
	memcpy(p + (e - s), i, in);
	memcpy(p + (e - s + in), se, lbuf_s(s)->len + 2 - (e - s) - (se - e));
	return p;
}

static void *ec_insert(char *loc, char *cmd, char *arg)
{
	int beg, end, o1 = -1, o2 = -1, ps = 0;
	if (ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	char *e, *se;
	char *ln = o1 >= 0 ? lbuf_get(xb, beg) : NULL;
	sbuf_smake(sb, 128)
	if (*arg)
		term_push(arg, strlen(arg));
	vi_insmov = 0;
	while (!ex_read(sb, "", msg_ft, ps, 0)) {
		if (xvis & 2 && !strcmp(".", sb->s + ps)) {
			sb->s_n--;
			break;
		}
		sbuf_chr(sb, '\n')
		ps = sb->s_n;
	}
	if (vi_insmov == TK_CTL('c'))
		goto ret;
	if (vi_insmov == 127 && sb->s_n && sb->s[sb->s_n-1] == '\n')
		sb->s_n--;
	sbuf_null(sb)
	if (cmd[0] == 'a' && (beg + 1 <= lbuf_len(xb)))
		beg++;
	else if (cmd[0] == 'i')
		end = beg;
	if (ln && cmd[0] == 'c') {
		if (rstate->s == ln) {
			o1 = MIN(o1, rstate->n);
			e = rstate->chrs[o1];
		} else
			e = uc_chrn(ln, o1, &o1);
		if (vi_msg[0])
			lbuf_mark(xb, '*', beg, o1);
		xoff = o1;
		se = o2 > o1 ? uc_chr(e, o2 - o1) : e;
		ln = ex_lbufstr(ln, e, se, sb->s, sb->s_n);
		free(sb->s);
		sb->s = ln;
		if (!sb->s_n && se == e)
			goto ret;
	} else if (!(xvis & 2) && vi_insmov != 127)
		sbufn_chr(sb, '\n')
	else if (!sb->s_n)
		goto ret;
	o1 = lbuf_len(xb);
	lbuf_edit(xb, sb->s, beg, end);
	xrow = MIN(lbuf_len(xb) - 1, end + lbuf_len(xb) - o1 - 1);
	ret:
	free(sb->s);
	return NULL;
}

static void *ec_print(char *loc, char *cmd, char *arg)
{
	int i, beg, end, o1 = -1, o2 = -1;
	char *o, *ln;
	if (!cmd[0] && !loc[0] && arg[0])
		return "unknown command";
	if (ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	if (!cmd[0] && loc[0]) {
		xrow = MAX(beg, end - 1);
		return NULL;
	}
	rstate = rstates+1;
	for (i = beg; i < end; i++) {
		o = NULL;
		rstate->s = o;
		ln = lbuf_get(xb, i);
		if (o1 >= 0 && o2 >= 0 && beg == end-1)
			o = uc_sub(ln, o1, o2);
		else if (o1 >= 0 && i == beg)
			o = uc_sub(ln, o1, -1);
		else if (o2 >= 0 && i == end-1)
			o = uc_sub(ln, 0, o2);
		else {
			if (xvis & 4 && beg == end-1)
				xleft = ren_position(ln)->pos[MIN(xoff, rstate->n - 1)];
			ex_cprint(ln, msg_ft, -1, 0, 1);
			continue;
		}
		preserve(int, xleft, xleft = 0;)
		ex_cprint(o, msg_ft, -1, 0, 1);
		restore(xleft)
		free(o);
	}
	rstate--;
	xrow = MAX(beg, end - (cmd[0] || loc[0]));
	return NULL;
}

static void *ec_delete(char *loc, char *cmd, char *arg)
{
	int beg, end;
	if (ex_vregion(loc, &beg, &end) || !lbuf_len(xb))
		return xrerr;
	lbuf_edit(xb, NULL, beg, end);
	xrow = beg;
	return NULL;
}

void ex_regput(unsigned char c, const char *s, int append)
{
	sbuf *sb = xregs[tolower(c)];
	if (s) {
		if (!sb) {
			sbuf_make(sb, 64)
			xregs[tolower(c)] = sb;
		}
		if (!append)
			sbuf_cut(sb, 0)
		sbuf_str(sb, s)
		sbuf_set(sb, '\0', 4)
		sb->s_n -= 4;
	} else if (sb) {
		sbuf_free(sb)
		xregs[tolower(c)] = NULL;
	}
}

static void *ec_yank(char *loc, char *cmd, char *arg)
{
	int beg, end, o1 = 0, o2 = -1;
	if (cmd[2] == '!') {
		ex_regput(arg[0], NULL, 0);
		return NULL;
	} else if (ex_region(loc, &beg, &end, &o1, &o2) || !lbuf_len(xb))
		return xrerr;
	char *buf = lbuf_region(xb, beg, o1, end-1, o2);
	ex_regput(arg[0], buf, isupper((unsigned char) arg[0]) || arg[1]);
	free(buf);
	return NULL;
}

static void *ec_put(char *loc, char *cmd, char *arg)
{
	int beg, end, i = 0;
	sbuf *buf;
	if (arg[i] == '!' && arg[i+1] && arg[i+1] != ' ')
		buf = xregs[0];
	else
		buf = xregs[(unsigned char) arg[i++]];
	if (!buf)
		return "uninitialized register";
	for (; arg[i] && arg[i] != '!'; i++){}
	if (arg[i] == '!' && arg[i+1])
		return ex_pipeout(arg + i + 1, buf->s);
	int n = lbuf_len(xb), o1 = -1, o2 = -1;
	if (ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	if (o1 >= 0) {
		char *ln = lbuf_get(xb, end-1);
		char *e = uc_chr(ln, o1);
		char *p = ex_lbufstr(ln, e, e, buf->s, buf->s_n);
		lbuf_edit(xb, p, end-1, end);
		free(p);
	} else
		lbuf_edit(xb, buf->s, end, end);
	xrow = MIN(lbuf_len(xb) - 1, end + lbuf_len(xb) - n - 1);
	return NULL;
}

static void *ec_lnum(char *loc, char *cmd, char *arg)
{
	char msg[128];
	int beg, end, o1 = -1, o2 = -1;
	if (ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	sprintf(msg, "%d", o1 >= 0 ? o1 : end);
	ex_print(msg, msg_ft)
	return NULL;
}

static void *ec_undoredo(char *loc, char *cmd, char *arg)
{
	int ret = cmd[0] == 'u' ? lbuf_undo(xb) : lbuf_redo(xb);
	return ret ? xuerr : NULL;
}

static void *ec_save(char *loc, char *cmd, char *arg)
{
	lbuf_saved(xb, *arg);
	return NULL;
}

static void *ec_mark(char *loc, char *cmd, char *arg)
{
	int beg, end;
	if (ex_vregion(loc, &beg, &end))
		return xrerr;
	lbuf_mark(xb, (unsigned char) arg[0], end - 1, xoff);
	return NULL;
}

static void *ec_substitute(char *loc, char *cmd, char *arg)
{
	int beg, end, grp;
	char *pat, *rep = NULL, *_rep;
	char *s = arg;
	rset *rs = xkwdrs;
	int i, first = -1, last;
	if (ex_vregion(loc, &beg, &end))
		return xrerr;
	pat = re_read(&s);
	if (pat && (*pat || !rs))
		rs = rset_smake(pat, xic ? REG_ICASE : 0);
	if (!rs) {
		free(pat);
		return xserr;
	}
	if (pat && *s) {
		s--;
		rep = re_read(&s);
	}
	free(pat);
	int offs[rs->grpcnt * 2];
	for (i = beg; i < end; i++) {
		char *ln = lbuf_get(xb, i);
		sbuf *r = NULL;
		while (rset_find(rs, ln, offs, REG_NEWLINE) >= 0) {
			if (offs[xgrp] < 0) {
				ln += offs[1] > 0 ? offs[1] : 1;
				continue;
			} else if (!r)
				sbuf_make(r, 256)
			sbuf_mem(r, ln, offs[xgrp])
			if (rep) {
				for (_rep = rep; *_rep; _rep++) {
					if (*_rep != '\\' || !_rep[1]) {
						sbuf_chr(r, (unsigned char)*_rep)
						continue;
					}
					_rep++;
					grp = abs((*_rep - '0') * 2);
					if (grp + 1 >= rs->grpcnt * 2)
						sbuf_chr(r, (unsigned char)*_rep)
					else if (offs[grp] >= 0)
						sbuf_mem(r, ln + offs[grp], offs[grp + 1] - offs[grp])
				}
			}
			ln += offs[xgrp + 1];
			if (!offs[xgrp + 1])	/* zero-length match */
				sbuf_chr(r, (unsigned char)*ln++)
			if (*ln == '\n' || !*ln || !strchr(s, 'g'))
				break;
		}
		if (r) {
			if (first < 0) {
				first = i;
				lbuf_emark(xb, lbuf_opt(xb, NULL, xrow, 0), 0, 0);
			}
			sbufn_str(r, ln)
			lbuf_edit(xb, r->s, i, i + 1);
			sbuf_free(r)
			last = i;
		}
	}
	if (first >= 0)
		lbuf_emark(xb, lbuf_opt(xb, NULL, xrow, 0), first, last);
	if (rs != xkwdrs)
		rset_free(rs);
	free(rep);
	return NULL;
}

static void *ec_exec(char *loc, char *cmd, char *arg)
{
	int beg, end;
	char *text, *rep;
	if (!loc[0])
		return ex_pipeout(arg, NULL);
	if (ex_vregion(loc, &beg, &end))
		return xrerr;
	text = lbuf_cp(xb, beg, end);
	rep = cmd_pipe(arg, text, NULL, 1);
	if (rep)
		lbuf_edit(xb, rep, beg, end);
	free(text);
	free(rep);
	return NULL;
}

static void *ec_ft(char *loc, char *cmd, char *arg)
{
	xb_ft = syn_setft(arg[0] ? arg : xb_ft);
	ex_print(xb_ft, msg_ft)
	if (led_attsb) {
		sbuf_free(led_attsb)
		led_attsb = NULL;
	}
	syn_reload = 1;
	return NULL;
}

static void *ec_cmap(char *loc, char *cmd, char *arg)
{
	if (arg[0])
		xkmap_alt = conf_kmapfind(arg);
	else
		ex_print(conf_kmap(xkmap)[0], msg_ft)
	if (arg[0] && !strchr(cmd, '!'))
		xkmap = xkmap_alt;
	return NULL;
}

static void *ec_glob(char *loc, char *cmd, char *arg)
{
	int i, beg, end, not;
	char *pat, *s = arg;
	rset *rs;
	if (!loc[0] && !xgdep)
		loc = "%";
	if (ex_vregion(loc, &beg, &end))
		return xrerr;
	not = !!strchr(cmd, '!');
	pat = re_read(&s);
	if (pat && *pat)
		rs = rset_smake(pat, xic ? REG_ICASE : 0);
	else
		rs = rset_smake(xregs['/'] ? xregs['/']->s : "", xic ? REG_ICASE : 0);
	free(pat);
	if (!rs)
		return xserr;
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
	return NULL;
}

static void *ec_while(char *loc, char *cmd, char *arg)
{
	int beg = 1, addr = 0, skip[4];
	void *ret = NULL;
	static int _skip[4];
	memset(skip, 0, sizeof(skip));
	for (; *loc && addr < 3; addr++) {
		if (*loc == ',')
			loc++;
		if (!addr)
			beg = *loc == '$' ? INT_MAX : atoi(loc);
		else
			skip[addr] = *loc == '$' ? INT_MAX : atoi(loc);
		while (*loc && *loc != ',')
			loc++;
	}
	if (addr == 2)
		skip[2] = skip[1];
	for (; beg && !ret; beg--)
		ret = ex_exec(arg);
	return ret && addr > 1 ? memcpy(_skip, skip, sizeof(skip)) : NULL;
}

static void *ec_setdir(char *loc, char *cmd, char *arg)
{
	free(fs_exdir);
	fs_exdir = uc_dup(*arg ? arg : ".");
	if (cmd[1] == 'd')
		dir_calc(fs_exdir);
	return NULL;
}

static void *ec_chdir(char *loc, char *cmd, char *arg)
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
	return NULL;
	err:
	return "chdir error";
}

static void *ec_setincl(char *loc, char *cmd, char *arg)
{
	rset_free(fsincl);
	if (!*arg)
		fsincl = NULL;
	else if (!(fsincl = rset_smake(arg, xic ? REG_ICASE : 0)))
		return xserr;
	return NULL;
}

static void *ec_setacreg(char *loc, char *cmd, char *arg)
{
	if (xacreg)
		sbuf_free(xacreg)
	if (*arg) {
		sbuf_make(xacreg, 128)
		sbufn_str(xacreg, arg)
	} else
		xacreg = NULL;
	return NULL;
}

static void *ec_setbufsmax(char *loc, char *cmd, char *arg)
{
	xbufsmax = *arg ? atoi(arg) : xbufsalloc;
	if (xbufsmax <= 0)
		return xserr;
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
	return NULL;
}

static void *ec_regprint(char *loc, char *cmd, char *arg)
{
	static char buf[5] = "  ";
	preserve(int, xtd, xtd = 2;)
	for (int i = 1; i < LEN(xregs); i++) {
		if (xregs[i] && i != tolower(xpr)) {
			*buf = i;
			RS(2, ex_cprint(buf, msg_ft, -1, 0, 0))
			RS(2, ex_cprint(xregs[i]->s, msg_ft, -1, xleft ? 0 : 2, 1))
		}
	}
	restore(xtd)
	return NULL;
}

static void *ec_setenc(char *loc, char *cmd, char *arg)
{
	if (cmd[0] == 'p') {
		if (!*arg) {
			if (ph != _ph)
				free(ph);
			phlen = LEN(_ph);
			ph = _ph;
			return NULL;
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
		return NULL;
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
	return NULL;
}

static int eo_val(char *arg)
{
	int val = atoi(arg);
	if (!val && !isdigit((unsigned char)*arg))
		return (unsigned char)*arg;
	return val;
}

#define _EO(opt, inner) \
static void *eo_##opt(char *loc, char *cmd, char *arg) { inner }

#define EO(opt) \
	_EO(opt, x##opt = !*arg ? !x##opt : eo_val(arg); return NULL;)

EO(pac) EO(pr) EO(ai) EO(ish) EO(ic) EO(grp) EO(shape) EO(seq)
EO(sep) EO(tbs) EO(td) EO(order) EO(hll) EO(hlw) EO(hlp) EO(hlr)
EO(hl) EO(lim) EO(led) EO(vis) EO(mpt)

_EO(left,
	if (*loc)
		xleft = (xcols / 2) * atoi(loc);
	else
		xleft = *arg ? atoi(arg) : 0;
	return NULL;
)

#undef EO
#define EO(opt) {#opt, eo_##opt}

/* commands & opts must be sorted longest of its kind topmost */
static struct excmd {
	char *name;
	void *(*ec)(char *loc, char *cmd, char *arg);
} excmds[] = {
	{"@", ec_termexec},
	{"&", ec_termexec},
	{"!", ec_exec},
	{"bp", ec_setpath},
	{"bs", ec_save},
	{"bx", ec_setbufsmax},
	{"b", ec_buffer},
	EO(pac),
	EO(pr),
	{"pu", ec_put},
	{"ph", ec_setenc},
	{"p", ec_print},
	EO(ai),
	{"ac", ec_setacreg},
	{"a", ec_insert},
	{"ea!", ec_editapprox},
	{"ea", ec_editapprox},
	{"e!", ec_edit},
	{"e", ec_edit},
	{"ft", ec_ft},
	{"fd", ec_setdir},
	{"fp", ec_setdir},
	{"f+", ec_find},
	{"f", ec_find},
	EO(ish),
	{"inc", ec_setincl},
	EO(ic),
	{"i", ec_insert},
	{"d", ec_delete},
	EO(grp),
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
	{"wl", ec_while},
	{"w!", ec_write},
	{"w", ec_write},
	{"uc", ec_setenc},
	{"uz", ec_setenc},
	{"ub", ec_setenc},
	{"u", ec_undoredo},
	EO(shape),
	EO(seq),
	EO(sep),
	{"s", ec_substitute},
	{"x!", ec_write},
	{"x", ec_write},
	{"ya!", ec_yank},
	{"ya", ec_yank},
	{"cm!", ec_cmap},
	{"cm", ec_cmap},
	{"cd", ec_chdir},
	{"c", ec_insert},
	EO(tbs),
	EO(td),
	EO(order),
	EO(hll),
	EO(hlw),
	EO(hlp),
	EO(hlr),
	EO(hl),
	EO(left),
	EO(lim),
	EO(led),
	EO(vis),
	EO(mpt),
	{"=", ec_lnum},
	{"", ec_print}, /* do not remove */
};

/* parse ex command until xsep or null. */
static const char *ex_parse(const char *src, char *loc, int *idx, char *arg)
{
	int i, j;
	while (*src == xsep || *src == ' ' || *src == '\t')
		src++;
	while (memchr(" \t0123456789+-.,/?$';%", *src, 22)) {
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
void *ex_exec(const char *ln)
{
	int idx = 0, len = strlen(ln) + 1;
	int r1 = -1, r2 = -1;
	char loc[len], arg[len], *ecmd, *ret = NULL;
	sbuf_smake(sb, 1024)
	for (int i = 0; *ln; i++) {
		ln = ex_parse(ln, loc, &idx, arg);
		if (i >= r1 && i <= r2)
			continue;
		if ((ecmd = ex_pathexpand(sb, arg))) {
			ret = excmds[idx].ec(loc, excmds[idx].name, ecmd);
			if (ret && !*ret) {
				r1 = ((int*)ret)[1] + i;
				r2 = ((int*)ret)[2] + i;
			} else if (ret && ret != xuerr)
				ex_print(ret, msg_ft)
		}
		sbuf_cut(sb, 0)
	}
	free(sb->s);
	return ret;
}

/* ex main loop */
void ex(void)
{
	xgrec++;
	int esc = 0;
	sbuf_smake(sb, xcols)
	while (!xquit) {
		if (!ex_read(sb, ":", ex_ft, 0, 1)) {
			if (!strcmp(sb->s, ":") && esc)
				ex_exec(xregs[':']->s);
			else
				ex_command(sb->s)
			xb->useq += xseq;
			esc = 1;
		} else
			esc = 0;
		sbuf_cut(sb, 0)
	}
	free(sb->s);
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
