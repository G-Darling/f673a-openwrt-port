/* uart-insmod-diag.c — freestanding ARM init: PL011 takeover + kernel log dump
 * + direct init_module() probe + heartbeat. Installed as /init when the CI
 * workflow runs with uart_init=true.
 *
 * Why: the vendor 4.19 kernel has no working serial console, so every earlier
 * experiment was blind (13s reset loop, no text). This binary:
 *   1. takes the PL011 over by hand (kernel leaves CR=0 after its failed probe)
 *   2. dumps the kernel printk ring buffer via klogctl(READ_ALL)
 *   3. mounts proc/sys/dev, prints /proc/version + /proc/cmdline
 *   4. loads /kmodule/tun.ko with the init_module() syscall (no busybox needed)
 *   5. prints the return code, /proc/modules, then the printk tail again
 *   6. heartbeats "HB<n>" once a second so we can see exactly where it dies
 *
 * NO NAND WRITES anywhere (any real write panics this kernel).
 *
 * Build:
 *   arm-linux-gnueabi-gcc -march=armv7-a -marm -mfloat-abi=soft -O2 \
 *     -static -nostdlib -ffreestanding -Wl,-e,_start -o uart-init uart-insmod-diag.c
 */
typedef unsigned int u32;
typedef unsigned long ulong;
typedef long i32;

#define UART_BASE 0x94404000UL
#define U_DR    (*(volatile u32 *)(UART_BASE + 0x00))
#define U_FR    (*(volatile u32 *)(UART_BASE + 0x18))
#define U_IBRD  (*(volatile u32 *)(UART_BASE + 0x24))
#define U_FBRD  (*(volatile u32 *)(UART_BASE + 0x28))
#define U_LCRH  (*(volatile u32 *)(UART_BASE + 0x2C))
#define U_CR    (*(volatile u32 *)(UART_BASE + 0x30))
#define U_ICR   (*(volatile u32 *)(UART_BASE + 0x44))

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
/* klogctl(type, buf, len) = syscall 103 */
static long sys_klogctl(int type, char *buf, int len) {
    return sc3(103, (long)type, (long)buf, (long)len);
}
/* init_module(void *image, unsigned long len, const char *params) = 128 */
static long sys_init_module(void *img, ulong len, const char *params) {
    return sc3(128, (long)img, (long)len, (long)params);
}
/* nanosleep(struct timespec*) = 162 */
static long sys_nanosleep(long sec, long nsec) {
    long ts[2];
    ts[0] = sec;
    ts[1] = nsec;
    return sc2(162, (long)ts, 0);
}

/* ---- string helpers (no libc) ---- */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void putc_(char c);
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
    int i;
    for (i = 28; i >= 0; i -= 4) putc_(hx[(v >> i) & 0xf]);
}
static void report(const char *label, long v) { puts_(label); putdec(v); puts_("\r\n"); }

/* ---- PL011: take it over explicitly ---- */
static void uart_init(void) {
    u32 ibrd = U_IBRD, fbrd = U_FBRD;
    if (ibrd == 0 || ibrd > 0xffff) { ibrd = 13; fbrd = 1; }  /* 24MHz/115200 */
    U_CR = 0;
    U_ICR = 0x7ff;
    U_IBRD = ibrd;
    U_FBRD = fbrd;
    U_LCRH = 0x70;          /* 8N1 + FIFO */
    U_CR = 0x301;           /* UARTEN | TXE | RXE */
}
static void putc_(char c) {
    int spin = 0;
    while ((U_FR & (1u << 5)) && spin < 2000000) spin++;
    U_DR = (u32)(unsigned char)c;
}
/* Kernel may have changed/stopped the UART clock; try a few common divisors. */
static void uart_probe(void) {
    u32 si = U_IBRD, sf = U_FBRD;
    static const u32 tbl[4][2] = {{13, 1}, {54, 16}, {27, 8}, {6, 49}};
    int k;
    for (k = 0; k < 4; k++) {
        U_CR = 0;
        U_ICR = 0x7ff;
        U_IBRD = tbl[k][0];
        U_FBRD = tbl[k][1];
        U_LCRH = 0x70;
        U_CR = 0x301;
        puts_("\r\n[P");
        putdec(k);
        puts_("] UART ALIVE 115200\r\n");
    }
    U_IBRD = si ? si : 13;
    U_FBRD = sf;
}

