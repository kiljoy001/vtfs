/*
 * vtfs - a persistent terminal, exposed as 9P files.
 *
 *	/mnt/vt/clone		read: allocate a session number
 *	/mnt/vt/N/cons		write: input (CR -> NL), read: raw output ring
 *	/mnt/vt/N/input		write: input
 *	/mnt/vt/N/screen	read: current 24x80 text screen, no escapes
 *	/mnt/vt/N/scroll	read: scrollback ring, oldest first
 *	/mnt/vt/N/ctl		write: start, reset, intr, kill, free,
 *				       echo 0|1, resize R C, clear
 *	/mnt/vt/N/status	read: state, pid, cursor, seq, idle
 *
 * The child is /bin/rc -i on pipes; there is no pty on Plan 9, so the
 * terminal is emulated here.  The screen is the state a model should
 * read; 'status' gives seq and idle so a client can wait for quiet.
 */

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

enum {
	Maxsess		= 8,
	Rows		= 24,
	Cols		= 80,
	Scrolllines	= 100,
	Rawbuf		= 256*1024,
};

enum {
	RClone,
	RCons,
	RInput,
	RScreen,
	RScroll,
	RCtl,
	RStatus,
};

enum {
	Free,
	Running,
	Dead,
};

enum {
	Ground,
	Esc,
	Csi,
	Osc,
	OscEsc,
	Charset,
};

typedef struct Session Session;
typedef struct Aux Aux;

struct Session {
	QLock	lk;
	int	used;
	int	state;
	int	pid;
	int	wpid;
	int	in;
	int	out;
	int	gen;
	int	rows, cols;
	int	cx, cy;
	int	wrapnext;
	int	savex, savey;
	int	echo;
	int	quiet;
	int	timeout;
	int	lastcr;

	Rune	grid[Rows][Cols];
	Rune	scroll[Scrolllines][Cols];
	int	nscroll;

	ulong	seq;
	vlong	lastout;

	uchar	raw[Rawbuf];
	uvlong	rawtotal;

	/* parser */
	int	pstate;
	int	params[8];
	int	nparam;
	int	curparam;
	int	csiq;
	uchar	pending[UTFmax];
	int	npending;
};

struct Aux {
	int	role;
	Session	*s;
};

static Session	sess[Maxsess];
static Srv	fs;
static QLock	clonelk;
static int	nextclone;
static char	*token;

static char *
statename(int s)
{
	switch(s){
	case Free:	return "free";
	case Running:	return "running";
	case Dead:	return "dead";
	}
	return "?";
}

static Aux *
mkaux(int role, Session *s)
{
	Aux *a;

	a = emalloc9p(sizeof *a);
	a->role = role;
	a->s = s;
	return a;
}

static void
clearall(Session *s)
{
	int x, y;

	for(y = 0; y < s->rows; y++)
		for(x = 0; x < s->cols; x++)
			s->grid[y][x] = ' ';
	s->cx = s->cy = 0;
	s->wrapnext = 0;
}

static void
scrollup(Session *s)
{
	int x, y;

	for(x = 0; x < s->cols; x++)
		s->scroll[s->nscroll % Scrolllines][x] = s->grid[0][x];
	s->nscroll++;

	for(y = 0; y < s->rows-1; y++)
		memmove(s->grid[y], s->grid[y+1], s->cols*sizeof(Rune));
	for(x = 0; x < s->cols; x++)
		s->grid[s->rows-1][x] = ' ';
}

static void
newline(Session *s)
{
	if(++s->cy >= s->rows){
		scrollup(s);
		s->cy = s->rows-1;
	}
}

static void
putrune(Session *s, Rune r)
{
	if(s->wrapnext){
		s->cx = 0;
		newline(s);
		s->wrapnext = 0;
	}
	if(s->cy < 0)
		s->cy = 0;
	if(s->cy >= s->rows)
		s->cy = s->rows-1;
	if(s->cx < 0)
		s->cx = 0;
	if(s->cx >= s->cols)
		s->cx = s->cols-1;

	s->grid[s->cy][s->cx] = r;
	if(s->cx < s->cols-1)
		s->cx++;
	else
		s->wrapnext = 1;
}

