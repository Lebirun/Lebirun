#include <sgtty.h>
#include <termios.h>
#include <errno.h>
#include <string.h>

#if !defined(__is_libk)

static void termios_to_sgttyb(const struct termios *t, struct sgttyb *s) {
    s->sg_ispeed = (char)(t->c_ispeed > 38400 ? 15 : t->c_ispeed / 300);
    s->sg_ospeed = (char)(t->c_ospeed > 38400 ? 15 : t->c_ospeed / 300);
    s->sg_erase = t->c_cc[VERASE];
    s->sg_kill = t->c_cc[VKILL];
    
    s->sg_flags = 0;
    
    if (t->c_lflag & ECHO)
        s->sg_flags |= ECHO;
    
    if (t->c_oflag & ONLCR)
        s->sg_flags |= CRMOD;
    
    if (!(t->c_lflag & ICANON)) {
        if (t->c_lflag & ISIG)
            s->sg_flags |= CBREAK;
        else
            s->sg_flags |= RAW;
    }
    
    if (t->c_iflag & IXON)
        s->sg_flags |= TANDEM;
    
    if (t->c_cflag & PARENB) {
        if (t->c_cflag & PARODD)
            s->sg_flags |= ODDP;
        else
            s->sg_flags |= EVENP;
    }
    
    if (t->c_iflag & IUCLC)
        s->sg_flags |= LCASE;
}

static void sgttyb_to_termios(const struct sgttyb *s, struct termios *t) {
    static const speed_t speeds[] = {
        B0, B50, B75, B110, B134, B150, B200, B300,
        B600, B1200, B1800, B2400, B4800, B9600, B19200, B38400
    };
    
    if (s->sg_ispeed >= 0 && s->sg_ispeed <= 15)
        t->c_ispeed = speeds[(int)s->sg_ispeed];
    if (s->sg_ospeed >= 0 && s->sg_ospeed <= 15)
        t->c_ospeed = speeds[(int)s->sg_ospeed];
    
    t->c_cc[VERASE] = s->sg_erase;
    t->c_cc[VKILL] = s->sg_kill;
    
    if (s->sg_flags & RAW) {
        t->c_iflag = 0;
        t->c_oflag = 0;
        t->c_lflag = 0;
        t->c_cflag = CS8 | CREAD;
    } else if (s->sg_flags & CBREAK) {
        t->c_iflag = BRKINT | IGNPAR | ICRNL | IXON;
        t->c_oflag = OPOST | ONLCR;
        t->c_lflag = ISIG;
        t->c_cflag = CS8 | CREAD | HUPCL;
    } else {
        t->c_iflag = BRKINT | IGNPAR | ICRNL | IXON;
        t->c_oflag = OPOST | ONLCR;
        t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
        t->c_cflag = CS8 | CREAD | HUPCL;
    }
    
    if (s->sg_flags & ECHO)
        t->c_lflag |= ECHO;
    else
        t->c_lflag &= ~ECHO;
    
    if (s->sg_flags & CRMOD)
        t->c_oflag |= ONLCR;
    else
        t->c_oflag &= ~ONLCR;
    
    if (s->sg_flags & TANDEM)
        t->c_iflag |= IXON | IXOFF;
    else
        t->c_iflag &= ~(IXON | IXOFF);
    
    if (s->sg_flags & LCASE) {
        t->c_iflag |= IUCLC;
        t->c_oflag |= OLCUC;
    } else {
        t->c_iflag &= ~IUCLC;
        t->c_oflag &= ~OLCUC;
    }
    
    t->c_cflag &= ~(PARENB | PARODD);
    if (s->sg_flags & ODDP)
        t->c_cflag |= PARENB | PARODD;
    else if (s->sg_flags & EVENP)
        t->c_cflag |= PARENB;
}

int gtty(int fd, struct sgttyb *buf) {
    if (!buf) {
        errno = EINVAL;
        return -1;
    }
    
    struct termios t;
    int ret = tcgetattr(fd, &t);
    if (ret < 0)
        return ret;
    
    termios_to_sgttyb(&t, buf);
    return 0;
}

int stty(int fd, const struct sgttyb *buf) {
    if (!buf) {
        errno = EINVAL;
        return -1;
    }
    
    struct termios t;
    int ret = tcgetattr(fd, &t);
    if (ret < 0)
        return ret;
    
    sgttyb_to_termios(buf, &t);
    return tcsetattr(fd, TCSADRAIN, &t);
}

#else

int gtty(int fd, struct sgttyb *buf) {
    (void)fd;
    (void)buf;
    return -1;
}

int stty(int fd, const struct sgttyb *buf) {
    (void)fd;
    (void)buf;
    return -1;
}

#endif
