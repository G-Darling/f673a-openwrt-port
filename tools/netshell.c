/*
 * netshell.c - minimal static TCP shell server for the ZTE F673A bring-up.
 *
 * Binds 0.0.0.0:PORT, forks a child per connection, dup2()s the socket to
 * fd 0/1/2 and execve()s /bin/sh. Built with:
 *   arm-linux-gnueabi-gcc -march=armv7-a -marm -mfloat-abi=soft -O2 \
 *     -static -nostdlib -ffreestanding -o netshell netshell.c
 * No libc, no dynamic loader, no entropy, nothing external. Works even if
 * busybox nc lacks listen mode (OpenWrt nc is client-only) and dropbear
 * fails to start.
 *
 * ARM EABI syscall numbers (linux/arm):
 *   restart_syscall 0, exit 1, fork 2, read 3, write 4, open 5, close 6,
 *   wait4 114, dup2 63 (old dup2), socket 281, bind 282, listen 283,
 *   accept 284, execve 11, exit_group 248
 */
typedef unsigned int u32;

static inline long sys(long n, long a, long b, long c, long d, long e, long f)
{
	register long r0 asm("r0") = a;
	register long r1 asm("r1") = b;
	register long r2 asm("r2") = c;
	register long r3 asm("r3") = d;
	register long r4 asm("r4") = e;
	register long r5 asm("r5") = f;
	register long r7 asm("r7") = n;
	asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4),
		     "r"(r5), "r"(r7) : "memory", "cc");
	return r0;
}

#define sys0(n) sys(n, 0, 0, 0, 0, 0, 0)

static void puts_(const char *s)
{
	long n = 0;
	while (s[n])
		n++;
	sys(4, 1, (long)s, n, 0, 0, 0); /* write(1, s, n) */
}

static void putnum(u32 v)
{
	char b[11];
	int i = 10;
	b[i] = '\0';
	if (!v)
		b[--i] = '0';
	while (v) {
		b[--i] = '0' + (v % 10);
		v /= 10;
	}
	puts_(b);
}

/* sockaddr_in: family(2) port(2 BE) addr(4) pad(8) = 16 bytes */
struct sa {
	u32 family;
	u32 port; /* big-endian in low 16 bits */
	u32 addr;
	u32 pad[2];
};

int main(void); /* forward */

/* entry point (linked with -Wl,-e,_start, no crt0) */
void _start(void)
{
	main();
	sys0(248); /* exit_group */
}

int main(void)
{
	int fd, cfd;
	struct sa sa;
	int one = 1;
	u32 pid;
	char *argv[2];
	char *envp[1];

	fd = (int)sys(281, 2, 1, 0, 0, 0, 0); /* socket(AF_INET=2, SOCK_STREAM=1, 0) */
	if (fd < 0) {
		puts_("netshell: socket fail ");
		putnum((u32)-fd);
		puts_("\n");
		sys0(1);
		return 0;
	}
	/* SO_REUSEADDR: setsockopt(fd, SOL_SOCKET=1, 2, &one, 4), nr 293 */
	sys(293, (long)fd, 1, 2, (long)&one, 4, 0);

	sa.family = 2;
	sa.port = (u32)(23 << 16); /* port 23, network byte order */
	sa.addr = 0;               /* INADDR_ANY */
	sa.pad[0] = sa.pad[1] = 0;
	if (sys(282, (long)fd, (long)&sa, 16, 0, 0, 0) < 0) {
		puts_("netshell: bind 23 fail\n");
		sys0(1);
		return 0;
	}
	if (sys(283, (long)fd, 8, 0, 0, 0, 0) < 0) { /* listen(fd, 8) */
		puts_("netshell: listen fail\n");
		sys0(1);
		return 0;
	}
	puts_("netshell: listening on 23\n");

	for (;;) {
		cfd = (int)sys(284, (long)fd, 0, 0, 0, 0, 0); /* accept */
		if (cfd < 0)
			continue;
		pid = (u32)sys0(2); /* fork */
		if (pid == 0) {
			/* child */
			sys(63, (long)cfd, 0, 0, 0, 0, 0); /* dup2(cfd, 0) */
			sys(63, (long)cfd, 1, 0, 0, 0, 0); /* dup2(cfd, 1) */
			sys(63, (long)cfd, 2, 0, 0, 0, 0); /* dup2(cfd, 2) */
			if (cfd > 2)
				sys(6, (long)cfd, 0, 0, 0, 0, 0); /* close(cfd) */
			sys(6, (long)fd, 0, 0, 0, 0, 0);	  /* close(fd) */
			argv[0] = "/bin/sh";
			argv[1] = 0;
			envp[0] = 0;
			sys(11, (long)"/bin/sh", (long)argv, (long)envp, 0, 0, 0);
			sys0(1); /* execve failed */
			return 0;
		}
		/* parent: reap and keep listening */
		sys(6, (long)cfd, 0, 0, 0, 0, 0); /* close(cfd) in parent */
	}
}
