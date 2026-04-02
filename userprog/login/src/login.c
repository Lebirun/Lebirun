#define _GNU_SOURCE
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>

#define WRITE_LIT(fd, s) write((fd), (s), sizeof(s) - 1)

#define MAX_USERNAME 64
#define MAX_PASSWORD 256
#define MAX_LINE     512
#define MAX_FIELD    256
#define MAX_ATTEMPTS 3

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

static int read_password(char *buf, int max)
{
    struct termios old_t;
    struct termios new_t;
    int r;

    tcgetattr(0, &old_t);
    new_t = old_t;
    new_t.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
    tcsetattr(0, TCSANOW, &new_t);

    r = read_line(0, buf, max);

    tcsetattr(0, TCSANOW, &old_t);
    write(1, "\n", 1);
    return r;
}

static int parse_field(const char *line, int field_num, char *out, int out_max)
{
    int i;
    int f;
    int start;
    int len;

    f = 0;
    start = 0;
    for (i = 0; line[i]; i++) {
        if (line[i] == ':') {
            if (f == field_num) {
                len = i - start;
                if (len >= out_max)
                    len = out_max - 1;
                memcpy(out, line + start, len);
                out[len] = '\0';
                return 0;
            }
            f++;
            start = i + 1;
        }
    }
    if (f == field_num) {
        len = i - start;
        if (len >= out_max)
            len = out_max - 1;
        memcpy(out, line + start, len);
        out[len] = '\0';
        return 0;
    }
    return -1;
}

static int lookup_shadow(const char *username, char *hash_out, int hash_max)
{
    int fd;
    char buf[MAX_LINE];
    char rbuf[4096];
    int pos;
    int rlen;
    int rpos;
    char c;
    char field[MAX_FIELD];

    fd = open("/etc/shadow", O_RDONLY);
    if (fd < 0)
        return -1;

    pos = 0;
    rlen = 0;
    rpos = 0;
    for (;;) {
        if (rpos >= rlen) {
            rlen = read(fd, rbuf, sizeof(rbuf));
            rpos = 0;
            if (rlen <= 0) {
                if (pos > 0) {
                    buf[pos] = '\0';
                    if (parse_field(buf, 0, field, sizeof(field)) == 0) {
                        if (strcmp(field, username) == 0) {
                            parse_field(buf, 1, hash_out, hash_max);
                            close(fd);
                            return 0;
                        }
                    }
                }
                break;
            }
        }
        c = rbuf[rpos++];
        if (c == '\n') {
            buf[pos] = '\0';
            pos = 0;
            if (parse_field(buf, 0, field, sizeof(field)) == 0) {
                if (strcmp(field, username) == 0) {
                    parse_field(buf, 1, hash_out, hash_max);
                    close(fd);
                    return 0;
                }
            }
        } else {
            if (pos < MAX_LINE - 1)
                buf[pos++] = c;
        }
    }
    close(fd);
    return -1;
}

static int lookup_passwd(const char *username, int *uid, int *gid,
                         char *home, int home_max,
                         char *shell, int shell_max)
{
    int fd;
    char buf[MAX_LINE];
    char rbuf[4096];
    int pos;
    int rlen;
    int rpos;
    char c;
    char field[MAX_FIELD];

    fd = open("/etc/passwd", O_RDONLY);
    if (fd < 0)
        return -1;

    pos = 0;
    rlen = 0;
    rpos = 0;
    for (;;) {
        if (rpos >= rlen) {
            rlen = read(fd, rbuf, sizeof(rbuf));
            rpos = 0;
            if (rlen <= 0) {
                if (pos > 0) {
                    buf[pos] = '\0';
                    if (parse_field(buf, 0, field, sizeof(field)) == 0) {
                        if (strcmp(field, username) == 0)
                            goto found;
                    }
                }
                break;
            }
        }
        c = rbuf[rpos++];
        if (c == '\n') {
            buf[pos] = '\0';
            pos = 0;
            if (parse_field(buf, 0, field, sizeof(field)) == 0) {
                if (strcmp(field, username) == 0)
                    goto found;
            }
        } else {
            if (pos < MAX_LINE - 1)
                buf[pos++] = c;
        }
    }
    close(fd);
    return -1;

found:
    parse_field(buf, 2, field, sizeof(field));
    *uid = atoi(field);
    parse_field(buf, 3, field, sizeof(field));
    *gid = atoi(field);
    parse_field(buf, 5, home, home_max);
    parse_field(buf, 6, shell, shell_max);
    close(fd);
    return 0;
}