static void
erasecells(Session *s, int y, int from, int to)
{
	int x;

	if(y < 0 || y >= s->rows)
		return;
	if(from < 0)
		from = 0;
	if(to > s->cols)
		to = s->cols;
	for(x = from; x < to; x++)
		s->grid[y][x] = ' ';
}

static int
par(Session *s, int i, int d)
{
	return i < s->nparam ? s->params[i] : d;
}

static void
csi(Session *s, Rune f)
{
	int n, y, x, i, p0;

	p0 = par(s, 0, 0);
	n = p0 ? p0 : 1;

	switch(f){
	case 'A':	s->cy -= n; break;
	case 'B':	s->cy += n; break;
	case 'C':	s->cx += n; break;
	case 'D':	s->cx -= n; break;
	case 'E':	s->cx = 0; s->cy += n; break;
	case 'F':	s->cx = 0; s->cy -= n; break;
	case 'G':	s->cx = (p0 ? p0 : 1) - 1; break;
	case 'd':	s->cy = (p0 ? p0 : 1) - 1; break;
	case 'H':
	case 'f':
		s->cy = (par(s, 0, 1) ? par(s, 0, 1) : 1) - 1;
		s->cx = (par(s, 1, 1) ? par(s, 1, 1) : 1) - 1;
		break;
	case 'J':
		if(p0 == 0 || p0 == 3){
			erasecells(s, s->cy, s->cx, s->cols);
			for(y = s->cy+1; y < s->rows; y++)
				erasecells(s, y, 0, s->cols);
			if(p0 == 3)
				clearall(s);
		}else if(p0 == 1){
			for(y = 0; y < s->cy; y++)
				erasecells(s, y, 0, s->cols);
			erasecells(s, s->cy, 0, s->cx+1);
		}else if(p0 == 2){
			clearall(s);
		}
		break;
	case 'K':
		if(p0 == 0)
			erasecells(s, s->cy, s->cx, s->cols);
		else if(p0 == 1)
			erasecells(s, s->cy, 0, s->cx+1);
		else if(p0 == 2)
			erasecells(s, s->cy, 0, s->cols);
		break;
	case 'L':
		for(i = 0; i < n; i++){
			for(y = s->rows-1; y > s->cy; y--)
				memmove(s->grid[y], s->grid[y-1], s->cols*sizeof(Rune));
			erasecells(s, s->cy, 0, s->cols);
		}
		break;
	case 'M':
		for(i = 0; i < n; i++){
			for(y = s->cy; y < s->rows-1; y++)
				memmove(s->grid[y], s->grid[y+1], s->cols*sizeof(Rune));
			erasecells(s, s->rows-1, 0, s->cols);
		}
		break;
	case 'P':
		if(s->cx < s->cols){
			if(n > s->cols - s->cx)
				n = s->cols - s->cx;
			memmove(&s->grid[s->cy][s->cx], &s->grid[s->cy][s->cx+n],
				(s->cols - s->cx - n)*sizeof(Rune));
			erasecells(s, s->cy, s->cols-n, s->cols);
		}
		break;
	case '@':
		if(s->cx < s->cols){
			if(n > s->cols - s->cx)
				n = s->cols - s->cx;
			memmove(&s->grid[s->cy][s->cx+n], &s->grid[s->cy][s->cx],
				(s->cols - s->cx - n)*sizeof(Rune));
			erasecells(s, s->cy, s->cx, s->cx+n);
		}
		break;
	case 'X':
		erasecells(s, s->cy, s->cx, s->cx+n);
		break;
	case 'S':
		for(i = 0; i < n; i++)
			scrollup(s);
		break;
	case 'T':
		for(i = 0; i < n; i++){
			for(y = s->rows-1; y > 0; y--)
				memmove(s->grid[y], s->grid[y-1], s->cols*sizeof(Rune));
			erasecells(s, 0, 0, s->cols);
		}
		break;
	case 'n':
		if(p0 == 6){
			char buf[32];
			int m;
			m = snprint(buf, sizeof buf, "\033[%d;%dR", s->cy+1, s->cx+1);
			if(s->in >= 0)
				write(s->in, buf, m);
		}
		break;
	case 's':
		s->savex = s->cx;
		s->savey = s->cy;
		break;
	case 'u':
		s->cx = s->savex;
		s->cy = s->savey;
		s->wrapnext = 0;
		break;
	case 'h':
		if(p0 == 1049){
			s->savex = s->cx;
			s->savey = s->cy;
			clearall(s);
		}
		break;
	case 'l':
		if(p0 == 1049){
			s->cx = s->savex;
			s->cy = s->savey;
			s->wrapnext = 0;
		}
		break;
	}

	if(s->cy < 0) s->cy = 0;
	if(s->cy >= s->rows) s->cy = s->rows-1;
	if(s->cx < 0) s->cx = 0;
	if(s->cx >= s->cols) s->cx = s->cols-1;
}

