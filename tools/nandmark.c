/*
 * nandmark - raw NAND marker writer for the F673A bring-up.
 *
 * The vendor kernel panics on any write through the mtdblock CACHE layer
 * (proven by v5/v6: any real dd to /dev/mtdblockN reset-loops the board,
 * while a dd beyond end-of-device fails harmlessly and the board is stable).
 *
 * This tool writes via the mtd CHAR device instead: MEMERASE the target
 * erase block, then pwrite the payload (writesize-padded, 0xFF filled).
 * That is the same path jffs2 uses internally and is known-good here.
 *
 * usage: nandmark <mtdchar_dev> <file> <hex_offset>
 *        offset must be erase-block AND writesize aligned.
 *        prints "nandmark rc=<n>"; rc 0 = ok.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * The arm-linux-gnueabi sysroot has no linux/mtd/mtd-user.h, so the MTD
 * ABI is declared here. These ioctl numbers are a frozen ABI:
 *   MEMGETINFO = _IOR('M', 1, int)                 = 0x80044D01
 *   MEMERASE   = _IOW('M', 2, struct erase_info_user(8)) = 0x40084D02
 */
struct mtd_info_u {
    unsigned char type;
    unsigned int flags;
    unsigned int size;
    unsigned int erasesize;
    unsigned int writesize;
    unsigned int oobsize;
    unsigned long long padding;
    unsigned int eccbytes;
    unsigned int eccstrength;
};
struct erase_info_u {
    unsigned int start;
    unsigned int length;
};
#define MTD_MEMGETINFO 0x80044D01U
#define MTD_MEMERASE   0x40084D02U>

#define MAXSZ (128 * 1024)

static char buf[MAXSZ];

static int do_write(int fd, const char *file, unsigned long off)
{
    struct mtd_info_u mi;
    struct erase_info_u ei;
    FILE *f;
    unsigned long ebs, astart, plen, sz;
    ssize_t n;

    if (ioctl(fd, MTD_MEMGETINFO, &mi) < 0)
        return -10;
    ebs = mi.erasesize;
    if (off % mi.writesize || off % ebs)
        return -11;
    astart = (off / ebs) * ebs;

    memset(buf, 0xff, sizeof(buf));
    f = fopen(file, "rb");
    if (!f)
        return -12;
    sz = fread(buf, 1, MAXSZ, f);
    fclose(f);

    ei.start = astart;
    ei.length = ebs;
    if (ioctl(fd, MTD_MEMERASE, &ei) < 0)
        return -13;

    plen = ((sz + mi.writesize - 1) / mi.writesize) * mi.writesize;
    if (plen > ebs)
        plen = ebs;
    if (plen == 0)
        plen = mi.writesize;
    n = pwrite(fd, buf, plen, (off_t)off);
    if (n < 0 || (unsigned long)n != plen)
        return -14;
    return 0;
}

int main(int argc, char **argv)
{
    int fd, rc;
    unsigned long off;

    if (argc != 4) {
        fprintf(stderr, "usage: nandmark <mtdchar_dev> <file> <hex_offset>\n");
        return 2;
    }
    off = strtoul(argv[3], NULL, 16);
    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        printf("nandmark rc=-1 errno=%d\n", errno);
        return 1;
    }
    rc = do_write(fd, argv[2], off);
    close(fd);
    printf("nandmark rc=%d\n", rc);
    return rc ? 1 : 0;
}
