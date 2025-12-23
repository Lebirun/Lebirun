#if !defined(__is_libk)

#include <termios.h>

speed_t cfgetispeed(const struct termios *termios_p) {
    return termios_p->c_ispeed;
}

speed_t cfgetospeed(const struct termios *termios_p) {
    return termios_p->c_ospeed;
}

int cfsetispeed(struct termios *termios_p, speed_t speed) {
    termios_p->c_ispeed = speed;
    return 0;
}

int cfsetospeed(struct termios *termios_p, speed_t speed) {
    termios_p->c_ospeed = speed;
    return 0;
}

int cfsetspeed(struct termios *termios_p, speed_t speed) {
    termios_p->c_ispeed = speed;
    termios_p->c_ospeed = speed;
    return 0;
}

#endif
