sbuf *term_sbuf;
int term_record;
int xrows, xcols;
static struct termios termios;

void term_init(void)
{
	if (xvis & 2)
		return;
	struct winsize win;
	struct termios newtermios;
	sbufn_make(term_sbuf, 2048)
	tcgetattr(0, &termios);
	newtermios = termios;
	newtermios.c_lflag &= ~(ICANON | ISIG | ECHO);
	tcsetattr(0, TCSAFLUSH, &newtermios);
	if (getenv("LINES"))
		xrows = atoi(getenv("LINES"));
	if (getenv("COLUMNS"))
		xcols = atoi(getenv("COLUMNS"));
	if (!ioctl(0, TIOCGWINSZ, &win)) {
		xcols = win.ws_col;
		xrows = win.ws_row;
	}
	xcols = xcols ? xcols : 80;
	xrows = xrows ? xrows : 25;
}

void term_done(void)
{
	if (xvis & 2)
		return;
	term_commit();
	sbuf_free(term_sbuf)
	tcsetattr(0, 0, &termios);
}

void term_clean(void)
{
	term_write("\x1b[2J", 4)	/* clear screen */
	term_write("\x1b[H", 3)		/* cursor topleft */
}

void term_suspend(void)
{
	term_done();
	kill(0, SIGSTOP);
	term_init();
}

void term_commit(void)
{
	term_write(term_sbuf->s, term_sbuf->s_n)
	sbuf_cut(term_sbuf, 0)
	term_record = 0;
}

static void term_out(char *s)
{
	if (term_record)
		sbufn_str(term_sbuf, s)
	else
		term_write(s, strlen(s))
}

void term_chr(int ch)
{
	char s[4] = {ch};
	term_out(s);
}

void term_kill(void)
{
	term_out("\33[K");
}

void term_room(int n)
{
	char cmd[64] = "\33[";
	if (!n)
		return;
	char *s = itoa(abs(n), cmd+2);
	s[0] = n < 0 ? 'M' : 'L';
	s[1] = '\0';
	term_out(cmd);
}

void term_pos(int r, int c)
{
	char buf[64] = "\r\33[", *s;
	if (r < 0) {
		memcpy(itoa(MAX(0, c), buf+3), c > 0 ? "C" : "D", 2);
		term_out(buf);
	} else {
		s = itoa(r + 1, buf+3);
		if (c > 0) {
			*s++ = ';';
			s = itoa(c + 1, s);
		}
		memcpy(s, "H", 2);
		term_out(buf+1);
	}
}

static unsigned char ibuf[4096];	/* input character buffer */
unsigned int ibuf_pos, ibuf_cnt;	/* ibuf[] position and length */
unsigned char icmd[4096];		/* read after the last term_cmd() */
unsigned int icmd_pos;			/* icmd[] position */
unsigned int tibuf_pos, texec, tn;

/* read s before reading from the terminal */
void term_push(char *s, unsigned int n)
{
	n = MIN(n, sizeof(ibuf) - ibuf_cnt);
	if (texec) {
		if (texec == '@' && n && xquit > 0)
			xquit = 0;
		if (tibuf_pos != ibuf_pos)
			tn = 0;
		memmove(ibuf + ibuf_pos + n + tn,
			ibuf + ibuf_pos + tn, ibuf_cnt - ibuf_pos - tn);
		memcpy(ibuf + ibuf_pos + tn, s, n);
		tn += n;
		tibuf_pos = ibuf_pos;
	} else
		memcpy(ibuf + ibuf_cnt, s, n);
	ibuf_cnt += n;
}

void term_back(int c)
{
	char s[1] = {c};
	term_push(s, 1);
}

int term_read(void)
{
	struct pollfd ufds[1];
	if (ibuf_pos >= ibuf_cnt) {
		if (texec) {
			xquit = !xquit ? 1 : xquit;
			if (texec == '&')
				goto err;
		}
		ufds[0].fd = STDIN_FILENO;
		ufds[0].events = POLLIN;
		/* read a single input character */
		if (xquit < 0 || poll(ufds, 1, -1) <= 0 ||
				read(STDIN_FILENO, ibuf, 1) <= 0) {
			xquit = !isatty(STDIN_FILENO) ? -1 : xquit;
			err:
			*ibuf = 0;
		} else if (texec && ibuf_pos < sizeof(ibuf)) {
			ibuf_cnt++;
			ibuf[ibuf_pos] = *ibuf;
			goto ret;
		}
		ibuf_cnt = 1;
		ibuf_pos = 0;
	}
	ret:
	icmd_pos = icmd_pos % sizeof(icmd);
	icmd[icmd_pos++] = ibuf[ibuf_pos];
	return ibuf[ibuf_pos++];
}