static void show_motd(void)
{
    int fd;
    char buf[512];
    int r;

    fd = open("/etc/motd", O_RDONLY);
    if (fd < 0)
        return;
    for (;;) {
        r = read(fd, buf, sizeof(buf));
        if (r <= 0)
            break;
        write(1, buf, r);
    }
    close(fd);
}

static int check_password(const char *password, const char *stored_hash)
{
    char *result;

    if (stored_hash[0] == '!' || stored_hash[0] == '*')
        return -1;

    if (stored_hash[0] == '\0') {
        if (password[0] == '\0')
            return 0;
        return -1;
    }

    result = crypt(password, stored_hash);
    if (!result)
        return -1;
    if (strcmp(result, stored_hash) == 0)
        return 0;
    return -1;
}

int main(int argc, char **argv)
{
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
    char stored_hash[MAX_FIELD];
    char home[MAX_FIELD];
    char shell[MAX_FIELD];
    int uid;
    int gid;
    int attempts;
    char *shell_argv[2];
    char *shell_envp[4];
    char env_home[MAX_FIELD + 8];
    char env_user[MAX_USERNAME + 8];
    char env_path[32];

    if (argc >= 2) {
        strncpy(username, argv[1], MAX_USERNAME - 1);
        username[MAX_USERNAME - 1] = '\0';
    } else {
        username[0] = '\0';
    }

    for (attempts = 0; attempts < MAX_ATTEMPTS; attempts++) {
        if (username[0] == '\0') {
            WRITE_LIT(1, "login: ");
            if (read_line(0, username, sizeof(username)) < 0)
                return 1;
            if (username[0] == '\0')
                continue;
        }

        if (lookup_shadow(username, stored_hash, sizeof(stored_hash)) < 0) {
            WRITE_LIT(1, "Password: ");
            read_password(password, sizeof(password));
            sleep(2);
            WRITE_LIT(1, "Login incorrect\n\n");
            username[0] = '\0';
            continue;
        }

        if (stored_hash[0] != '\0' && stored_hash[0] != '!' && stored_hash[0] != '*') {
            WRITE_LIT(1, "Password: ");
            if (read_password(password, sizeof(password)) < 0)
                return 1;
        } else if (stored_hash[0] == '!' || stored_hash[0] == '*') {
            sleep(2);
            WRITE_LIT(1, "Login incorrect\n\n");
            username[0] = '\0';
            continue;
        } else {
            password[0] = '\0';
        }

        if (check_password(password, stored_hash) < 0) {
            sleep(2);
            WRITE_LIT(1, "Login incorrect\n\n");
            username[0] = '\0';
            continue;
        }

        if (lookup_passwd(username, &uid, &gid, home, sizeof(home), shell, sizeof(shell)) < 0) {
            WRITE_LIT(2, "login: no passwd entry\n");
            username[0] = '\0';
            continue;
        }

        if (shell[0] == '\0')
            strcpy(shell, "/bin/sh");
        if (home[0] == '\0')
            strcpy(home, "/");

        if (setgid(gid) < 0) {
            WRITE_LIT(2, "login: setgid failed\n");
            return 1;
        }
        if (setuid(uid) < 0) {
            WRITE_LIT(2, "login: setuid failed\n");
            return 1;
        }

        chdir(home);

        memset(password, 0, sizeof(password));
        memset(stored_hash, 0, sizeof(stored_hash));

        show_motd();

        strcpy(env_home, "HOME=");
        strcat(env_home, home);
        strcpy(env_user, "USER=");
        strcat(env_user, username);
        strcpy(env_path, "PATH=/bin:/sbin");

        shell_argv[0] = shell;
        shell_argv[1] = (char *)0;
        shell_envp[0] = env_home;
        shell_envp[1] = env_user;
        shell_envp[2] = env_path;
        shell_envp[3] = (char *)0;

        execve(shell, shell_argv, shell_envp);

        WRITE_LIT(2, "login: failed to exec shell\n");
        return 1;
    }

    WRITE_LIT(1, "Too many login failures\n");
    return 1;
}