static void
step(Session *s, Rune r)
{
	switch(s->pstate){
	case Ground:
		if(r == 0x1b){
			s->pstate = Esc;
			break;
		}
		switch(r){
		case '\r':
			s->cx = 0;
			s->wrapnext = 0;
			break;
		case '\n':
			s->cx = 0;
			newline(s);
			s->wrapnext = 0;
			break;
		case '\b':
			if(s->cx > 0)
				s->cx--;
			s->wrapnext = 0;
			break;
		case '\t':
			s->cx = (s->cx + 8) & ~7;
			if(s->cx >= s->cols)
				s->cx = s->cols-1;
			s->wrapnext = 0;
			break;
		case '\a':
			break;
		default:
			if(r >= 0x20 && r != 0x7f)
				putrune(s, r);
			break;
		}
		break;

	case Esc:
		s->pstate = Ground;
		switch(r){
		case '[':
			s->pstate = Csi;
			s->nparam = 0;
			s->curparam = 0;
			s->csiq = 0;
			break;
		case ']':
			s->pstate = Osc;
			break;
		case '(':
		case ')':
			s->pstate = Charset;
			break;
		case '7':
			s->savex = s->cx;
			s->savey = s->cy;
			break;
		case '8':
			s->cx = s->savex;
			s->cy = s->savey;
			s->wrapnext = 0;
			break;
		case 'D':
			newline(s);
			break;
		case 'M':
			if(s->cy > 0)
				s->cy--;
			break;
		case 'E':
			s->cx = 0;
			newline(s);
			break;
		case 'c':
			clearall(s);
			break;
		}
		break;

	case Charset:
		s->pstate = Ground;
		break;

	case Osc:
		if(r == '\a')
			s->pstate = Ground;
		else if(r == 0x1b)
			s->pstate = OscEsc;
		break;

	case OscEsc:
		s->pstate = (r == '\\') ? Ground : Osc;
		break;

	case Csi:
		if(r >= '0' && r <= '9'){
			s->curparam = s->curparam*10 + (r - '0');
			break;
		}
		if(r == ';'){
			if(s->nparam < nelem(s->params))
				s->params[s->nparam++] = s->curparam;
			s->curparam = 0;
			break;
		}
		if(r == '?' || r == '>' || r == '<' || r == '='){
			s->csiq = 1;
			break;
		}
		if(r >= 0x40 && r <= 0x7e){
			if(s->nparam < nelem(s->params))
				s->params[s->nparam++] = s->curparam;
			csi(s, r);
			s->pstate = Ground;
		}
		break;
	}
}

static void
rawput(Session *s, uchar *p, int n)
{
	int i;

	for(i = 0; i < n; i++)
		s->raw[s->rawtotal++ % Rawbuf] = p[i];
}

