#ifndef _SGTTY_H
#define _SGTTY_H 1

#include <sys/cdefs.h>
#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sgttyb {
    char sg_ispeed;
    char sg_ospeed;
    char sg_erase;
    char sg_kill;
    short sg_flags;
};

struct tchars {
    char t_intrc;
    char t_quitc;
    char t_startc;
    char t_stopc;
    char t_eofc;
    char t_brkc;
};

struct ltchars {
    char t_suspc;
    char t_dsuspc;
    char t_rprntc;
    char t_flushc;
    char t_werasc;
    char t_lnextc;
};

#define TANDEM      0x00001
#define CBREAK      0x00002
#define LCASE       0x00004
#define ECHO        0x00008
#define CRMOD       0x00010
#define RAW         0x00020
#define ODDP        0x00040
#define EVENP       0x00080
#define ANYP        0x000C0
#define NLDELAY     0x00300
#define NL0         0x00000
#define NL1         0x00100
#define NL2         0x00200
#define NL3         0x00300
#define TBDELAY     0x00C00
#ifndef TAB0
#define TAB0        0x00000
#endif
#ifndef TAB1
#define TAB1        0x00400
#endif
#ifndef TAB2
#define TAB2        0x00800
#endif
#ifndef XTABS
#define XTABS       0x00C00
#endif
#define CRDELAY     0x03000
#ifndef CR0
#define CR0         0x00000
#endif
#ifndef CR1
#define CR1         0x01000
#endif
#ifndef CR2
#define CR2         0x02000
#endif
#ifndef CR3
#define CR3         0x03000
#endif
#define VTDELAY     0x04000
#ifndef FF0
#define FF0         0x00000
#endif
#ifndef FF1
#define FF1         0x04000
#endif
#define BSDELAY     0x08000
#ifndef BS0
#define BS0         0x00000
#endif
#ifndef BS1
#define BS1         0x08000
#endif
#define ALLDELAY    (NLDELAY|TBDELAY|CRDELAY|VTDELAY|BSDELAY)
#define CRTBS       0x10000
#define PRTERA      0x20000
#define CRTERA      0x40000
#define TILDE       0x80000
#define MDMBUF      0x100000
#define LITOUT      0x200000
#ifndef TOSTOP
#define TOSTOP      0x400000
#endif
#ifndef FLUSHO
#define FLUSHO      0x800000
#endif
#define NOHANG      0x1000000
#define L001000     0x2000000
#define CRTKIL      0x4000000
#define PASS8       0x8000000
#define CTLECH      0x10000000
#ifndef PENDIN
#define PENDIN      0x20000000
#endif
#define DECCTQ      0x40000000
#define NOFLSH      0x80000000

#ifndef TIOCGETP
#define TIOCGETP    _IOR('t', 8, struct sgttyb)
#endif
#ifndef TIOCSETP
#define TIOCSETP    _IOW('t', 9, struct sgttyb)
#endif
#ifndef TIOCSETN
#define TIOCSETN    _IOW('t', 10, struct sgttyb)
#endif
#ifndef TIOCGETC
#define TIOCGETC    _IOR('t', 18, struct tchars)
#endif
#ifndef TIOCSETC
#define TIOCSETC    _IOW('t', 17, struct tchars)
#endif
#ifndef TIOCGLTC
#define TIOCGLTC    _IOR('t', 116, struct ltchars)
#endif
#ifndef TIOCSLTC
#define TIOCSLTC    _IOW('t', 117, struct ltchars)
#endif
#ifndef TIOCLGET
#define TIOCLGET    _IOR('t', 124, int)
#endif
#ifndef TIOCLSET
#define TIOCLSET    _IOW('t', 125, int)
#endif
#ifndef TIOCLBIS
#define TIOCLBIS    _IOW('t', 127, int)
#endif
#ifndef TIOCLBIC
#define TIOCLBIC    _IOW('t', 126, int)
#endif

#define LCRTBS      0x0001
#define LPRTERA     0x0002
#define LCRTERA     0x0004
#define LTILDE      0x0008
#define LMDMBUF     0x0010
#define LLITOUT     0x0020
#define LTOSTOP     0x0040
#define LFLUSHO     0x0080
#define LNOHANG     0x0100
#define LCRTKIL     0x0200
#define LPASS8      0x0400
#define LCTLECH     0x0800
#define LPENDIN     0x1000
#define LDECCTQ     0x2000
#define LNOFLSH     0x4000

int gtty(int fd, struct sgttyb *buf);
int stty(int fd, const struct sgttyb *buf);

#ifdef __cplusplus
}
#endif

#endif
