/* initbin.c — freestanding ARM PID1 for the ZTE F673AV9a (vendor 4.19 kernel).
 *
 * WHY THIS EXISTS (root causes proven 2026-11-27 via RAM __log_buf mining):
 *   1. vendor kernel arms a hardware watchdog in driver init:
 *      pdt_wdt_init: "Starting Watchdog Timer... heartbeat 16 sec".
 *   2. shell-script init died early with exitcode=0x00007f00 (127) because
 *      /dev has no devtmpfs yet (kernel does not auto-mount it), so all
 *      diagnostic writes silently failed.
 *   3. The kernel console [ttyAMA0] never emits to the serial wire.
 *
 * THIS BINARY (static, no libc; proven lineage = uart-insmod-diag v10 that
 * ran on this board as sf26's /sbin/init):
 *   1. takes over the PL011 -> LIVE serial output on COM7
 *   2. dumps kernel printk ring to UART (klogctl READ_ALL)
 *   3. mounts devtmpfs/proc/sysfs (/dev/kmsg + /dev/watchdog appear)
 *   4. forks child: execve /bin/sh /init (bring-up script; its
 *      stderr->/dev/kmsg + set -x trace NOW WORKS)
 *   5. PID1 = this binary, NEVER exits:
 *        - feeds watchdog every 3s (/dev/watchdog write; fallback
 *          /proc/watchdog/ctrl "feeddog")
 *        - every 6s: klogctl READ_CLEAR -> new kmsg lines mirrored to UART
 *        - child death: wait4(WNOHANG) reap + auto-respawn /init
 *
 * NO NAND WRITES anywhere.
 *
 * Build:
 *   arm-linux-gnueabi-gcc -march=armv7-a -marm -mfloat-abi=soft -O2 \
 *     -static -nostdlib -ffreestanding -Wl,-e,_start -o initbin initbin.c
 */
typedef unsigned int u32;
typedef unsigned long ulong;

#define UART_BASE 0x94404000UL
#define U_DR    (*(volatile u32 *)(UART_BASE + 0x00))
#define U_FR    (*(volatile u32 *)(UART_BASE + 0x18))
#define U_IBRD  (*(volatile u32 *)(UART_BASE + 0x24))
#define U_FBRD  (*(volatile u32 *)(UART_BASE + 0x28))
#define U_LCRH  (*(volatile u32 *)(UART_BASE + 0x2C))
#define U_CR    (*(volatile u32 *)(UART_BASE + 0x30))
#define U_ICR   (*(volatile u32 *)(UART_BASE + 0x44))

/* ---- raw syscalls (ARM EABI, r7 = number) ---- */
static inline long sc2(long n, long a, long b) {
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r7 asm("r7") = n;
    asm volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r7) : "memory");
    return r0;
}
static inline long sc3(long n, long a, long b, long c) {
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r7 asm("r7") = n;
    asm volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}
static inline long sc4(long n, long a, long b, long c, long d) {
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r3 asm("r3") = d;
    register long r7 asm("r7") = n;
    asm volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r7) : "memory");
    return r0;
}
static inline long sc5(long n, long a, long b, long c, long d, long e) {
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r3 asm("r3") = d;
    register long r4 asm("r4") = e;
    register long r7 asm("r7") = n;
    asm volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r7) : "memory");
    return r0;
}
static inline long sc1(long n, long a) {
    register long r0 asm("r0") = a;
    register long r7 asm("r7") = n;
    asm volatile("svc #0" : "+r"(r0) : "r"(r7) : "memory");
    return r0;
}

#define SYS_EXIT_GROUP 248
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_FORK       2
#define SYS_EXECVE     11
#define SYS_MOUNT      21
#define SYS_KLOGCTL    103
#define SYS_NANOSLEEP  162
#define SYS_WAIT4      258

static long sys_write(int fd, const char *buf, long n) { return sc3(4, fd, (long)buf, n); }
static long sys_open(const char *p, long fl) { return sc3(SYS_OPEN, (long)p, fl, 0); }
static long sys_klogctl(int type, char *buf, int len) { return sc3(SYS_KLOGCTL, type, (long)buf, len); }
static long sys_mount(const char *src, const char *tgt, const char *fs, ulong fl, const void *d) {
    return sc5(SYS_MOUNT, (long)src, (long)tgt, (long)fs, (long)fl, (long)d);
}
/* klog: inject a line into the kernel printk ring via /dev/kmsg (minable!) */
static void klog(const char *s) {
    char buf[256];
    long fd = sys_open("/dev/kmsg", 1);
    int i = 0;
    const char *p = s;
    while (*p && i < 200) buf[i++] = *p++;
    buf[i++] = '\n';
    if (fd >= 0) { sys_write(fd, buf, i); sc1(SYS_CLOSE, fd); }
}
static long sys_nanosleep(long sec, long nsec) {
    long ts[2]; ts[0] = sec; ts[1] = nsec;
    return sc2(SYS_NANOSLEEP, (long)ts, 0);
}

