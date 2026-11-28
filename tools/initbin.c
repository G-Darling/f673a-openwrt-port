/* initbin.c v6 — freestanding ARM PID1 for the ZTE F673AV9a (vendor 4.19 kernel).
 *
 * v6 CHANGES: NO UART MMIO ACCESS AT ALL. The PL011 at 0x94404000 is not
 * mapped into user space; touching it SIGSEGVs PID1 (suspected root cause of
 * the v2-v5 blackouts). The serial wire is dead output on this platform
 * anyway (even the vendor system prints nothing after "Uncompressing Linux").
 * All diagnostics go through /dev/kmsg -> kernel printk ring -> minable from
 * RAM after warm reset via U-Boot md.b.
 *
 * Flow:
 *   1. mount proc (+ sysfs after channel live)
 *   2. mount a writable /dev (tmpfs, fallback ramfs, fallback devtmpfs)
 *   3. mknod /dev/kmsg (chr 1:11) -> klog channel live
 *   4. klog every rc (mounts, mknod, watchdog feed paths, spawn)
 *   5. watchdog feed every 3s: /dev/watchdog, /proc/watchdog/ctrl "feeddog",
 *      /proc/zx_wdt/ctrl "feeddog"
 *   6. spawn /bin/sh /init (bring-up script); respawn automatically on death
 *   7. PID1 NEVER exits
 *
 * NO NAND WRITES anywhere.
 *
 * Build:
 *   arm-linux-gnueabi-gcc -march=armv7-a -marm -mfloat-abi=soft -O2 \
 *     -static -nostdlib -ffreestanding -Wl,-e,_start -o initbin initbin.c
 */
typedef unsigned int u32;
typedef unsigned long ulong;

/* ---- raw syscalls (ARM EABI, r7 = number) ---- */
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

#define SYS_EXIT_GROUP 248
#define SYS_REBOOT     88
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_FORK       2
#define SYS_EXECVE     11
#define SYS_MOUNT      21
#define SYS_KLOGCTL    103
#define SYS_NANOSLEEP  162
#define SYS_WAIT4      258
#define SYS_MKNOD      14

static long sys_write(int fd, const char *buf, long n) { return sc3(4, fd, (long)buf, n); }
static long sys_open(const char *p, long fl) { return sc3(SYS_OPEN, (long)p, fl, 0); }
static long sys_mount(const char *src, const char *tgt, const char *fs, ulong fl, const void *d) {
    return sc5(SYS_MOUNT, (long)src, (long)tgt, (long)fs, (long)fl, (long)d);
}
static long sys_klogctl(int type, char *buf, int len) { return sc3(SYS_KLOGCTL, type, (long)buf, len); }
static long sys_nanosleep(long sec, long nsec) {
    long ts[2]; ts[0] = sec; ts[1] = nsec;
    return sc2(SYS_NANOSLEEP, (long)ts, 0);
}
static long sys_mknod(const char *p, int mode, int dev) { return sc3(SYS_MKNOD, (long)p, mode, dev); }

/* ---- klog channel ---- */
static int kmsg_fd = -1;
static void klog(const char *s) {
    char buf[256];
    int i = 0;
    const char *p = s;
    if (kmsg_fd < 0) return;
    while (*p && i < 200) buf[i++] = *p++;
    buf[i++] = '\n';
    sys_write(kmsg_fd, buf, i);
}
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
static void klog_hexword(long v) {
    const char *hx = "0123456789abcdef";
    int k;
    for (k = 28; k >= 0; k -= 4) kline[klen++] = hx[(v >> k) & 0xf];
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
    if (wdt_fd < 0)
        wdt_fd = (int)sys_open("/dev/watchdog", 1);
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
                wdt_ok_path = k + 2;
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
        sc3(SYS_EXECVE, (long)"/bin/sh", (long)argv, (long)envp);
        sc1(SYS_EXIT_GROUP, 9);
    }
    klog_add("[INITBIN] spawned /init pid=");
    klog_num(pid);
    klog_flush();
}

void _start(void) {
    int i;
    long rc;
    long fd;

    /* 1. mounts: proc (independent of /dev) */
    rc = sys_mount("proc", "/proc", "proc", 0, 0);
    klen = 0;
    klog_add("[INITBIN] v13 alive mount proc=");
    klog_num(rc);

    /* 2. writable /dev: tmpfs -> ramfs -> devtmpfs */
    rc = sys_mount("tmpfs", "/dev", "tmpfs", 0, 0);
    if (rc != 0) rc = sys_mount("ramfs", "/dev", "ramfs", 0, 0);
    if (rc != 0) rc = sys_mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
    klog_add(" dev=");
    klog_num(rc);

    /* 3. mknod /dev/kmsg (chr 1:11) - works on tmpfs/ramfs */
    rc = sys_mknod("/dev/kmsg", 0x2000 | 0622, (1 << 8) | 11);
    klog_add(" mknod=");
    klog_num(rc);

    /* open the kmsg channel */
    fd = sys_open("/dev/kmsg", 1);
    kmsg_fd = (int)fd;
    klog_add(" kmsg=");
    klog_num(fd);
    klog_flush();

    if (kmsg_fd < 0) {
        /* nothing else to do but idle (never exit, avoid init-kill panic) */
        for (;;)
            sys_nanosleep(150, 0);
    }

    klog("[INITBIN] kmsg channel live - diagnostics from here on");

    /* mount sysfs now that we have a channel */
    rc = sys_mount("sysfs", "/sys", "sysfs", 0, 0);
    klog_add("[INITBIN] sysfs rc=");
    klog_num(rc);
    klog_flush();

    /* 4. extra device nodes */
    sys_mknod("/dev/null", 0x2000 | 0666, (1 << 8) | 3);
    sys_mknod("/dev/console", 0x2000 | 0600, (5 << 8) | 1);
    sys_mknod("/dev/watchdog", 0x2000 | 0600, (10 << 8) | 130);
    sys_mknod("/dev/mem", 0x2000 | 0600, (1 << 8) | 1);

    /* 5. watchdog status probe */
    {
        long w = sys_open("/dev/watchdog", 1);
        klog_add("[INITBIN] /dev/watchdog open=");
        klog_num(w);
        if (w >= 0) { wdt_fd = (int)w; klog_add(" (kept open)"); }
        klog_flush();
    }

    /* 6. spawn the bring-up script */
    spawn_init();

    /* 7. PID1 never exits: feed wdt, reap children, respawn /init.
     * v8 DIAG MODE: after REBOOT_SECONDS PID1 issues reboot(2) itself
     * (warm reset, DRAM preserved) so the __log_buf with the full
     * [V19] trace can be mined from U-Boot. Board then cycles:
     * Linux ~60s + boot ~35s. Interrupt autoboot to mine. */
#define REBOOT_SECONDS 240
    for (i = 1; ; i++) {
        int st = 0;
        sys_nanosleep(3, 0);
        if ((i - 1) * 3 >= REBOOT_SECONDS) {
            klog("[INITBIN] diag: issuing warm reboot now");
            sc3(SYS_REBOOT, (long)0xfee1dead, (long)672274793, (long)0x1234567);
            /* reboot must not return; if it did, keep logging */
            klog("[INITBIN] diag: reboot syscall returned (!) - looping");
        }
        if (feed_watchdog() != 0) {
            if ((i % 10) == 0)
                klog("[INITBIN] watchdog feed FAILED (all paths)");
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
            klog_hexword(st);
            klog_add(" -> respawn");
            klog_flush();
            spawn_init();
        }
    }
}
