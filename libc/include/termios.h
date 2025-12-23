#ifndef _TERMIOS_H
#define _TERMIOS_H 1

#include <sys/cdefs.h>

#define NCCS 32

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define VEOF     0
#define VEOL     1
#define VEOL2    2
#define VERASE   3
#define VWERASE  4
#define VKILL    5
#define VREPRINT 6
#define VSWTC    7
#define VINTR    8
#define VQUIT    9
#define VSUSP    10
#define VDSUSP   11
#define VSTART   12
#define VSTOP    13
#define VLNEXT   14
#define VDISCARD 15
#define VMIN     16
#define VTIME    17
#define VSTATUS  18

#define IGNBRK  0x00001
#define BRKINT  0x00002
#define IGNPAR  0x00004
#define PARMRK  0x00008
#define INPCK   0x00010
#define ISTRIP  0x00020
#define INLCR   0x00040
#define IGNCR   0x00080
#define ICRNL   0x00100
#define IXON    0x00200
#define IXOFF   0x00400
#define IXANY   0x00800
#define IUCLC   0x01000
#define IMAXBEL 0x02000
#define IUTF8   0x04000

#define OPOST   0x00001
#define ONLCR   0x00002
#define OLCUC   0x00004
#define OCRNL   0x00008
#define ONOCR   0x00010
#define ONLRET  0x00020
#define OFILL   0x00040
#define OFDEL   0x00080
#define NLDLY   0x00100
#define NL0     0x00000
#define NL1     0x00100
#define CRDLY   0x00600
#define CR0     0x00000
#define CR1     0x00200
#define CR2     0x00400
#define CR3     0x00600
#define TABDLY  0x01800
#define TAB0    0x00000
#define TAB1    0x00800
#define TAB2    0x01000
#define TAB3    0x01800
#define XTABS   0x01800
#define BSDLY   0x02000
#define BS0     0x00000
#define BS1     0x02000
#define VTDLY   0x04000
#define VT0     0x00000
#define VT1     0x04000
#define FFDLY   0x08000
#define FF0     0x00000
#define FF1     0x08000

#define CSIZE   0x00300
#define CS5     0x00000
#define CS6     0x00100
#define CS7     0x00200
#define CS8     0x00300
#define CSTOPB  0x00400
#define CREAD   0x00800
#define PARENB  0x01000
#define PARODD  0x02000
#define HUPCL   0x04000
#define CLOCAL  0x08000
#define CBAUD   0x100F
#define CBAUDEX 0x1000
#define CIBAUD  0x100F0000
#define CMSPAR  0x40000000
#define CRTSCTS 0x80000000

#define ECHOKE  0x00001
#define ECHOE   0x00002
#define ECHOK   0x00004
#define ECHO    0x00008
#define ECHONL  0x00010
#define ECHOPRT 0x00020
#define ECHOCTL 0x00040
#define ISIG    0x00080
#define ICANON  0x00100
#define ALTWERASE 0x00200
#define IEXTEN  0x00400
#define EXTPROC 0x00800
#define TOSTOP  0x01000
#define FLUSHO  0x02000
#define NOKERNINFO 0x04000
#define PENDIN  0x08000
#define NOFLSH  0x80000000

#define B0      0
#define B50     50
#define B75     75
#define B110    110
#define B134    134
#define B150    150
#define B200    200
#define B300    300
#define B600    600
#define B1200   1200
#define B1800   1800
#define B2400   2400
#define B4800   4800
#define B9600   9600
#define B19200  19200
#define B38400  38400
#define B57600  57600
#define B115200 115200
#define B230400 230400
#define B460800 460800
#define B500000 500000
#define B576000 576000
#define B921600 921600
#define B1000000 1000000
#define B1152000 1152000
#define B1500000 1500000
#define B2000000 2000000
#define B2500000 2500000
#define B3000000 3000000
#define B3500000 3500000
#define B4000000 4000000

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

#define TCOOFF    0
#define TCOON     1
#define TCIOFF    2
#define TCION     3

#ifdef __cplusplus
extern "C" {
#endif

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);
int tcsendbreak(int fd, int duration);
int tcdrain(int fd);
int tcflush(int fd, int queue_selector);
int tcflow(int fd, int action);
void cfmakeraw(struct termios *termios_p);
speed_t cfgetispeed(const struct termios *termios_p);
speed_t cfgetospeed(const struct termios *termios_p);
int cfsetispeed(struct termios *termios_p, speed_t speed);
int cfsetospeed(struct termios *termios_p, speed_t speed);
int cfsetspeed(struct termios *termios_p, speed_t speed);
int tcgetpgrp(int fd);
int tcsetpgrp(int fd, int pgrp);

#ifdef __cplusplus
}
#endif

#endif