/* ---- PL011 ---- */
static void putc_(char c) {
    int spin = 0;
    while ((U_FR & (1u << 5)) && spin < 2000000) spin++;
    U_DR = (u32)(unsigned char)c;
}
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void putdec(long v) {
    char b[24]; int i = 0; ulong u;
    if (v < 0) { putc_('-'); u = (ulong)(-v); } else u = (ulong)v;
    if (!u) { putc_('0'); return; }
    while (u) { b[i++] = (char)('0' + (u % 10)); u /= 10; }
    while (i--) putc_(b[i]);
}
static void puthex(ulong v) {
    const char *hx = "0123456789abcdef";
    int k;
    for (k = 28; k >= 0; k -= 4) putc_(hx[(v >> k) & 0xf]);
}
static void report(const char *label, long v) { puts_(label); putdec(v); puts_("\r\n"); }
/* append decimal of v to the last klog line: open+write appends? /dev/kmsg
   writes are line-based, so use one-shot helper instead */
static char kline[192];
static int klen = 0;
static void klog_add(const char *s) {
    while (*s && klen < 180) kline[klen++] = *s++;
}
static void klog_num(long v) {
    char b[16]; int i = 0; ulong u;
    if (v < 0) { klog_add("-"); u = (ulong)(-v); } else u = (ulong)v;
    if (!u) kline[klen++] = '0';
    while (u) { b[i++] = (char)('0' + (u % 10)); u /= 10; }
    while (i--) kline[klen++] = b[i];
}
static void klog_flush(void) {
    kline[klen] = 0;
    klog(kline);
    klen = 0;
}

static void uart_init(void) {
    u32 ibrd = U_IBRD, fbrd = U_FBRD;
    if (ibrd == 0 || ibrd > 0xffff) { ibrd = 13; fbrd = 1; }
    U_CR = 0;
    U_ICR = 0x7ff;
    U_IBRD = ibrd;
    U_FBRD = fbrd;
    U_LCRH = 0x70;
    U_CR = 0x301;
}
static void uart_probe(void) {
    static const u32 tbl[4][2] = {{13, 1}, {54, 16}, {27, 8}, {6, 49}};
    int k;
    for (k = 0; k < 4; k++) {
        U_CR = 0;
        U_ICR = 0x7ff;
        U_IBRD = tbl[k][0];
        U_FBRD = tbl[k][1];
        U_LCRH = 0x70;
        U_CR = 0x301;
        puts_("\r\n[P"); putdec(k); puts_("] UART ALIVE\r\n");
    }
    U_IBRD = 13;
    U_FBRD = 1;
}

/* ---- kmsg to UART ---- */
static void dump_kmsg_new(void) {
    static char kb[16384];
    long n = sys_klogctl(4, kb, (int)sizeof(kb) - 1);   /* READ_CLEAR */
    long i;
    if (n <= 0) return;
    if (n > (long)sizeof(kb) - 1) n = sizeof(kb) - 1;
    for (i = 0; i < n; i++) {
        char c = kb[i];
        if (c == '\n') putc_('\r');
        putc_(c);
    }
}
static void dump_kmsg_all(const char *title) {
    static char kb[32768];
    long n, i;
    puts_("\r\n===="); puts_(title); puts_("====\r\n");
    n = sys_klogctl(3, kb, (int)sizeof(kb) - 1);        /* READ_ALL */
    if (n <= 0) { report("[kmsg] rc=", n); return; }
    if (n > (long)sizeof(kb) - 1) n = sizeof(kb) - 1;
    for (i = 0; i < n; i++) {
        char c = kb[i];
        if (c == '\n') putc_('\r');
        putc_(c);
    }
    puts_("====end kmsg====\r\n");
}

/* ---- watchdog: never give up ---- */
static int wdt_fd = -1;
static int wdt_ok_path = 0;
static int feed_watchdog(void) {
    char c = 'F';
    char msg[8] = "feeddog";
    if (wdt_fd >= 0) {
        if (sys_write(wdt_fd, &c, 1) == 1) return 0;
        sc1(SYS_CLOSE, wdt_fd);
        wdt_fd = -1;
    }
    if (wdt_fd < 0) {
        wdt_fd = (int)sys_open("/dev/watchdog", 1);
        if (wdt_fd < 0) wdt_fd = (int)sys_open("/dev/zx_wdt", 1);
    }
    if (wdt_fd >= 0 && sys_write(wdt_fd, &c, 1) == 1) return 0;
    {
        const char *paths[3];
        long fd; int k;
        paths[0] = "/proc/watchdog/ctrl";
        paths[1] = "/proc/zx_wdt/ctrl";
        paths[2] = "/proc/watchdog";
        for (k = 0; k < 3; k++) {
            fd = sys_open(paths[k], 1);
            if (fd >= 0) {
                sys_write(fd, msg, 7);
                sc1(SYS_CLOSE, fd);
                if (wdt_ok_path == 0) wdt_ok_path = k + 1;
                return 0;
            }
        }
    }
    return -1;
}

