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

/* ---- watchdog: two paths, never give up ---- */
static int wdt_fd = -1;
static int feed_watchdog(void) {
    char c = 'F';
    char msg[8] = "feeddog";
    if (wdt_fd >= 0) {
        if (sys_write(wdt_fd, &c, 1) == 1) return 0;
        sc1(SYS_CLOSE, wdt_fd);
        wdt_fd = -1;
    }
    if (wdt_fd < 0)
        wdt_fd = (int)sys_open("/dev/watchdog", 1 /* O_WRONLY */);
    if (wdt_fd >= 0 && sys_write(wdt_fd, &c, 1) == 1) return 0;
    {
        long fd = sys_open("/proc/watchdog/ctrl", 1);
        if (fd >= 0) {
            sys_write(fd, msg, 7);
            sc1(SYS_CLOSE, fd);
            return 0;
        }
        fd = sys_open("/proc/watchdog", 1);
        if (fd >= 0) {
            sys_write(fd, msg, 7);
            sc1(SYS_CLOSE, fd);
            return 0;
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
        sc3(SYS_EXECVE, (long)"/bin/sh", (long)argv, (long)envp);
        sc1(SYS_EXIT_GROUP, 9);   /* child only */
    }
    puts_("[INITBIN] spawned /init pid=");
    putdec(pid);
    puts_("\r\n");
}

void _start(void) {
    int i;
    uart_probe();
    uart_init();
    puts_("\r\n\r\n[INITBIN] v2 ALIVE (static PID1: uart + kmsg + wdt + /init)\r\n");
    dump_kmsg_all("KERNEL LOG (boot)");

    sys_mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
    sys_mount("proc", "/proc", "proc", 0, 0);
    sys_mount("sysfs", "/sys", "sysfs", 0, 0);
    puts_("[INITBIN] mounts done\r\n");

    spawn_init();

    for (i = 1; ; i++) {
        int st = 0;
        sys_nanosleep(3, 0);
        if (feed_watchdog() != 0) {
            puts_("[INITBIN] WATCHDOG FEED FAILED t=");
            putdec(i * 3);
            puts_("s\r\n");
        } else if ((i % 40) == 1) {
            puts_("[INITBIN] alive t=");
            putdec((i - 1) * 3);
            puts_("s wdt=ok\r\n");
        }
        /* reap exited children, WNOHANG=1: wait4(-1, &st, 1, 0) */
        for (;;) {
            long r = sc4(SYS_WAIT4, -1, (long)&st, 1, 0);
            if (r <= 0) break;
            puts_("[INITBIN] child pid=");
            putdec(r);
            puts_(" exited status=0x");
            puthex((ulong)st);
            puts_(" -> respawning /init\r\n");
            spawn_init();
        }
        if ((i % 2) == 0)
            dump_kmsg_new();     /* mirror fresh kmsg (script trace) to UART */
    }
}
