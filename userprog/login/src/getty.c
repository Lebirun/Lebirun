#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <errno.h>
#include <lebirun.h>

#define WRITE_LIT(fd, s) write((fd), (s), sizeof(s) - 1)
#define MAX_USERNAME 64

static int read_line(int fd, char *buf, int max)
{
    int i;
    char c;
    int r;

    i = 0;
    while (i < max - 1) {
        r = read(fd, &c, 1);
        if (r <= 0)
            return -1;
        if (c == '\n' || c == '\r')
            break;
        if (c == 127 || c == '\b') {
            if (i > 0)
                i--;
            continue;
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

static void write_tty_num(int n)
{
    char buf[4];
    int i;

    i = 0;
    if (n >= 10)
        buf[i++] = '0' + (n / 10);
    buf[i++] = '0' + (n % 10);
    write(1, buf, i);
}

int main(int argc, char **argv)
{
    int console_num;
    char username[MAX_USERNAME];
    char *login_argv[3];
    char *login_envp[2];
    int r;
    int exec_attempts;
    struct utsname uts;

    if (argc < 2) {
        WRITE_LIT(2, "Usage: getty <console_num>\n");
        return 1;
    }

    console_num = atoi(argv[1]);

    setsid();

    if (console_setid(console_num) < 0) {
        WRITE_LIT(2, "getty: console_setid failed\n");
        return 1;
    }

    ioctl(0, TIOCSCTTY, 0);
    tcsetpgrp(0, getpid());

    if (uname(&uts) < 0) {
        strcpy(uts.sysname, "lebirun");
        strcpy(uts.nodename, "localhost");
    }

    if (console_num != 0)
        WRITE_LIT(1, "\033[2J\033[H");

    for (;;) {
        write(1, "\n", 1);

        write(1, uts.sysname, strlen(uts.sysname));
        WRITE_LIT(1, " | ");
        write(1, uts.nodename, strlen(uts.nodename));
        WRITE_LIT(1, " | tty");
        write_tty_num(console_num);
        WRITE_LIT(1, "\n\nlogin: ");

        r = read_line(0, username, sizeof(username));
        if (r < 0)
            continue;
        if (username[0] == '\0')
            continue;

        login_argv[0] = "/bin/login";
        login_argv[1] = username;
        login_argv[2] = (char *)0;
        login_envp[0] = "TERM=linux";
        login_envp[1] = (char *)0;

        for (exec_attempts = 0; exec_attempts < 5; exec_attempts++) {
            execve("/bin/login", login_argv, login_envp);
            if (errno != ENOMEM)
                break;
            sleep(1);
        }

        WRITE_LIT(2, "getty: failed to exec login\n");
        return 1;
    }
}
