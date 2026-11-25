/* netprobe-init.c — freestanding init.
 *
 * 目的：原厂内核 console 完全不可用（PL011 probe 失败），无法用串口判断
 * rootfs 是否挂载成功、init 是否执行。这里改为把 eth0 配成 192.168.1.1
 * 并拉起，用 ping 作为唯一的判定信号。
 *
 * 构建：
 *   arm-linux-gnueabi-gcc -march=armv7-a -marm -mfloat-abi=soft -O2 \
 *     -static -nostdlib -ffreestanding -Wl,-e,_start -o netprobe-init netprobe-init.c
 */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

static inline long sc(long n, long a, long b, long c) {
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r7 asm("r7") = n;
    asm volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}

static void z(void *p, int n) { u8 *q = p; while (n-- > 0) *q++ = 0; }
static void cpy(char *d, const char *s) { while ((*d++ = *s++)) ; }

#define SIOCSIFADDR     0x8916
#define SIOCSIFFLAGS    0x8914
#define SIOCSIFNETMASK  0x891c

/* struct ifreq: 16 字节名字 + union(16 字节 sockaddr / 2 字节 flags) + pad */
struct ifreq {
    char name[16];
    u16 family;      /* 与 ifr_flags 共用起始位置 */
    u16 port;
    u32 ip;
    u32 zero[2];
    u32 pad[2];
};

/* ip 按网络序写入：192.168.1.1 -> c0 a8 01 01，小端内存里是 0x0101A8C0 */
static void cfg(long fd, const char *dev, u32 ip, u32 mask) {
    struct ifreq r;

    z(&r, sizeof r); cpy(r.name, dev); r.family = 2; r.ip = ip;
    sc(54, fd, SIOCSIFADDR, (long)&r);

    z(&r, sizeof r); cpy(r.name, dev); r.family = 2; r.ip = mask;
    sc(54, fd, SIOCSIFNETMASK, (long)&r);

    z(&r, sizeof r); cpy(r.name, dev);
    r.family = 0x43;                 /* IFF_UP|IFF_BROADCAST|IFF_RUNNING */
    sc(54, fd, SIOCSIFFLAGS, (long)&r);
}

/* 极简 UART（内核若把 CR 关了这里没输出，仅作附加通道） */
#define UB 0x94404000UL
static void putc_(char c) {
    volatile u32 *fr = (u32 *)(UB + 0x18);
    volatile u32 *dr = (u32 *)(UB + 0x00);
    int i;
    for (i = 0; i < 20000; i++) if (!(*fr & (1u << 5))) break;
    *dr = (u32)(u8)c;
}
static void puts_(const char *s) { while (*s) putc_(*s++); }

void _start(void) {
    long fd = sc(281, 2 /*AF_INET*/, 2 /*SOCK_DGRAM*/, 0);
    puts_("\r\n[NETPROBE] start\r\n");
    if (fd < 0) {
        puts_("[NETPROBE] socket fail\r\n");
        for (;;) { }
    }
    cfg(fd, "eth0", 0x0101A8C0u /*192.168.1.1*/, 0x00FFFFFFu /*255.255.255.0*/);
    cfg(fd, "eth1", 0x0201A8C0u /*192.168.1.2*/, 0x00FFFFFFu);
    puts_("[NETPROBE] ip set 192.168.1.1/24 on eth0\r\n");
    for (;;) { }
}
