/* Freestanding LD_PRELOAD connect() interposer for mipsel (uClibc target).
 * Build: mipsel-linux-gnu-gcc -shared -fPIC -nostdlib -O2 -DLISTEN_PORT=11225 -o hook.so hook.c
 * Rewrites a connect to 10.10.10.1:11224 -> 127.0.0.1:LISTEN_PORT; forwards all else.
 * No libc symbols are referenced, so it loads into any libc (glibc-built, uClibc host). */
#ifndef LISTEN_PORT
#define LISTEN_PORT 11225
#endif

/* MIPS o32: __NR_connect = 4170. Args $a0-$a2, num $v0, error flag in $a3. */
static long raw_connect(int fd, const void *addr, unsigned len) {
    register long v0 asm("$2") = 4170;
    register long a0 asm("$4") = fd;
    register long a1 asm("$5") = (long)addr;
    register long a2 asm("$6") = len;
    register long a3 asm("$7") = 0;
    __asm__ volatile("syscall"
        : "+r"(v0), "+r"(a3)
        : "r"(a0), "r"(a1), "r"(a2)
        : "$1","$3","$8","$9","$10","$11","$12","$13","$14","$15","$24","$25","memory");
    if (a3 != 0) return -1;        /* error: mimic libc's -1 (errno not set; see README) */
    return v0;                     /* 0 on success for a blocking connect */
}

int connect(int fd, const void *addr, unsigned int len) {
    const unsigned char *a = (const unsigned char *)addr;
    /* AF_INET == 2 (sa_family u16, little-endian -> a[0]=2,a[1]=0);
       sockaddr_in: [2..3]=port (big-endian), [4..7]=IPv4 (big-endian). */
    if (a && len >= 8 && a[0] == 2 && a[1] == 0) {
        unsigned port = ((unsigned)a[2] << 8) | a[3];
        if (a[4] == 10 && a[5] == 10 && a[6] == 10 && a[7] == 1 && port == 11224) {
            unsigned char buf[28];
            unsigned i, n = len <= sizeof(buf) ? len : sizeof(buf);
            for (i = 0; i < n; i++) buf[i] = a[i];
            buf[2] = (LISTEN_PORT >> 8) & 0xff;   /* port (big-endian) */
            buf[3] = LISTEN_PORT & 0xff;
            buf[4] = 127; buf[5] = 0; buf[6] = 0; buf[7] = 1;   /* 127.0.0.1 */
            return (int)raw_connect(fd, buf, n);   /* clamped len: never read past buf */
        }
    }
    return (int)raw_connect(fd, addr, len);
}
