#include "syscall_defs.h"

struct kernel_termios tty_termios[NUM_CONSOLES];
struct kernel_winsize tty_winsize[NUM_CONSOLES];
int tty_pgrp[NUM_CONSOLES];

static void termios_init_defaults(int tty_id) {
    struct kernel_termios *t = &tty_termios[tty_id];
    
    t->c_iflag = ICRNL | IXON;
    
    t->c_oflag = OPOST | ONLCR;
    
    t->c_cflag = CS8 | CREAD | CLOCAL;
    
    t->c_lflag = ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ISIG | IEXTEN;
    
    memset(t->c_cc, 0, NCCS);
    t->c_cc[VEOF]   = 4;
    t->c_cc[VEOL]   = 0;
    t->c_cc[VERASE] = 127;
    t->c_cc[VKILL]  = 21;
    t->c_cc[VINTR]  = 3;
    t->c_cc[VQUIT]  = 28;
    t->c_cc[VSUSP]  = 26;
    t->c_cc[VSTART] = 17;
    t->c_cc[VSTOP]  = 19;
    t->c_cc[VMIN]   = 1;
    t->c_cc[VTIME]  = 0;
    
    t->c_ispeed = 38400;
    t->c_ospeed = 38400;
    
    framebuffer_t *fb = fb_get();
    if (fb && fb->font) {
        tty_winsize[tty_id].ws_col = fb->cols;
        tty_winsize[tty_id].ws_row = fb->rows;
        tty_winsize[tty_id].ws_xpixel = fb->width;
        tty_winsize[tty_id].ws_ypixel = fb->height;
    } else {
        tty_winsize[tty_id].ws_col = 80;
        tty_winsize[tty_id].ws_row = 25;
        tty_winsize[tty_id].ws_xpixel = 0;
        tty_winsize[tty_id].ws_ypixel = 0;
    }
    
    tty_pgrp[tty_id] = 0;
}

static int get_tty_id_for_fd(int fd) {
    if (fd < 0 || fd > 2) return -1;
    
    if (current_task && current_task->console_id >= 0) {
        return current_task->console_id;
    }
    return console_get_current();
}

static int sys_tcgetattr(int fd, const char *termios_ptr, int unused) {
    (void)unused;
    
    int tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0 || tty_id >= NUM_CONSOLES) return -1;
    
    uint32_t addr = (uint32_t)termios_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -1;
    
    memcpy((void*)addr, &tty_termios[tty_id], sizeof(struct kernel_termios));
    return 0;
}

static int sys_tcsetattr(int fd, const char *actions_ptr, int termios_ptr) {
    int actions = (int)(uintptr_t)actions_ptr;
    (void)actions;
    
    int tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0 || tty_id >= NUM_CONSOLES) return -1;
    
    uint32_t addr = (uint32_t)termios_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -1;
    
    memcpy(&tty_termios[tty_id], (void*)addr, sizeof(struct kernel_termios));
    return 0;
}

static int sys_ioctl(int fd, const char *request_ptr, int arg) {
    unsigned long request = (unsigned long)(uintptr_t)request_ptr;
    
    int tty_id = get_tty_id_for_fd(fd);
    
    switch (request) {
        case TIOCGETA:
            if (tty_id < 0) return -1;
            if (!arg || (uint32_t)arg >= 0xC0000000 || (uint32_t)arg < 0x1000) return -1;
            memcpy((void*)(uintptr_t)arg, &tty_termios[tty_id], sizeof(struct kernel_termios));
            return 0;
            
        case TIOCSETA:
        case TIOCSETAW:
        case TIOCSETAF:
            if (tty_id < 0) return -1;
            if (!arg || (uint32_t)arg >= 0xC0000000 || (uint32_t)arg < 0x1000) return -1;
            memcpy(&tty_termios[tty_id], (void*)(uintptr_t)arg, sizeof(struct kernel_termios));
            return 0;
            
        case TIOCGWINSZ:
            if (tty_id < 0) return -1;
            if (!arg || (uint32_t)arg >= 0xC0000000 || (uint32_t)arg < 0x1000) return -1;
            {
                framebuffer_t *fb = fb_get();
                if (fb && fb->font) {
                    tty_winsize[tty_id].ws_col = fb->cols;
                    tty_winsize[tty_id].ws_row = fb->rows;
                    tty_winsize[tty_id].ws_xpixel = fb->width;
                    tty_winsize[tty_id].ws_ypixel = fb->height;
                }
            }
            memcpy((void*)(uintptr_t)arg, &tty_winsize[tty_id], sizeof(struct kernel_winsize));
            return 0;
            
        case TIOCSWINSZ:
            if (tty_id < 0) return -1;
            if (!arg || (uint32_t)arg >= 0xC0000000 || (uint32_t)arg < 0x1000) return -1;
            memcpy(&tty_winsize[tty_id], (void*)(uintptr_t)arg, sizeof(struct kernel_winsize));
            return 0;
            
        case TIOCGPGRP:
            if (tty_id < 0) return -1;
            if (!arg || (uint32_t)arg >= 0xC0000000 || (uint32_t)arg < 0x1000) return -1;
            *(int*)(uintptr_t)arg = tty_pgrp[tty_id];
            return 0;
            
        case TIOCSPGRP:
            if (tty_id < 0) return -1;
            tty_pgrp[tty_id] = arg;
            return 0;
            
        case FIONREAD:
            if (fd == 0 && tty_id >= 0) {
                if (!arg || (uint32_t)arg >= 0xC0000000 || (uint32_t)arg < 0x1000) return -1;
                *(int*)(uintptr_t)arg = keyboard_has_data_for(tty_id) ? 1 : 0;
                return 0;
            }
            return -1;
            
        case FIONBIO:
            return 0;
            
        default:
            return -1;
    }
}

static int sys_tcflush(int fd, const char *queue_ptr, int unused) {
    (void)unused;
    int queue = (int)(uintptr_t)queue_ptr;
    
    int tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0) return -1;
    
    (void)queue;
    return 0;
}

static int sys_tcflow(int fd, const char *action_ptr, int unused) {
    (void)unused;
    int action = (int)(uintptr_t)action_ptr;
    
    int tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0) return -1;
    
    (void)action;
    return 0;
}

static int sys_tcdrain(int fd, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    int tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0) return -1;
    
    return 0;
}

static int sys_tcgetpgrp(int fd, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    int tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0) return -1;
    
    return tty_pgrp[tty_id];
}

static int sys_tcsetpgrp(int fd, const char *pgrp_ptr, int unused) {
    (void)unused;
    int pgrp = (int)(uintptr_t)pgrp_ptr;
    
    int tty_id = get_tty_id_for_fd(fd);
    if (tty_id < 0) return -1;
    
    tty_pgrp[tty_id] = pgrp;
    return 0;
}

void syscalls_termios_init(void) {
    syscall_table[SYSCALL_TCGETATTR] = sys_tcgetattr;
    syscall_table[SYSCALL_TCSETATTR] = sys_tcsetattr;
    syscall_table[SYSCALL_IOCTL] = sys_ioctl;
    syscall_table[SYSCALL_TCFLUSH] = sys_tcflush;
    syscall_table[SYSCALL_TCFLOW] = sys_tcflow;
    syscall_table[SYSCALL_TCDRAIN] = sys_tcdrain;
    syscall_table[SYSCALL_TCGETPGRP] = sys_tcgetpgrp;
    syscall_table[SYSCALL_TCSETPGRP] = sys_tcsetpgrp;
    
    for (int i = 0; i < NUM_CONSOLES; i++) {
        termios_init_defaults(i);
    }
}