static void
feed(Session *s, uchar *p, int n, int raw)
{
	uchar tmp[640];
	int m, i, w;
	Rune r;

	if(raw)
		rawput(s, p, n);

	m = 0;
	if(s->npending){
		memcpy(tmp, s->pending, s->npending);
		m = s->npending;
		s->npending = 0;
	}
	if(n > (int)sizeof(tmp) - m)
		n = sizeof(tmp) - m;
	memcpy(tmp+m, p, n);
	m += n;

	for(i = 0; i < m; ){
		w = chartorune(&r, (char*)tmp+i);
		if(w == 1 && r == Runeerror && (tmp[i] & 0x80) && m-i < UTFmax){
			memcpy(s->pending, tmp+i, m-i);
			s->npending = m-i;
			break;
		}
		step(s, r);
		i += w;
	}
}

static void
readerproc(void *v)
{
	Session *s;
	char buf[512];
	int n, gen;

	s = v;
	gen = s->gen;
	for(;;){
		n = read(s->out, buf, sizeof buf);
		if(n <= 0)
			break;
		qlock(&s->lk);
		feed(s, (uchar*)buf, n, 1);
		s->seq++;
		s->lastout = nsec();
		qunlock(&s->lk);
	}

	qlock(&s->lk);
	if(s->gen == gen){
		s->state = Dead;
		s->seq++;
		s->lastout = nsec();
	}
	qunlock(&s->lk);
	_exits(nil);
}

static void
shellwrap(Session *s)
{
	int pid, i;
	Waitmsg *w;

	for(;;){
		pid = rfork(RFPROC|RFNAMEG|RFFDG|RFNOTEG);
		if(pid < 0){
			sleep(100);
			continue;
		}
		if(pid == 0){
			for(i = 3; i < 64; i++)
				close(i);
			execl("/bin/rc", "rc", nil);
			_exits("exec");
		}
		qlock(&s->lk);
		s->pid = pid;
		s->state = Running;
		s->seq++;
		s->lastout = nsec();
		qunlock(&s->lk);

		w = wait();
		if(w != nil)
			free(w);

		qlock(&s->lk);
		if(s->pid == pid)
			s->pid = 0;
		s->seq++;
		s->lastout = nsec();
		qunlock(&s->lk);
		sleep(50);
	}
}

static void
start(Session *s)
{
	int pin[2], pout[2], wpid, i;

	if(s->state == Running)
		return;
	if(pipe(pin) < 0 || pipe(pout) < 0){
		werrstr("pipe: %r");
		return;
	}

	s->gen++;
	wpid = rfork(RFPROC|RFMEM|RFNAMEG|RFFDG|RFNOTEG);
	if(wpid < 0){
		close(pin[0]); close(pin[1]);
		close(pout[0]); close(pout[1]);
		werrstr("rfork: %r");
		return;
	}
	if(wpid == 0){
		close(pin[1]);
		close(pout[0]);
		dup(pin[0], 0);
		dup(pout[1], 1);
		dup(pout[1], 2);
		if(pin[0] > 2)
			close(pin[0]);
		if(pout[1] > 2)
			close(pout[1]);
		for(i = 3; i < 64; i++)
			close(i);
		chdir("/");
		putenv("TERM", "vt100");
		putenv("LINES", "24");
		putenv("COLUMNS", "80");
		shellwrap(s);
		_exits(nil);
	}

	close(pin[0]);
	close(pout[1]);
	s->in = pin[1];
	s->out = pout[0];
	s->wpid = wpid;
	s->pid = 0;
	s->state = Running;
	s->seq++;
	s->lastout = nsec();
	s->lastcr = 0;

	switch(rfork(RFPROC|RFMEM|RFNAMEG|RFFDG|RFNOTEG)){
	case 0:
		readerproc(s);
		_exits(nil);
	}
}

static char *
renderscreen(Session *s, int *np)
{
	char *b, *p;
	int x, y, last;

	b = emalloc9p(s->rows*(s->cols*UTFmax+1) + s->rows + 2);
	p = b;
	for(y = 0; y < s->rows; y++){
		last = s->cols-1;
		while(last >= 0 && s->grid[y][last] == ' ')
			last--;
		for(x = 0; x <= last; x++)
			p += runetochar(p, &s->grid[y][x]);
		*p++ = '\n';
	}
	*p = 0;
	*np = p - b;
	return b;
}