/* return a static string that changes text attributes to att */
char *term_att(int att)
{
	static char buf[128];
	char *s = buf;
	int fg = SYN_FG(att);
	int bg = SYN_BG(att);
	*s++ = '\x1b';
	*s++ = '[';
	if (att & SYN_BD)
		{*s++ = ';'; *s++ = '1';}
	if (att & SYN_IT)
		{*s++ = ';'; *s++ = '3';}
	else if (att & SYN_RV)
		{*s++ = ';'; *s++ = '7';}
	if (SYN_FGSET(att)) {
		*s++ = ';';
		if ((fg & 0xff) < 8)
			s = itoa(30 + (fg & 0xff), s);
		else
			s = itoa(fg & 0xff, (char*)memcpy(s, "38;5;", 5)+5);
	}
	if (SYN_BGSET(att)) {
		*s++ = ';';
		if ((bg & 0xff) < 8)
			s = itoa(40 + (bg & 0xff), s);
		else
			s = itoa(bg & 0xff, (char*)memcpy(s, "48;5;", 5)+5);
	}
	s[0] = 'm';
	s[1] = '\0';
	return buf;
}

static int cmd_make(char **argv, int *ifd, int *ofd)
{
	int pid;
	int pipefds0[2] = {-1, -1};
	int pipefds1[2] = {-1, -1};
	if (ifd)
		pipe(pipefds0);
	if (ofd)
		pipe(pipefds1);
	if (!(pid = fork())) {
		if (ifd) {		/* setting up stdin */
			close(0);
			dup(pipefds0[0]);
			close(pipefds0[1]);
			close(pipefds0[0]);
		}
		if (ofd) {		/* setting up stdout and stderr */
			close(1);
			dup(pipefds1[1]);
			close(2);
			dup(pipefds1[1]);
			close(pipefds1[0]);
			close(pipefds1[1]);
		}
		execvp(argv[0], argv);
		exit(1);
	}
	if (ifd)
		close(pipefds0[0]);
	if (ofd)
		close(pipefds1[1]);
	if (pid < 0) {
		if (ifd)
			close(pipefds0[1]);
		if (ofd)
			close(pipefds1[0]);
		return -1;
	}
	if (ifd)
		*ifd = pipefds0[1];
	if (ofd)
		*ofd = pipefds1[0];
	return pid;
}

char *xgetenv(char **q)
{
	char *r = NULL;
	while (*q && !r) {
		if (**q == '$')
			r = getenv(*q+1);
		else
			return *q;
		q++;
	}
	return r;
}

/* execute a command; pass in input if ibuf and process output if oproc */
char *cmd_pipe(char *cmd, char *ibuf, int oproc)
{
	static char *sh[] = {"$SHELL", "sh", NULL};
	struct pollfd fds[3];
	char buf[512];
	int ifd = -1, ofd = -1;
	int slen = ibuf ? strlen(ibuf) : 0;
	int nw = 0;
	char *argv[5];
	argv[0] = xgetenv(sh);
	argv[1] = xish ? "-i" : argv[0];
	argv[2] = "-c";
	argv[3] = cmd;
	argv[4] = NULL;
	int pid = cmd_make(argv+!xish, ibuf ? &ifd : NULL, oproc ? &ofd : NULL);
	if (pid <= 0)
		return NULL;
	sbuf_smake(sb, sizeof(buf))
	if (!ibuf) {
		signal(SIGINT, SIG_IGN);
		term_done();
	} else if (ifd >= 0)
		fcntl(ifd, F_SETFL, fcntl(ifd, F_GETFL, 0) | O_NONBLOCK);
	fds[0].fd = ofd;
	fds[0].events = POLLIN;
	fds[1].fd = ifd;
	fds[1].events = POLLOUT;
	fds[2].fd = ibuf ? 0 : -1;
	fds[2].events = POLLIN;
	while ((fds[0].fd >= 0 || fds[1].fd >= 0) && poll(fds, 3, 200) >= 0) {
		if (fds[0].revents & POLLIN) {
			int ret = read(fds[0].fd, buf, sizeof(buf));
			if (ret > 0 && oproc == 2)
				term_write(buf, ret)
			if (ret > 0)
				sbuf_mem(sb, buf, ret)
			else {
				close(fds[0].fd);
				fds[0].fd = -1;
			}
		} else if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			close(fds[0].fd);
			fds[0].fd = -1;
		}
		if (fds[1].revents & POLLOUT) {
			int ret = write(fds[1].fd, ibuf + nw, slen - nw);
			if (ret > 0)
				nw += ret;
			if (ret <= 0 || nw == slen) {
				close(fds[1].fd);
				fds[1].fd = -1;
			}
		} else if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			close(fds[1].fd);
			fds[1].fd = -1;
		}
		if (fds[2].revents & POLLIN) {
			int ret = read(fds[2].fd, buf, sizeof(buf));
			for (int i = 0; i < ret; i++)
				if ((unsigned char) buf[i] == TK_CTL('c'))
					kill(pid, SIGINT);
		} else if (fds[2].revents & (POLLERR | POLLHUP | POLLNVAL))
			fds[2].fd = -1;
	}
	if (fds[0].fd >= 0)
		close(ofd);
	if (fds[1].fd >= 0)
		close(ifd);
	waitpid(pid, NULL, 0);
	signal(SIGTTOU, SIG_IGN);
	tcsetpgrp(STDIN_FILENO, getpgrp());
	signal(SIGTTOU, SIG_DFL);
	if (!ibuf) {
		term_init();
		signal(SIGINT, SIG_DFL);
	}
	if (oproc)
		sbufn_sret(sb)
	free(sb->s);
	return NULL;
}