/* ---- fork + exec /init ---- */
static void spawn_init(void) {
    long pid = sc1(SYS_FORK, 0);
    if (pid == 0) {
        static char *argv[] = {"/bin/sh", "/init", 0};
        static char *envp[] = {"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
                               "HOME=/", "TERM=linux", 0};
        {
            long rc = sc3(SYS_EXECVE, (long)"/bin/sh", (long)argv, (long)envp);
            char b2[96];
            int bi = 0;
            const char *m = "[INITBIN] exec /bin/sh rc=";
            while (*m) b2[bi++] = *m++;
            { long v = rc; char d[12]; int di = 0; ulong u;
              if (v < 0) { b2[bi++]='-'; u=(ulong)(-v);} else u=(ulong)v;
              if (!u) b2[bi++]='0';
              while (u) { d[di++]=(char)('0'+(u%10)); u/=10; }
              while (di--) b2[bi++]=d[di]; }
            b2[bi]=0;
            klog(b2);
        }
        sc1(SYS_EXIT_GROUP, 9);   /* child only */
    }
    puts_("[INITBIN] spawned /init pid=");
    putdec(pid);
    puts_("\r\n");
}

/* ---- mknod via syscall 14 (ARM EABI legacy table) ---- */
#define SYS_MKNOD 14
static long sys_mknod(const char *p, int mode, int dev) {
    return sc3(SYS_MKNOD, (long)p, mode, dev);
}

void _start(void) {
    int i;
    uart_probe();
    uart_init();
    puts_("\r\n\r\n[INITBIN] v4 ALIVE\r\n");

    {
        long rc1 = sys_mount("proc", "/proc", "proc", 0, 0);
        long rc2 = sys_mount("sysfs", "/sys", "sysfs", 0, 0);
        long rc3 = sys_mount("tmpfs", "/dev", "tmpfs", 0, 0);
        /* /dev is now a writable tmpfs: create the nodes we need */
        sys_mknod("/dev/kmsg", 0x20 | 0600, 0x0b0001);      /* c 1:11 */
        sys_mknod("/dev/null", 0x20 | 0666, 0x010003);      /* c 1:3  */
        sys_mknod("/dev/console", 0x20 | 0600, 0x050001);   /* c 5:1  */
        sys_mknod("/dev/watchdog", 0x20 | 0600, 0x0a0082);  /* c 10:130 */
        sys_mknod("/dev/mem", 0x20 | 0600, 0x010001);       /* c 1:1  */
        /* now /dev/kmsg exists regardless of devtmpfs support */
        klog_add("[INITBIN] v4 alive proc=");
        klog_num(rc1); klog_add(" sys="); klog_num(rc2);
        klog_add(" dev="); klog_num(rc3);
        klog_flush();
        klog("[INITBIN] /dev/kmsg node created - this line proves the channel");
    }

    dump_kmsg_all("KERNEL LOG (boot)");

    /* watchdog: /dev/watchdog (misc 10:130) + proc paths */
    if (feed_watchdog() == 0) {
        klog("[INITBIN] watchdog feed OK first try");
    } else {
        klog("[INITBIN] watchdog feed FAILED first try");
    }

    spawn_init();

    for (i = 1; ; i++) {
        int st = 0;
        sys_nanosleep(3, 0);
        if (feed_watchdog() != 0) {
            if ((i % 10) == 0) klog("[INITBIN] WATCHDOG FEED FAILED (repeat)");
        } else if ((i % 40) == 1) {
            klog_add("[INITBIN] alive t=");
            klog_num((i - 1) * 3);
            klog_add("s wdt_path=");
            klog_num(wdt_ok_path);
            klog_flush();
        }
        for (;;) {
            long r = sc4(SYS_WAIT4, -1, (long)&st, 1, 0);
            if (r <= 0) break;
            klog_add("[INITBIN] child pid=");
            klog_num(r);
            klog_add(" status=0x");
            { ulong u = (ulong)st; const char *hx = "0123456789abcdef"; int k;
              for (k = 28; k >= 0; k -= 4) putc_(hx[(u >> k) & 0xf]); }
            klog_add(" -> respawn /init");
            klog_flush();
            spawn_init();
        }
        if ((i % 2) == 0)
            dump_kmsg_new();
    }
}