static char *
renderscroll(Session *s, int *np)
{
	char *b, *p;
	int x, k, last, start, total;

	total = s->nscroll;
	start = total - Scrolllines;
	if(start < 0)
		start = 0;

	b = emalloc9p((total-start)*(s->cols*UTFmax+1) + 2);
	p = b;
	for(k = start; k < total; k++){
		Rune *line = s->scroll[k % Scrolllines];
		last = s->cols-1;
		while(last >= 0 && line[last] == ' ')
			last--;
		for(x = 0; x <= last; x++)
			p += runetochar(p, &line[x]);
		*p++ = '\n';
	}
	*p = 0;
	*np = p - b;
	return b;
}

static int
rawread(Session *s, vlong off, char *dst, long cnt)
{
	uvlong start, end, avail;
	int i;

	if(off < 0)
		off = 0;
	avail = s->rawtotal;
	start = off;
	if(start > avail)
		start = avail;
	if(start + Rawbuf < avail)
		start = avail - Rawbuf;
	end = start + cnt;
	if(end > avail)
		end = avail;
	for(i = 0; i < end-start; i++)
		dst[i] = s->raw[(start+i) % Rawbuf];
	return end-start;
}

static void
reply(Req *r, char *b, int n)
{
	vlong off;
	long cnt;

	off = r->ifcall.offset;
	cnt = r->ifcall.count;
	if(off < 0)
		off = 0;
	if(off > n)
		off = n;
	if(cnt > n-off)
		cnt = n-off;
	memmove(r->ofcall.data, b+off, cnt);
	r->ofcall.count = cnt;
	respond(r, nil);
}

