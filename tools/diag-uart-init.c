/* diag-uart-init.c — freestanding ARM init that talks to PL011 directly.
 *
 * Why: the vendor 4.19 kernel has no working serial console (pl011 platform
 * driver fails to register, CONFIG_VFP=n). So we bypass the kernel console
 * entirely: write to the UART data register ourselves, and capture child
 * process output through a pipe.
 *
 * Build:
 *   arm-linux-gnueabi-gcc -march=armv7-a -marm -mfloat-abi=soft -O2 \
 *     -static -nostdlib -ffreestanding -Wl,-e,_start -o uart-init diag-uart-init.c
 */
typedef unsigned int u32;
typedef unsigned long ulong;

#define UART_BASE 0x94404000UL
#define UART_DR   (*(volatile u32 *)(UART_BASE + 0x00))
#define UART_FR   (*(volatile u32 *)(UART_BASE + 0x18))

/* ---- syscalls (ARM EABI, r7 = number) ---- */
static inline long sc1(long n, long a) {
    register long r0 asm("r0") = a;
    register long r7 asm("r7") = n;
    asm volatile("svc #0" : "+r"(r0) : "r"(r7) : "memory");
    return r0;
}
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
static long sys_mount(const char *s, const char *t, const char *f, ulong fl, const void *d) {
    register long r0 asm("r0") = (long)s;
    register long r1 asm("r1") = (long)t;
    register long r2 asm("r2") = (long)f;
    register long r3 asm("r3") = (long)fl;
    register long r4 asm("r4") = (long)d;
    register long r7 asm("r7") = 21;
    asm volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r7) : "memory");
    return r0;
}

/* ---- tiny UART console ---- */
static void putc_(char c) {
    while (UART_FR & (1u << 5)) { }
    UART_DR = (u32)(unsigned char)c;
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
    const char *h = "0123456789abcdef"; int i;
    puts_("0x");
    for (i = 15; i >= 0; i--) putc_(h[(v >> (i * 4)) & 0xf]);
}

/* ---- helpers ---- */
static void dump_file(const char *path, int max) {
    char buf[400];
    long fd = sc3(5, (long)path, 0, 0);
    if (fd < 0) { puts_("  [open "); puts_(path); puts_(" failed "); putdec(fd); puts_("]\r\n"); return; }
    int total = 0;
    for (;;) {
        long n = sc3(3, fd, (long)buf, (long)sizeof(buf));
        if (n <= 0) break;
        for (long i = 0; i < n; i++) { char c = buf[i]; if (c == '\n') putc_('\r'); putc_(c); }
        total += (int)n;
        if (total > max) { puts_("  ...truncated\r\n"); break; }
    }
    sc1(6, fd);
}

static void report(const char *label, long v) {
    puts_(label); putdec(v); puts_("\r\n");
}

static void run_cmd(const char *cmd) {
    long p[2];
    if (sc2(42, (long)p, 0) < 0) { puts_("[run] pipe failed\r\n"); return; }
    long pid = sc1(2, 0);            /* fork */
    if (pid == 0) {
        sc2(63, p[1], 1);            /* dup2 stdout */
        sc2(63, p[1], 2);            /* dup2 stderr */
        sc1(6, p[0]); sc1(6, p[1]);
        char *argv[] = { (char *)"/bin/busybox", (char *)"sh", (char *)"-c", (char *)cmd, 0 };
        char *envp[] = { (char *)"PATH=/sbin:/bin:/usr/sbin:/usr/bin", (char *)"HOME=/", 0 };
        long e = sc3(11, (long)"/bin/busybox", (long)argv, (long)envp);
        puts_("[run] execve /bin/busybox failed, errno="); putdec(-e); puts_("\r\n");
        sc1(1, 127);
    }
    sc1(6, p[1]);
    char buf[400];
    for (;;) {
        long n = sc3(3, p[0], (long)buf, (long)sizeof(buf));
        if (n <= 0) break;
        for (long i = 0; i < n; i++) { char c = buf[i]; if (c == '\n') putc_('\r'); putc_(c); }
    }
    sc1(6, p[0]);
    int st = 0;
    sc4(114, pid, (long)&st, 0, 0);
    puts_("[run] exit status="); putdec(st);
    puts_(" signal="); putdec(st & 0x7f); puts_("\r\n");
}

/* ---- entry ---- */
void _start(void) {
    puts_("\r\n\r\n[UART-INIT] ALIVE\r\n");

    sc2(39, (long)"/proc", 0555);    /* mkdir */
    sc2(39, (long)"/sys", 0555);
    sc2(39, (long)"/dev", 0755);
    sc2(39, (long)"/tmp", 0777);

    report("[UART-INIT] mount proc = ", sys_mount("proc", "/proc", "proc", 0, 0));
    report("[UART-INIT] mount sysfs = ", sys_mount("sysfs", "/sys", "sysfs", 0, 0));
    report("[UART-INIT] mount devtmpfs = ", sys_mount("devtmpfs", "/dev", "devtmpfs", 0, 0));

    puts_("---- /proc/version ----\r\n");   dump_file("/proc/version", 400);
    puts_("---- /proc/cmdline ----\r\n");   dump_file("/proc/cmdline", 600);
    puts_("---- /proc/mtd ----\r\n");       dump_file("/proc/mtd", 2000);
    puts_("---- /proc/mounts ----\r\n");    dump_file("/proc/mounts", 1200);
    puts_("---- /etc/openwrt_release ----\r\n"); dump_file("/etc/openwrt_release", 600);

    puts_("---- test: busybox echo ----\r\n");
    run_cmd("echo BUSYBOX-OK; uname -a");

    puts_("---- test: busybox ls / ----\r\n");
    run_cmd("ls /");

    puts_("---- test: /dev/ttyAMA0 ----\r\n");
    report("[open /dev/ttyAMA0] fd = ", sc3(5, (long)"/dev/ttyAMA0", 1, 0));

    puts_("[UART-INIT] DONE\r\n");
    for (;;) { }
}
