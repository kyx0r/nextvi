int xleft;			/* the first visible column */
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
int xts = 8;			/* number of spaces for tab */
int xish;			/* interactive shell */
int xgrp;			/* regex search group */
int xpac;			/* print autocomplete options */
int xmpt;			/* whether to prompt after printing > 1 lines in vi */
int xpr;			/* ex_cprint register */
int xlim = -1;			/* rendering cutoff for non cursor lines */
int xseq = 1;			/* undo/redo sequence */
int xerr = 1;			/* error handling -
				bit 1: print errors, bit 2: early return, bit 3: ignore errors */
int xrcm = 1;			/* range command model -
				0: exec at command parse 1: exec at command */

int xquit;			/* exit if positive, force quit if negative */
int xrow, xoff, xtop;		/* current row, column, and top row */
int xbufcur;			/* number of active buffers */
int xgrec;			/* global vi/ex recursion depth */
int xkmap;			/* the current keymap */
int xkmap_alt = 1;		/* the alternate keymap */
int xkwddir;			/* the last search direction */
int xkwdcnt;			/* number of search kwd changes */
int xpln;			/* tracks newline from ex print and pipe stdout */
int xsep = ':';			/* ex command separator */
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
static int xexp = '%';		/* ex command internal state expand  */
static int xexe = '!';		/* ex command external command expand */
static char xuerr[] = "unreported error";
static char xserr[] = "syntax error";
static char xirerr[] = "invalid range";
static char xrnferr[] = "range not found";
static char *xrerr;

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