static void
fsread(Req *r)
{
	Aux *a;
	Session *s;
	char *b, statbuf[256];
	int n, i, id;
	vlong idle;

	a = r->fid->file->aux;
	if(a == nil){
		respond(r, "bad file");
		return;
	}
	if(a->role == RClone){
		if(r->ifcall.offset > 0){
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		qlock(&clonelk);
		id = -1;
		for(i = 0; i < Maxsess; i++){
			int k = (nextclone+i) % Maxsess;
			if(!sess[k].used && sess[k].state != Dead){
				sess[k].used = 1;
				nextclone = (k+1) % Maxsess;
				id = k;
				break;
			}
		}
		qunlock(&clonelk);
		if(id < 0){
			respond(r, "no free sessions");
			return;
		}
		n = snprint(statbuf, sizeof statbuf, "%d\n", id+1);
		reply(r, statbuf, n);
		return;
	}

	s = a->s;
	switch(a->role){
	case RScreen:
		qlock(&s->lk);
		b = renderscreen(s, &n);
		qunlock(&s->lk);
		reply(r, b, n);
		free(b);
		return;

	case RScroll:
		qlock(&s->lk);
		b = renderscroll(s, &n);
		qunlock(&s->lk);
		reply(r, b, n);
		free(b);
		return;

	case RStatus:
		qlock(&s->lk);
		idle = s->lastout ? (nsec() - s->lastout)/1000000 : -1;
		n = snprint(statbuf, sizeof statbuf,
			"state %s\npid %d\nrows %d\ncols %d\n"
			"cursor %d %d\nseq %lud\nidle %lld\nout %llud\n"
			"quiet %d\ntimeout %d\n",
			statename(s->state), s->pid, s->rows, s->cols,
			s->cy, s->cx, s->seq, idle, s->rawtotal,
			s->quiet, s->timeout);
		qunlock(&s->lk);
		reply(r, statbuf, n);
		return;

	case RCons:
		qlock(&s->lk);
		n = rawread(s, r->ifcall.offset, r->ofcall.data, r->ifcall.count);
		qunlock(&s->lk);
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}

	respond(r, "not readable");
}

static int interruptchildren(int);

static void
sendinput(Session *s, char *d, long n)
{
	char buf[512];
	long i;
	int m;

	m = 0;
	for(i = 0; i < n; i++){
		if(d[i] == 3){
			interruptchildren(s->pid);
			continue;
		}
		if(d[i] == '\r'){
			if(s->lastcr)
				continue;
			s->lastcr = 1;
			buf[m++] = '\n';
		}else if(d[i] == '\n'){
			if(s->lastcr){
				s->lastcr = 0;
				continue;
			}
			buf[m++] = '\n';
		}else{
			s->lastcr = 0;
			buf[m++] = d[i];
		}
		if(m == sizeof buf){
			if(s->in >= 0)
				write(s->in, buf, m);
			if(s->echo)
				feed(s, (uchar*)buf, m, 0);
			m = 0;
		}
	}
	if(m){
		if(s->in >= 0)
			write(s->in, buf, m);
		if(s->echo)
			feed(s, (uchar*)buf, m, 0);
	}
}

static int
interruptchildren(int ppid)
{
	Dir *d;
	int fd, n, i, f, m, child, count;
	char path[64], buf[64];

	count = 0;
	if(ppid <= 0)
		return 0;
	fd = open("/proc", OREAD);
	if(fd < 0)
		return 0;
	while((n = dirread(fd, &d)) > 0){
		for(i = 0; i < n; i++){
			child = atoi(d[i].name);
			if(child <= 0)
				continue;
			snprint(path, sizeof path, "/proc/%d/ppid", child);
			f = open(path, OREAD);
			if(f < 0)
				continue;
			m = read(f, buf, sizeof buf - 1);
			close(f);
			if(m <= 0)
				continue;
			buf[m] = 0;
			if(atoi(buf) == ppid){
				postnote(PNPROC, child, "interrupt");
				count++;
			}
		}
		free(d);
	}
	close(fd);
	return count;
}

static void
ctlwrite(Session *s, char *d, long n)
{
	char buf[512], *line, *e, *p;
	int m;

	m = n < (long)sizeof(buf)-1 ? n : (int)sizeof(buf)-1;
	memcpy(buf, d, m);
	buf[m] = 0;

	for(line = buf; line != nil && *line != 0; line = e){
		e = strchr(line, '\n');
		if(e != nil)
			*e++ = 0;
		p = line + strlen(line);
		while(p > line && (p[-1] == '\r' || p[-1] == ' '))
			*--p = 0;

		qlock(&s->lk);
		if(strcmp(line, "start") == 0){
			/* the wrapper starts rc automatically */
		}else if(strcmp(line, "reset") == 0 || strcmp(line, "restart") == 0){
			if(s->pid > 0)
				postnote(PNGROUP, s->pid, "kill");
			clearall(s);
		}else if(strcmp(line, "intr") == 0){
			interruptchildren(s->pid);
		}else if(strcmp(line, "kill") == 0){
			if(s->pid > 0)
				postnote(PNGROUP, s->pid, "kill");
			if(s->wpid > 0)
				postnote(PNPROC, s->wpid, "kill");
			s->pid = 0;
			s->wpid = 0;
			s->state = Dead;
			clearall(s);
		}else if(strcmp(line, "free") == 0){
			if(s->pid > 0)
				postnote(PNGROUP, s->pid, "kill");
			s->used = 0;
			clearall(s);
		}else if(strcmp(line, "clear") == 0){
			clearall(s);
		}else if(strncmp(line, "echo ", 5) == 0){
			s->echo = atoi(line+5) != 0;
		}else if(strncmp(line, "quiet ", 6) == 0){
			s->quiet = atoi(line+6);
		}else if(strncmp(line, "timeout ", 8) == 0){
			s->timeout = atoi(line+8);
		}else if(strncmp(line, "resize ", 7) == 0){
			int rows, cols;
			rows = atoi(line+7);
			cols = 1;
			p = strchr(line+7, ' ');
			if(p != nil)
				cols = atoi(p+1);
			if(rows > 0 && rows <= Rows)
				s->rows = rows;
			if(cols > 0 && cols <= Cols)
				s->cols = cols;
			clearall(s);
		}
		qunlock(&s->lk);
	}
}

static void
fswrite(Req *r)
{
	Aux *a;
	Session *s;

	a = r->fid->file->aux;
	if(a == nil){
		respond(r, "bad file");
		return;
	}
	s = a->s;

	switch(a->role){
	case RCtl:
		ctlwrite(s, r->ifcall.data, r->ifcall.count);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;

	case RCons:
	case RInput:
		qlock(&s->lk);
		if(s->state != Running){
			qunlock(&s->lk);
			respond(r, "session not running");
			return;
		}
		sendinput(s, r->ifcall.data, r->ifcall.count);
		s->seq++;
		s->lastout = nsec();
		qunlock(&s->lk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}

	respond(r, "not writable");
}

static void
fsattach(Req *r)
{
	if(token != nil && token[0] != 0
	&& (r->ifcall.aname == nil || strcmp(r->ifcall.aname, token) != 0)){
		respond(r, "authentication required");
		return;
	}
	respond(r, nil);
}

static void
fsopen(Req *r)
{
	Aux *a;

	respond(r, nil);
}

static void
mkent(File *d, char *name, ulong perm, int role, Session *s)
{
	File *f;

	f = createfile(d, name, "vt", perm, nil);
	if(f == nil)
		sysfatal("createfile %s: %r", name);
	f->aux = mkaux(role, s);
}

static void
setup(void)
{
	File *root, *d;
	char name[16];
	int i;

	fs.tree = alloctree("vt", "vt", DMDIR|0777, nil);
	root = fs.tree->root;
	mkent(root, "clone", 0666, RClone, nil);

	for(i = 0; i < Maxsess; i++){
		Session *s = &sess[i];
		s->in = s->out = -1;
		s->rows = Rows;
		s->cols = Cols;
		s->echo = 1;
		s->quiet = 120;
		s->timeout = 2000;
		clearall(s);

		snprint(name, sizeof name, "%d", i+1);
		d = createfile(root, name, "vt", DMDIR|0777, nil);
		if(d == nil)
			sysfatal("createfile %s: %r", name);
		mkent(d, "cons", 0666, RCons, s);
		mkent(d, "input", 0222, RInput, s);
		mkent(d, "screen", 0444, RScreen, s);
		mkent(d, "scroll", 0444, RScroll, s);
		mkent(d, "ctl", 0222, RCtl, s);
		mkent(d, "status", 0444, RStatus, s);
	}

	/* start before the server forks; request handlers must inherit the fds */
	for(i = 0; i < Maxsess; i++)
		start(&sess[i]);
}

static char *
readtoken(char *path)
{
	int fd, n;
	char buf[256];

	fd = open(path, OREAD);
	if(fd < 0)
		sysfatal("token %s: %r", path);
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		sysfatal("empty token %s", path);
	buf[n] = 0;
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
		buf[--n] = 0;
	return estrdup9p(buf);
}

static void
usage(void)
{
	fprint(2, "usage: %s [-D] [-M] [-a addr] [-s srvname] [-m mtpt] [-T tokenfile]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *srvname, *mtpt, *addr, *tokenfile;

	srvname = "vt";
	mtpt = "/mnt/vt";
	addr = nil;
	tokenfile = nil;

	ARGBEGIN{
	case 'D':
		chatty9p++;
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 'M':
		mtpt = nil;
		break;
	case 'a':
		addr = EARGF(usage());
		break;
	case 'T':
		tokenfile = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;

	if(argc)
		usage();

	if(tokenfile != nil)
		token = readtoken(tokenfile);

	setup();
	fs.attach = fsattach;
	fs.open = fsopen;
	fs.read = fsread;
	fs.write = fswrite;

	if(mtpt != nil && access(mtpt, AEXIST) < 0){
		int fd = create(mtpt, OREAD, DMDIR|0777);
		if(fd >= 0)
			close(fd);
	}

	if(addr != nil)
		listensrv(&fs, addr);
	postmountsrv(&fs, srvname, mtpt, MREPL|MCREATE);
	exits(0);
}