/* ---- file helpers ---- */
static void dump_kmsg(const char *title, int clear_first) {
    static char kb[32768];
    long n;
    puts_("\r\n====");
    puts_(title);
    puts_("====\r\n");
    if (clear_first) sys_klogctl(4, kb, 0);      /* READ_CLEAR, discard old */
    n = sys_klogctl(3, kb, (int)sizeof(kb) - 1); /* READ_ALL */
    if (n <= 0) { report("[kmsg] rc=", n); return; }
    if (n > (long)sizeof(kb) - 1) n = sizeof(kb) - 1;
    {
        long i;
        for (i = 0; i < n; i++) {
            char c = kb[i];
            if (c == '\n') putc_('\r');
            putc_(c);
        }
    }
    puts_("\r\n====end kmsg====\r\n");
}
static void dump_file(const char *path, int max) {
    static char buf[512];
    long fd = sc3(5, (long)path, 0, 0);
    int total = 0;
    if (fd < 0) { puts_("  [open "); puts_(path); puts_(" failed "); putdec(fd); puts_("]\r\n"); return; }
    for (;;) {
        long n = sc3(3, fd, (long)buf, (long)sizeof(buf));
        long i;
        if (n <= 0) break;
        for (i = 0; i < n; i++) { char c = buf[i]; if (c == '\n') putc_('\r'); putc_(c); }
        total += (int)n;
        if (total > max) { puts_("  ...truncated\r\n"); break; }
    }
    sc1(6, fd);
}

/* ---- load one module via init_module(2) ---- */
static void insmod_one(const char *path) {
    static char img[320 * 1024];
    long fd, n, rc, total = 0;
    puts_("\r\n[insmod] opening ");
    puts_(path);
    puts_("\r\n");
    fd = sc3(5, (long)path, 0, 0);
    if (fd < 0) { report("[insmod] open rc=", fd); return; }
    for (;;) {
        n = sc3(3, fd, (long)(img + total), (long)(sizeof(img) - total));
        if (n <= 0) break;
        total += n;
        if ((ulong)total >= sizeof(img)) break;
    }
    sc1(6, fd);
    report("[insmod] bytes read = ", total);
    if (total <= 0) return;
    puts_("[insmod] calling init_module() ...\r\n");
    rc = sys_init_module(img, (ulong)total, "");
    puts_("[insmod] init_module rc = ");
    putdec(rc);
    puts_(" (errno ");
    putdec(-rc);
    puts_(")\r\n");
}

void _start(void) {
    int i;
    /* 1. bring the UART back to life first (kernel may have left CR=0) */
    uart_probe();
    uart_init();
    puts_("\r\n\r\n[UART-DIAG] v10 ALIVE\r\n");

    /* 2. kernel view before anything else */
    dump_kmsg("KERNEL LOG (boot)", 0);

    /* 3. mounts + version info */
    sys_mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
    sys_mount("proc", "/proc", "proc", 0, 0);
    sys_mount("sysfs", "/sys", "sysfs", 0, 0);
    puts_("---- /proc/version ----\r\n");   dump_file("/proc/version", 400);
    puts_("---- /proc/cmdline ----\r\n");   dump_file("/proc/cmdline", 600);

    /* 4. the actual experiment: load tun.ko through the syscall */
    insmod_one("/kmodule/tun.ko");
    puts_("---- /proc/modules ----\r\n");   dump_file("/proc/modules", 800);
    dump_kmsg("KERNEL LOG (after insmod)", 0);

    /* 5. heartbeat: if the board resets ~13s from now we see where HB stopped */
    puts_("\r\n[UART-DIAG] heartbeat start\r\n");
    for (i = 1; i <= 120; i++) {
        puts_("HB");
        putdec(i);
        puts_("\r\n");
        sys_nanosleep(1, 0);
        if ((i % 10) == 0) dump_kmsg("KERNEL LOG (hb)", 1);
    }
    puts_("[UART-DIAG] 120s heartbeat done, idling\r\n");
    for (;;) sys_nanosleep(60, 0);
}