void temp_switch(int i, int swap)
{
	if (ex_buf == &tempbufs[i]) {
		if (swap) {
			exbuf_save(ex_buf)
			ex_buf = ex_pbuf;
			ex_pbuf = ex_tpbuf;
		}
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
	lbuf_edit(lb, str, tempbufs[i].row, tempbufs[i].row, 0, 0);
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

static int ex_range(char *ploc, char **num, int n, int *row)
{
	int dir, off, beg, end, adj = 0;
	while (**num == ' ' || **num == '\t')
		++*num;
	switch ((unsigned char)**num) {
	case '.':
		++*num;
		break;
	case '%':
		if (ploc != *num)
			break;
	case '$':
		n = row ? lbuf_eol(xb, *row, 2) : lbuf_len(xb) - 1;
		++*num;
		break;
	case '\'':
		if (lbuf_jump(xb, (unsigned char)*++(*num),
				&n, row ? &n : &dir))
			return -1;
		++*num;
		break;
	case '>':
	case '<':
		dir = **num == '>' ? 2 : -2;
		off = row ? n : 0;
		beg = row ? *row : n + (dir > 0);
		end = row ? beg+1 : lbuf_len(xb);
		if (off < 0 || beg < 0 || beg >= lbuf_len(xb))
			return -1;
		char *e = re_read(num);
		ex_krsset(e, dir);
		free(e);
		if (!xkwdrs) {
			xrerr = xserr;
			return -1;
		}
		if (lbuf_search(xb, xkwdrs, xkwddir, row ? beg : 0, end,
				MIN(dir, 0), !row, &beg, &off)) {
			xrerr = xrnferr;
			return -1;
		}
		n = row ? off : beg;
		break;
	default:
		if (isdigit((unsigned char)**num)) {
			adj = !row;
			n = atoi(*num);
			while (isdigit((unsigned char)**num))
				++*num;
		}
	}
	while (**num) {
		dir = atoi(*num+1);
		if (**num == '-')
			n -= dir;
		else if (**num == '+')
			n += dir;
		else if (**num == '*')
			n *= dir;
		else if (**num == '/' && dir)
			n /= dir;
		else if (**num == '%' && dir)
			n %= dir;
		else
			break;
		for (++*num; isdigit((unsigned char)**num);)
			++*num;
	}
	return n - adj;
}

/* parse ex command addresses */
#define ex_vregion(loc, beg, end) ex_region(loc, beg, end, &xoff, &xoff)
static int ex_region(char *loc, int *beg, int *end, int *o1, int *o2)
{
	int vaddr = *loc == '%', haddr = 0, update = 0, row = xrow, ooff = xoff;
	char *ploc = loc;
	xrerr = xirerr;
	if (vaddr)
		*beg = 0;
	while (*loc) {
		if (*loc == '|') {
			char *cmd = ++loc;
			for (; *loc && *loc != '|'; loc++)
				if (*loc == '\\' && loc[1] == '|')
					loc++;
			int nulled = *loc;
			*loc = '\0';
			void *err = ex_exec(cmd);
			if (nulled)
				*loc++ = '|';
			if (err) {
				xrerr = "ex command error";
				return 1;
			}
			continue;
		} else if (*loc == ';') {
			update = loc[1] == '#';
			loc += 1 + update;
			if ((ooff = ex_range(ploc, &loc, update ? ooff : xoff, &row)) < 0)
				return 1;
			if (haddr++ % 2)
				*o2 = ooff;
			else
				*o1 = ooff;
		} else {
			if (*loc == ',') {
				update = loc[1] == '#';
				loc += 1 + update;
			}
			row = ex_range(ploc, &loc, update ? row : xrow, NULL);
			if (vaddr++ % 2)
				*end = row + 1;
			else
				*beg = row;
		}
		while (*loc && *loc != '|' && *loc != ';' && *loc != ',')
		        loc++;
	}
	if (!vaddr) {
		*beg = xrow;
		*end = MIN(lbuf_len(xb), *beg + 1);
	} else if (vaddr == 1)
		*end = *beg + 1;
	return *beg < 0 || *beg >= lbuf_len(xb) || *end <= *beg || *end > lbuf_len(xb);
}

static int ex_read(sbuf *sb, char *msg, ins_state *is, int ps, int flg)
{
	int n = sb->s_n, key;
	if (xvis & 2) {
		while ((key = term_read(0)) != '\n') {
			sbuf_chr(sb, key)
			if (flg & 2 || xquit)
				break;
		}
		sbuf_null(sb)
		return key;
	}
	sbuf_str(sb, msg)
	key = led_prompt(sb, NULL, &xkmap, is, ps, flg);
	if (key == '\n' && (!*msg || strcmp(sb->s + n, msg)))
		term_chr('\n');
	return key;
}

#define readfile(errchk) \
fd = open(xb_path, O_RDONLY); \
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
	readfile()
	return 0;
}

static void *ec_edit(char *loc, char *cmd, char *arg)
{
	char msg[512];
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
	readfile(rd =)
	if (cd == 3 || (!rd && fd >= 0)) {
		ex_bufpostfix(ex_buf, arg[0]);
		syn_setft(xb_ft);
	}
	snprintf(msg, sizeof(msg), "\"%s\" %dL [%c]",
			*xb_path ? xb_path : "unnamed", lbuf_len(xb),
			fd < 0 || rd ? 'f' : 'r');
	if (!(xvis & 8))
		ex_print(msg, bar_ft)
	return (fd < 0 || rd) && *arg ? xuerr : NULL;
}

static void *ec_fuzz(char *loc, char *cmd, char *arg)
{
	rset *rs;
	char *path, *p, buf[128], trunc[128], *sret = NULL;
	int c, pos, subs[2], inst = -1, lnum = -1;
	int beg, end, max = INT_MAX, dwid1, dwid2;
	int flg = REG_NEWLINE | REG_NOCAP;
	int pflg = ((xvis & 4) == 0) * 2;
	ins_state is;
	ins_init(is)
	if (*cmd !='f')
		temp_switch(1, 0);
	if (ex_vregion(loc, &beg, &end)) {
		if (*cmd !='f')
			temp_switch(1, 1);
		return xrerr;
	}
	if (!*loc) {
		beg = 0;
		end = lbuf_len(xb);
		max = xrows ? xrows * 3 : end;
	}
	snprintf(trunc, sizeof(trunc), "truncated to %d lines", max);
	dwid1 = snprintf(NULL, 0, "%d", max - 1);
	sbuf_smake(sb, 128)
	sbuf_smake(fuzz, 16)
	sbuf_smake(cmdbuf, 16)
	sbuf_str(fuzz, arg)
	syn_setft(fuzz_ft);
	while(1) {
		sbuf_null(fuzz)
		c = 0;
		rs = rset_smake(fuzz->s, xic ? flg | REG_ICASE : flg);
		if (rs) {
			syn_reloadft(syn_addhl(fuzz->s, 1), rs->regex->flg);
			term_record = !!term_sbuf;
			end = MIN(end, lbuf_len(xb));
			dwid2 = snprintf(NULL, 0, "%d", end);
			dwid1 = max == INT_MAX ? dwid2 : MIN(dwid1, dwid2);
			for (pos = beg; c < max && pos < end; pos++) {
				path = xb->ln[pos];
				if (rset_match(rs, path, 0)) {
					sbuf_mem(sb, &pos, (int)sizeof(pos))
					p = itoa(c++, buf);
					int z, wid = p - buf;
					for (z = dwid1 + 1 - wid; z; z--)
						*p++ = ' ';
					wid = snprintf(NULL, 0, "%d", pos+1);
					for (z = dwid2 - wid; z; z--)
						*p++ = ' ';
					p = itoa(pos+1, p);
					ex_cprint2(buf, msg_ft, -1, 0, 0, pflg)
					ex_cprint2(path, NULL, -1, (p - buf) + 1, 0, !pflg)
				}
			}
			if (c == max && c != end)
				ex_cprint2(trunc, msg_ft, -1, 0, 0, pflg)
			if (pflg && c)
				term_chr('\n');
			if (term_record)
				term_commit();
		}
		if ((inst = ex_read(fuzz, "", &is, 0, 2)) == '\n' && c) {
			if (c == 1)
				break;
			if ((inst = ex_read(cmdbuf, "", NULL, 0, 0)) == '\n') {
				inst = atoi(cmdbuf->s);
				break;
			}
		}
		if (TK_INT(inst))
			goto ret;
		if (c && c < 11 && isdigit(inst)) {
			inst -= '0';
			if (inst < c) {
				fuzz->s_n--;
				break;
			}
		}
		rset_free(rs);
		sbuf_cut(sb, 0)
		if (pflg) {
			term_clean();
			term_pos(xrows, 0);
		} else if (c)
			ex_print("", NULL)
	}
	if ((inst >= 0 && inst < c) || c == 1)
		lnum = *((int*)sb->s + (c == 1 ? 0 : inst));
	ret:
	syn_setft(xb_ft);
	if (fuzz->s_n > 0) {
		sbuf_cut(cmdbuf, 0)
		sbuf_str(cmdbuf, loc)
		sbuf_str(cmdbuf, cmd)
		sbuf_chr(cmdbuf, ' ')
		sbufn_mem(cmdbuf, fuzz->s, fuzz->s_n)
		lbuf_dedup(tempbufs[0].lb, cmdbuf->s, cmdbuf->s_n)
		temp_pos(0, -1, 0, 0);
		temp_write(0, cmdbuf->s);
	}
	free(cmdbuf->s);
	free(fuzz->s);
	free(sb->s);
	path = lbuf_get(xb, lnum);
	if (*cmd == 'f' && path) {
		rset_find(rs, path, subs, 0);
		xrow = lnum;
		xoff = uc_off(path, subs[0]);
	} else if (path) {
		path[lbuf_s(path)->len] = '\0';
		sret = ec_edit(loc, cmd, path);
		path[lbuf_s(path)->len] = '\n';
	} else if (*cmd != 'f')
		temp_switch(1, 1);
	rset_free(rs);
	return sret;
}

static void *ec_find(char *loc, char *cmd, char *arg)
{
	int pskip, dir, off, nbeg, beg, end;
	if (ex_vregion(loc, &beg, &end))
		return xrerr;
	dir = cmd[1] == '+' || cmd[1] == '>' ? 2 : -2;
	ex_krsset(arg, dir);
	if (!xkwdrs)
		return xserr;
	off = xoff;
	if (xrow < beg || xrow >= end) {
		off = 0;
		nbeg = dir > 0 ? beg : end - (cmd[1] == '<');
		end += cmd[1] == '-';
		pskip = -1;
	} else {
		nbeg = xrow;
		pskip = cmd[1] == '+' ? 1 : MIN(dir, 0);
	}
	if (lbuf_search(xb, xkwdrs, xkwddir, beg, end,
			pskip, cmd[1] == '-', &nbeg, &off))
		return xuerr;
	xrow = nbeg;
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
			temp_switch(abs(atoi(arg))-1, 1);
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
	xquit = strchr(cmd, '!') ? -1 : !xquit ? 1 : xquit;
	return NULL;
}

void ex_bufpostfix(struct buf *p, int clear)
{
	p->mtime = mtime(p->path);
	p->ft = syn_filetype(p->path);
	lbuf_saved(p->lb, clear);
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
	sbuf obuf, *sb;
	char msg[512];
	char *path, *ret = NULL;
	int beg = 0, end = 0, o1 = 0, o2 = -1;
	int row = xrow, off = xoff, fd = -1;
	struct lbuf *lb = lbuf_make(), *pxb = xb;
	path = arg[0] ? arg : xb_path;
	if (arg[0] == '!') {
		if ((sb = cmd_pipe(arg + 1, NULL, 1, NULL))) {
			lbuf_edit(lb, sb->s, 0, 0, 0, 0);
			sbuf_free(sb)
		}
	} else {
		if ((fd = open(path, O_RDONLY)) < 0) {
			ret = "open failed";
			goto err;
		}
		if (lbuf_rd(lb, fd, 0, 0)) {
			ret = "read failed";
			goto err;
		}
	}
	xb = lb;
	xrow = 0;
	xoff = 0;
	if (lbuf_len(lb) && ex_region(loc, &beg, &end, &o1, &o2)) {
		ret = xrerr;
		goto err;
	} else if (!*loc) {
		beg = 0;
		end = lbuf_len(lb);
	}
	lbuf_region(lb, &obuf, beg, o1, end-1, o2);
	lbuf_edit(pxb, obuf.s, row, row, 0, 0);
	free(obuf.s);
	snprintf(msg, sizeof(msg), "\"%s\" %dL [r]",
			path, lbuf_len(pxb) - lbuf_len(lb));
	ex_print(msg, bar_ft)
	err:
	lbuf_free(lb);
	xrow = row;
	xoff = off;
	xb = pxb;
	if (fd >= 0)
		close(fd);
	return ret;
}

static void *ex_pipeout(char *cmd, sbuf *buf)
{
	int ret = 0;
	if (!(xvis & 4) && xmpt >= 0 && !xpln) {
		term_chr('\n');
		xpln = 1;
		xmpt = 2;
	} else if (xvis & 4 && xpln == 2) {
		term_chr('\n');
		xpln = 0;
	}
	cmd_pipe(cmd, buf, 0, &ret);
	return ret ? xuerr : NULL;
}

static void *ec_write(char *loc, char *cmd, char *arg)
{
	char msg[512], *path;
	sbuf ibuf;
	int fd, beg = 0, end = 0, o1 = -1, o2 = -1;
	path = arg[0] ? arg : xb_path;
	if (cmd[0] == 'x' && !xb->modified)
		return ec_quit("", cmd, "");
	if (lbuf_len(xb) && ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	if (!*loc) {
		beg = 0;
		end = lbuf_len(xb);
	}
	if (arg[0] == '!') {
		lbuf_region(xb, &ibuf, beg, MAX(0, o1), end-1, o2);
		ex_pipeout(arg + 1, &ibuf);
		free(ibuf.s);
	} else {
		if (!strchr(cmd, '!')) {
			if (!strcmp(xb_path, path) && mtime(path) > ex_buf->mtime)
				return "write failed: file changed";
			if (arg[0] && mtime(path) >= 0)
				return "write failed: file exists";
		}
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, conf_mode);
		if (fd < 0)
			return "write failed: cannot create file";
		if (o1 >= 0) {
			lbuf_region(xb, &ibuf, beg, o1, end-1, o2);
			o1 = write(fd, ibuf.s, ibuf.s_n);
			free(ibuf.s);
		} else
			o1 = lbuf_wr(xb, fd, beg, end);
		close(fd);
		if (o1 < 0)
			return "write failed";
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

void ex_cprint(char *line, char *ft, int r, int c, int left, int flg)
{
	if (xpr) {
		ex_regput(xpr, line, 1);
		if (flg & 1 && isupper(xpr) &&
				xregs[xpr] && xregs[xpr]->s_n &&
				xregs[xpr]->s[xregs[xpr]->s_n-1] != '\n')
			ex_regput(xpr, "\n", 1);
	}
	if (xvis & 2) {
		term_write(line, dstrlen(line, '\n'))
		term_write("\n", 1)
		return;
	}
	syn_blockhl = -1;
	if (flg && !(xvis & 4)) {
		term_pos(xrows, 0);
		if ((!xpln && xmpt > 0) || flg == 2)
			term_chr('\n');
		xmpt += xmpt >= 0 && flg == 1;
	}
	xpln = 0;
	preserve(int, ftidx,)
	if (ft)
		syn_setft(ft);
	led_crender(line, r, c, left, left + xcols - c)
	restore(ftidx)
	if (flg && xvis & 4)
		term_chr('\n');
}

static void *ec_insert(char *loc, char *cmd, char *arg)
{
	int key, beg = 0, end = 0, o1 = -1, o2 = -1, ps = 0;
	if (lbuf_len(xb) && ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	sbuf_smake(sb, 128)
	if (*arg)
		term_push(arg, strlen(arg));
	while (1) {
		syn_setft(msg_ft);
		if ((key = ex_read(sb, "", NULL, ps, 0)) != '\n')
			break;
		if (xvis & 2 && !strcmp(".", sb->s + ps)) {
			sb->s_n--;
			break;
		}
		sbuf_chr(sb, '\n')
		ps = sb->s_n;
	}
	syn_setft(xb_ft);
	if (key == TK_CTL('c'))
		goto ret;
	if (key == 127 && sb->s_n && sb->s[sb->s_n-1] == '\n')
		sb->s_n--;
	sbuf_null(sb)
	if (cmd[0] == 'a' && (beg + 1 <= lbuf_len(xb)))
		beg++;
	else if (cmd[0] == 'i')
		end = beg;
	if (o1 >= 0 && cmd[0] == 'c') {
		if (!sb->s_n && o2 <= o1)
			goto ret;
		char *p = lbuf_joinsb(xb, beg, end-1, sb, &o1, &o2);
		o1 -= sb->s[0] == '\n';
		free(sb->s);
		sb->s = p;
	} else if (!(xvis & 2) && key != 127)
		sbufn_chr(sb, '\n')
	else if (!sb->s_n)
		goto ret;
	ps = lbuf_len(xb);
	lbuf_edit(xb, sb->s, beg, end, o1, o2);
	xrow = MIN(lbuf_len(xb) - 1, end + lbuf_len(xb) - ps - 1);
	if (o1 >= 0)
		xoff = o1;
	ret:
	free(sb->s);
	return NULL;
}

static void *ec_print(char *loc, char *cmd, char *arg)
{
	int i, beg, end, o1 = -1, o2 = -1;
	char *o, *ln;
	if (!*cmd && !*loc && *arg)
		return "unknown command";
	if (ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	if (o1 >= 0)
		xoff = MAX(o1, o2);
	if (!*cmd && *loc) {
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
			ex_cprint(ln, msg_ft, -1, 0, *loc ? 0 : xleft, 1);
			continue;
		}
		ex_cprint(o, msg_ft, -1, 0, 0, 1);
		free(o);
	}
	rstate--;
	xrow = MAX(beg, end - (cmd[0] || loc[0]));
	return NULL;
}

static void *ec_delete(char *loc, char *cmd, char *arg)
{
	int beg, end, o1 = -1, o2 = -1;
	sbuf sb;
	char *p = NULL;
	if (ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	if (o1 >= 0) {
		sb.s = "";
		sb.s_n = 0;
		p = lbuf_joinsb(xb, beg, end-1, &sb, &o1, &o2);
		xoff = o1;
	}
	lbuf_edit(xb, p, beg, end, o1, o2);
	free(p);
	xrow = beg;
	return NULL;
}

void ex_regput(unsigned char c, const char *s, int append)
{
	sbuf *sb = xregs[c];
	if (s) {
		if (!sb) {
			sbuf_make(sb, 64)
			xregs[c] = sb;
		}
		if (!append)
			sbuf_cut(sb, 0)
		sbuf_str(sb, s)
		sbufn_null(sb)
	} else if (sb) {
		sbuf_free(sb)
		xregs[c] = NULL;
	}
}

static void *ec_yank(char *loc, char *cmd, char *arg)
{
	int beg, end, o1 = 0, o2 = -1;
	if (cmd[2] == '!') {
		ex_regput(*arg, NULL, 0);
		return NULL;
	} else if (ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	sbuf sb;
	lbuf_region(xb, &sb, beg, o1, end-1, o2);
	ex_regput(*arg, sb.s, *arg && arg[1]);
	free(sb.s);
	return NULL;
}

static void *ec_put(char *loc, char *cmd, char *arg)
{
	int beg = 0, end = 0, i = 0;
	sbuf *buf;
	if (!*arg || (arg[i] == '!' && arg[i+1] && arg[i+1] != ' '))
		buf = xregs[i];
	else
		buf = xregs[(unsigned char)arg[i++]];
	if (!buf)
		return "uninitialized register";
	for (; arg[i] && arg[i] != '!'; i++){}
	if (arg[i] == '!' && arg[i+1])
		return ex_pipeout(arg + i + 1, buf);
	int n = lbuf_len(xb), o1 = -1, o2 = -1;
	if (n && ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	if (o1 >= 0 && n) {
		char *p = lbuf_joinsb(xb, end-1, end-1, buf, &o1, &o2);
		lbuf_edit(xb, p, end-1, end, o1, o1);
		free(p);
	} else
		lbuf_edit(xb, buf->s, end, end, o1, o1);
	xrow = MIN(lbuf_len(xb) - 1, end + lbuf_len(xb) - n - 1);
	return NULL;
}

static void *ec_num(char *loc, char *cmd, char *arg)
{
	char msg[128];
	int arr[4] = {0, 0, -1, -1};
	int ret = ex_region(loc, &arr[0], &arr[1], &arr[2], &arr[3]);
	if (ret && !((*arg && arg[1]) || (*arg && (*arg ^ '0') >= 4)))
		return xrerr;
	if ((*arg ^ '0') < 4)
		itoa(arr[*arg ^ '0'], msg);
	else
		sprintf(msg, "%d %d %d %d", arr[0], arr[1], arr[2], arr[3]);
	ex_print(msg, msg_ft)
	return NULL;
}

static void *ec_undoredo(char *loc, char *cmd, char *arg)
{
	int ref;
	if (cmd[0] == 'u')
		return lbuf_undo(xb, &ref, &ref) ? xuerr : NULL;
	return lbuf_redo(xb, &ref, &ref) ? xuerr : NULL;
}

static void *ec_bufsave(char *loc, char *cmd, char *arg)
{
	lbuf_saved(xb, *arg);
	return NULL;
}

static void *ec_mark(char *loc, char *cmd, char *arg)
{
	int beg, end, o1 = xoff, o2 = xoff;
	if (ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	for (int i = 0; *arg; i++)
		lbuf_mark(xb, (unsigned char)*arg++,
			i % 2 ? end - 1 : beg, i % 2 ? o2 : o1);
	return NULL;
}

static void *ec_substitute(char *loc, char *cmd, char *arg)
{
	int beg, end, grp;
	char *pat, *rep = NULL, *_rep;
	char *s = arg;
	rset *rs = xkwdrs;
	int i, first = -1, last;
	struct lopt *lo;
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
	int offs[rs->nsubc];
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
					if (grp + 1 >= rs->nsubc)
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
				lo = lbuf_opt(xb, xrow, xoff, 0);
				lbuf_smark(xb, lo, i, 0);
				lbuf_emark(xb, lo, 0, 0);
			}
			sbufn_str(r, ln)
			lbuf_edit(xb, r->s, i, i + 1, 0, 0);
			sbuf_free(r)
			last = i;
		}
	}
	if (first >= 0) {
		lo = lbuf_opt(xb, xrow, xoff, 0);
		lbuf_smark(xb, lo, first, 0);
		lbuf_emark(xb, lo, last, 0);
	}
	if (rs != xkwdrs)
		rset_free(rs);
	free(rep);
	return NULL;
}

static void *ec_exec(char *loc, char *cmd, char *arg)
{
	int beg = 0, end = 0, o1 = 0, o2 = -1;
	if (!loc[0])
		return ex_pipeout(arg, NULL);
	if (lbuf_len(xb) && ex_region(loc, &beg, &end, &o1, &o2))
		return xrerr;
	sbuf text;
	lbuf_region(xb, &text, beg, o1, end-1, o2);
	sbuf *rep = cmd_pipe(arg, &text, 1, NULL);
	free(text.s);
	if (!rep)
		return NULL;
	if (o1 > 0) {
		char *p = lbuf_joinsb(xb, beg, end-1, rep, &o1, &o2);
		lbuf_edit(xb, p, beg, end, o1, o2);
		free(p);
	} else
		lbuf_edit(xb, rep->s, beg, end, o1, o2);
	sbuf_free(rep)
	return NULL;
}

static void *ec_ft(char *loc, char *cmd, char *arg)
{
	if (!(loc = syn_setft(*arg ? arg : xb_ft)))
		return "filetype not found";
	xb_ft = loc;
	ex_print(xb_ft, msg_ft)
	if (led_attsb) {
		sbuf_free(led_attsb)
		led_attsb = NULL;
	}
	for (int i = 1; i < 4; i++)
		syn_reloadft(syn_findhl(i), 0);
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
		if (rset_match(rs, ln, REG_NEWLINE) != not) {
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
	int beg = 1, addr = 0, skip[3] = {0, 1, 1};
	void *ret = NULL;
	static int _skip[3];
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
	return !ret || addr == 1 ? NULL : memcpy(_skip, skip, sizeof(skip));
}

static void *ec_join(char *loc, char *cmd, char *arg)
{
	int beg, end, o2 = 0;
	if (ex_vregion(loc, &beg, &end))
		return xrerr;
	lbuf_join(xb, beg, end+1, xoff, &o2, arg[0]);
	xrow = beg;
	return NULL;
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
	oldpath[0] = '\0';
	oldpath[sizeof(oldpath)-1] = '\0';
	if (!getcwd(oldpath, sizeof(oldpath)))
		if ((opath = getenv("PWD")))
			strncpy(oldpath, opath, sizeof(oldpath)-1);
	plen = strlen(oldpath);
	i = plen == sizeof(oldpath)-1;
	if (chdir(*arg ? arg : oldpath))
		return "chdir error";
	if (!getcwd(newpath, sizeof(newpath)))
		return "getcwd error";
	setenv("PWD", newpath, 1);
	if (i)
		return "oldpath >= 4096";
	if (plen && oldpath[plen-1] != '/')
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
	int flg = (xvis & 4) == 0;
	preserve(int, xtd, xtd = 2;)
	for (int i = 1; i < LEN(xregs); i++) {
		if (xregs[i] && i != xpr) {
			*buf = i;
			ex_cprint2(buf, msg_ft, -1, 0, 0, flg)
			ex_cprint2(xregs[i]->s, msg_ft, -1, xleft ? 0 : 2, xleft, !flg)
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

static void *ec_specials(char *loc, char *cmd, char *arg)
{
	if (!*arg) {
		xsep = cmd[2] ? 0 : ':';
		xexp = cmd[2] ? 0 : '%';
		xexe = cmd[2] ? 0 : '!';
		return NULL;
	}
	int i = *loc ? atoi(loc) : 0;
	for (; *arg; arg++, i++) {
		if (i == 0)
			xsep = *arg;
		else if (i == 1)
			xexp = *arg;
		else if (i == 2)
			xexe = *arg;
	}
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

EO(pac) EO(pr) EO(ai) EO(err) EO(ish) EO(ic) EO(grp) EO(mpt) EO(rcm)
EO(shape) EO(seq) EO(ts) EO(td) EO(order) EO(hll) EO(hlw)
EO(hlp) EO(hlr) EO(hl) EO(lim) EO(led) EO(vis)

_EO(left,
	if (*loc)
		xleft = (xcols / 2) * atoi(loc);
	else if (*arg)
		xleft = atoi(arg);
	else if (lbuf_get(xb, xrow))
		xleft = ren_position(lbuf_get(xb, xrow))->pos[MIN(xoff, rstate->n)];
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
	{"?", ec_while},
	{"bp", ec_setpath},
	{"bs", ec_bufsave},
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
	EO(err),
	{"ef!", ec_fuzz},
	{"ef", ec_fuzz},
	{"e!", ec_edit},
	{"e", ec_edit},
	{"ft", ec_ft},
	{"fd", ec_setdir},
	{"fp", ec_setdir},
	{"f+", ec_find},
	{"f-", ec_find},
	{"f>", ec_find},
	{"f<", ec_find},
	{"f", ec_fuzz},
	EO(ish),
	{"inc", ec_setincl},
	EO(ic),
	{"i", ec_insert},
	{"d", ec_delete},
	EO(grp),
	{"g!", ec_glob},
	{"g", ec_glob},
	EO(mpt),
	{"m", ec_mark},
	{"q!", ec_quit},
	{"q", ec_quit},
	EO(rcm),
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
	EO(shape),
	EO(seq),
	{"sc!", ec_specials},
	{"sc", ec_specials},
	{"s", ec_substitute},
	{"x!", ec_write},
	{"x", ec_write},
	{"ya!", ec_yank},
	{"ya", ec_yank},
	{"cm!", ec_cmap},
	{"cm", ec_cmap},
	{"cd", ec_chdir},
	{"c", ec_insert},
	{"j", ec_join},
	EO(ts),
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
	{"=", ec_num},
	{"", ec_print}, /* do not remove */
};

/* parse command argument expanding % and ! */
static const char *ex_arg(const char *src, sbuf *sb, int *arg)
{
	*arg = sb->s_n;
	while (*src && *src != xsep) {
		if (*src == xexp) {
			src++;
			if (*src == '@' && src[1] && src[1] != '\\') {
				sbuf *reg = xregs[(unsigned char)src[1]];
				if (reg)
					sbuf_str(sb, reg->s)
				src += 2;
			} else {
				struct buf *pbuf = ex_buf;
				int n;
				if (*src == '#') {
					src++;
					pbuf = ex_pbuf;
				} else if ((*src ^ '0') < 10) {
					pbuf = &bufs[n = atoi(src)];
					src += snprintf(NULL, 0, "%d", n);
				}
				src += *src == '\\' && src[-1] != '#' && (src[1] ^ '0') < 10;
				if (pbuf >= bufs && pbuf < &bufs[xbufcur] && pbuf->path[0])
					sbuf_str(sb, pbuf->path)
			}
		} else if (*src == xexe) {
			int n = sb->s_n;
			src++;
			while (*src && *src != xexe) {
				if (*src == '\\' && src[1] == xexe)
					src++;
				sbuf_chr(sb, *src++)
			}
			src += *src ? 1 : 0;
			sbuf_null(sb)
			sbuf_cut(sb, n)
			sbuf *str = cmd_pipe(sb->s + n, NULL, 1, NULL);
			if (str) {
				sbuf_mem(sb, str->s, str->s_n)
				sbuf_free(str)
			}
		} else {
			if (*src == '\\' && (src[1] == xsep || src[1] == xexp
					|| src[1] == xexe) && src[1])
				src++;
			sbuf_chr(sb, *src++)
		}
	}
	sbuf_null(sb)
	return src;
}

/* parse range and command */
static const char *ex_cmd(const char *src, sbuf *sb, int *idx)
{
	int i, j;
	char *dst = sb->s, *pdst, *err;
	while (*src && (*src == xsep || *src == ' ' || *src == '\t'))
		src++;
	while (memchr(" \t0123456789+-.,<>/$';%*#|", *src, 26)) {
		if (*src == '\'' && src[1])
			*dst++ = *src++;
		if (*src == '>' || *src == '<' || *src == '|') {
			pdst = dst;
			j = *src;
			do {
				*dst++ = *src++;
			} while (*src && (*src != j || src[-1] == '\\'));
			if (j == '|' && !xrcm) {
				if (*src)
					src++;
				*dst = '\0';
				dst = pdst;
				err = ex_exec(pdst+1);
				if (err && err != xuerr && xerr & 1)
					ex_print("parse command error", msg_ft)
			} else if (*src)
				*dst++ = *src++;
		} else
			*dst++ = *src++;
	}
	*dst++ = '\0';
	sb->s_n = dst - sb->s;
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
	return src;
}

/* execute a single ex command */
void *ex_exec(const char *ln)
{
	int arg, i = 0, idx = 0, r1 = -1, r2 = -1;
	char *ret = NULL;
	if (!xgdep)
		lbuf_mark(xb, '*', xrow, xoff);
	sbuf_smake(sb, strlen(ln) + 4)
	do {
		sbuf_cut(sb, 0)
		ln = ex_arg(ex_cmd(ln, sb, &idx), sb, &arg);
		if (i < r1 || i > r2) {
			ret = excmds[idx].ec(sb->s, excmds[idx].name, sb->s + arg);
			if (ret) {
				if (!*ret) {
					r1 = ((int*)ret)[1] + i;
					r2 = ((int*)ret)[2] + i;
				} else if (ret != xuerr && xerr & 1) {
					ex_print(ret, msg_ft)
					if (xerr & 2)
						break;
				}
			}
		}
		i++;
	} while (*ln);
	free(sb->s);
	return xerr & 4 ? NULL : ret;
}

/* ex main loop */
void ex(void)
{
	xgrec++;
	int esc = 0;
	sbuf_smake(sb, xcols)
	while (!xquit) {
		syn_setft(ex_ft);
		if (ex_read(sb, ":", NULL, 0, 1) == '\n') {
			if (!strcmp(sb->s, ":") && esc) {
				xpln = 2;
				ex_exec(xregs[':']->s);
			} else
				ex_command(sb->s + !(xvis & 2))
			xb->useq += xseq;
			esc = 1;
		} else
			esc = 0;
		sbuf_cut(sb, 0)
	}
	syn_setft(xb_ft);
	free(sb->s);
	xgrec--;
}

void ex_init(char **files, int n)
{
	xbufsalloc = MAX(n, xbufsalloc);
	ec_setbufsmax(NULL, NULL, "");
	char *s = files[0] ? files[0] : "";
	do {
		xmpt = 0;
		ec_edit("", "e", s);
		s = *(++files);
	} while (--n > 0);
	xvis &= ~8;
	if ((s = getenv("EXINIT")))
		ex_command(s)
}
