#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H 1

#include <sys/cdefs.h>
#include <termios.h>

#define _IOC(inout,group,num,len) \
    ((inout) | (((len) & 0x1FFF) << 16) | ((group) << 8) | (num))
#define _IO(g,n)        _IOC(0x20000000, (g), (n), 0)
#define _IOR(g,n,t)     _IOC(0x40000000, (g), (n), sizeof(t))
#define _IOW(g,n,t)     _IOC(0x80000000, (g), (n), sizeof(t))
#define _IOWR(g,n,t)    _IOC(0xC0000000, (g), (n), sizeof(t))

#define TIOCGETA    0x5401
#define TCGETS      0x5401
#define TIOCSETA    0x5402
#define TCSETS      0x5402
#define TIOCSETAW   0x5403
#define TCSETSW     0x5403
#define TIOCSETAF   0x5404
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define FIONREAD    0x541B
#define FIONBIO     0x5421
#define TIOCNOTTY   0x5422
#define TIOCSCTTY   0x540E
#define TIOCEXCL    0x540C
#define TIOCNXCL    0x540D
#define TIOCOUTQ    0x5411
#define TIOCSTI     0x5412
#define TIOCCONS    0x541D
#define TIOCPKT     0x5420
#define TIOCGSID    0x5429
#define TIOCGETD    0x5424
#define TIOCSETD    0x5423
#define TIOCSBRK    0x5427
#define TIOCCBRK    0x5428
#define TIOCGSOFTCAR 0x5419
#define TIOCSSOFTCAR 0x541A

#ifndef TIOCFLUSH
#define TIOCFLUSH TCIOFLUSH
#endif

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
