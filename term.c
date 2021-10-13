struct sbuf *term_sbuf;
int term_record;
static int rows, cols;
static struct termios termios;

void term_init(void)
{
	struct winsize win;
	struct termios newtermios;
	term_sbuf = sbuf_make(2048);
	tcgetattr(0, &termios);
	newtermios = termios;
	newtermios.c_lflag &= ~(ICANON | ISIG);
	newtermios.c_lflag &= ~ECHO;
	tcsetattr(0, TCSAFLUSH, &newtermios);
	if (getenv("LINES"))
		rows = atoi(getenv("LINES"));
	if (getenv("COLUMNS"))
		cols = atoi(getenv("COLUMNS"));
	if (!ioctl(0, TIOCGWINSZ, &win)) {
		cols = win.ws_col;
		rows = win.ws_row;
	}
	cols = cols ? cols : 80;
	rows = rows ? rows : 25;
	term_out("\33[m");
}

void term_done(void)
{
	term_commit();
	sbuf_free(term_sbuf);
	tcsetattr(0, 0, &termios);
}

void term_clean(void)
{
	write(1, CSI_CLEAR_ALL);
	write(1, CSI_CURSOR_TOP_LEFT);
}

void term_suspend(void)
{
	term_done();
	kill(getpid(), SIGSTOP);
	term_init();
}

void term_commit(void)
{
	write(1, sbuf_buf(term_sbuf), sbuf_len(term_sbuf));
	sbuf_cut(term_sbuf, 0);
	term_record = 0;
}

void term_out(char *s)
{
	if (term_record)
		sbuf_str(term_sbuf, s);
	else
		write(1, s, strlen(s));
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
	char cmd[16];
	if (n < 0)
		sprintf(cmd, "\33[%dM", -n);
	if (n > 0)
		sprintf(cmd, "\33[%dL", n);
	if (n)
		term_out(cmd);
}

void term_pos(int r, int c)
{
	char buf[32] = "\r";
	if (c < 0)
		c = 0;
	if (c >= xcols)
		c = cols - 1;
	if (r < 0)
		sprintf(buf, "\r\33[%d%c", abs(c), c > 0 ? 'C' : 'D');
	else
		sprintf(buf, "\33[%d;%dH", r + 1, c + 1);
	term_out(buf);
}

int term_rows(void)
{
	return rows;
}

int term_cols(void)
{
	return cols;
}

static char ibuf[4096];		/* input character buffer */
static char icmd[4096];		/* read after the last term_cmd() */
static int ibuf_pos, ibuf_cnt;	/* ibuf[] position and length */
static int icmd_pos;		/* icmd[] position */

void term_clear()
{
	ibuf_cnt = 0;
}

/* read s before reading from the terminal */
void term_push(char *s, int n)
{
	n = MIN(n, sizeof(ibuf) - ibuf_cnt);
	memcpy(ibuf + ibuf_cnt, s, n);
	ibuf_cnt += n;
}

/* return a static buffer containing inputs read since the last term_cmd() */
char *term_cmd(int *n)
{
	*n = icmd_pos;
	icmd_pos = 0;
	return icmd;
}

int term_read(void)
{
	struct pollfd ufds[1];
	int n, c;
	if (ibuf_pos >= ibuf_cnt) {
		ufds[0].fd = STDIN_FILENO;
		ufds[0].events = POLLIN;
		if (poll(ufds, 1, -1) <= 0)
			return -1;
		/* read a single input character */
		if ((n = read(STDIN_FILENO, ibuf, 1)) <= 0)
			return -1;
		ibuf_cnt = n;
		ibuf_pos = 0;
	}
	c = ibuf_pos < ibuf_cnt ? (unsigned char) ibuf[ibuf_pos++] : -1;
	if (icmd_pos < sizeof(icmd))
		icmd[icmd_pos++] = c;
	return c;
}

/* return a static string that changes text attributes from old to att */
char *term_att(int att, int old)
{
	static char buf[128];
	char *s = buf;
	int fg = SYN_FG(att);
	int bg = SYN_BG(att);
	if (att == old)
		return "";
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
			s = itoa(40 + (fg & 0xff), s);
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
	int pipefds0[2];
	int pipefds1[2];
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
		if (ofd) {		/* setting up stdout */
			close(1);
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

char* xgetenv(char* q[]) {
	char* r = NULL;
	while (*q && !r) {
		if (**q == '$') {
			r = getenv(*q+1);
		}
		else {
			return *q;
		}
		q += 1;
	}
	return r;
}

/* execute a command; process input if iproc and process output if oproc */
char *cmd_pipe(char *cmd, char *ibuf, int iproc, int oproc)
{
	static char* sh[] = {"$SHELL", "sh", NULL};
	char *argv[4+xish]; 
	argv[0] = xgetenv(sh);
	if (xish) 
		argv[xish] = "-i";
	argv[1+xish] = "-c";
	argv[2+xish] = cmd; 
	argv[3+xish] = NULL;

	struct pollfd fds[3];
	struct sbuf *sb = NULL;
	char buf[512];
	int ifd = -1, ofd = -1;
	int slen = iproc ? strlen(ibuf) : 0;
	int nw = 0;
	int pid = cmd_make(argv, iproc ? &ifd : NULL, oproc ? &ofd : NULL);
	if (pid <= 0)
		return NULL;
	if (oproc)
		sb = sbuf_make(64);
	if (!iproc) {
		signal(SIGINT, SIG_IGN);
		term_done();
	}
	fcntl(ifd, F_SETFL, fcntl(ifd, F_GETFL, 0) | O_NONBLOCK);
	fds[0].fd = ofd;
	fds[0].events = POLLIN;
	fds[1].fd = ifd;
	fds[1].events = POLLOUT;
	fds[2].fd = iproc ? 0 : -1;
	fds[2].events = POLLIN;
	while ((fds[0].fd >= 0 || fds[1].fd >= 0) && poll(fds, 3, 200) >= 0) {
		if (fds[0].revents & POLLIN) {
			int ret = read(fds[0].fd, buf, sizeof(buf));
			if (ret > 0)
				sbuf_mem(sb, buf, ret);
			if (ret <= 0)
				close(fds[0].fd);
			continue;
		}
		if (fds[1].revents & POLLOUT) {
			int ret = write(fds[1].fd, ibuf + nw, slen - nw);
			if (ret > 0)
				nw += ret;
			if (ret <= 0 || nw == slen)
				close(fds[1].fd);
			continue;
		}
		if (fds[2].revents & POLLIN) {
			int ret = read(fds[2].fd, buf, sizeof(buf));
			int i;
			for (i = 0; i < ret; i++)
				if ((unsigned char) buf[i] == TK_CTL('c'))
					kill(pid, SIGINT);
		}
		if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
			fds[0].fd = -1;
		if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))
			fds[1].fd = -1;
		if (fds[2].revents & (POLLERR | POLLHUP | POLLNVAL))
			fds[2].fd = -1;
	}
	close(ifd);
	close(ofd);
	waitpid(pid, NULL, 0);
	signal(SIGTTOU, SIG_IGN);
	tcsetpgrp(STDIN_FILENO, getpid());
	signal(SIGTTOU, SIG_DFL);
	if (!iproc) {
		term_init();
		signal(SIGINT, SIG_DFL);
	}
	if (oproc)
		return sbuf_done(sb);
	return NULL;
}

int cmd_exec(char *cmd)
{
	cmd_pipe(cmd, NULL, 0, 0);
	return 0;
}
