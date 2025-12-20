#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#define O_RDONLY 0
#define SHELL_PATH_MAX 256
#define MAX_BIN_SIZE (64 * 1024)

static char cwd[SHELL_PATH_MAX] = "/";

static void resolve_path(const char *path, char *out, size_t outsize) {
    if (!path || !out || outsize == 0) return;

    char tmp[SHELL_PATH_MAX * 2];
    if (path[0] == '/') {
        strncpy(tmp, path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
    } else if (strcmp(cwd, "/") == 0) {
        snprintf(tmp, sizeof(tmp), "/%s", path);
    } else {
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);
    }

    char *parts[16];
    int pc = 0;
    for (char *p = tmp; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = '\0';
        if (strcmp(start, ".") == 0) continue;
        if (strcmp(start, "..") == 0) { if (pc > 0) pc--; continue; }
        if (pc < (int)(sizeof(parts) / sizeof(parts[0]))) parts[pc++] = start;
    }

    if (pc == 0) {
        strncpy(out, "/", outsize);
        out[outsize - 1] = '\0';
        return;
    }

    size_t pos = 0;
    out[pos++] = '/';
    for (int i = 0; i < pc; i++) {
        size_t len = strlen(parts[i]);
        if (pos + len + 1 >= outsize) break;
        memcpy(out + pos, parts[i], len);
        pos += len;
        if (i + 1 < pc) out[pos++] = '/';
    }
    out[pos] = '\0';
}

static int run_binary(const char *path) {
    char resolved[SHELL_PATH_MAX];
    resolve_path(path, resolved, sizeof(resolved));

    int fd = -1;
    uint8_t *bin = 0;
    unsigned int size = 0, type = 0;

    fd = vfs_open(resolved, O_RDONLY);
    if (fd < 0) { printf("Cannot open '%s'\n", resolved); return -1; }
    if (vfs_stat(fd, &size, &type) < 0) { printf("Cannot stat '%s'\n", resolved); goto err; }
    if (type & 0x02) { printf("'%s' is a directory\n", resolved); goto err; }
    if (size == 0) { printf("'%s' is empty\n", resolved); goto err; }
    if (size > MAX_BIN_SIZE) {
        printf("'%s' is too large (%u bytes, max %d)\n", resolved, size, MAX_BIN_SIZE);
        goto err;
    }

    bin = (uint8_t *)sbrk((int)size);
    if (!bin || bin == (uint8_t *)-1) { printf("Failed to allocate %u bytes\n", size); goto err; }

    int rd = vfs_read_fd(fd, bin, size);
    if (rd <= 0) { printf("Failed to read '%s'\n", resolved); goto err; }
    vfs_close_fd(fd);
    fd = -1;

    if (rd < 4 || bin[0] != 0x7F || bin[1] != 'E' || bin[2] != 'L' || bin[3] != 'F') {
        printf("'%s' is not a valid ELF binary\n", resolved);
        return -1;
    }

    int pid = fork();
    if (pid < 0) { printf("Fork failed\n"); return -1; }
    if (pid == 0) {
        int ret = exec(bin, (unsigned int)rd);
        printf("exec failed: %d\n", ret);
        exit(1);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return status;

err:
    if (fd >= 0) vfs_close_fd(fd);
    return -1;
}

static inline int is_executable_path(const char *cmd) {
    return cmd && *cmd && (cmd[0] == '/' || (cmd[0] == '.' && cmd[1] == '/'));
}

static int read_line(char *buf, size_t cap) {
    size_t pos = 0;
    if (!buf || cap == 0) return 0;
    while (1) {
        int in = getchar();
        if (in < 0) continue;
        char c = (char)in;
        if (c == '\n' || c == '\r') {
            putchar('\n');
            buf[pos] = '\0';
            return 1;
        }
        if (c == '\b' || c == 127) {
            if (pos) { pos--; putchar('\b'); putchar(' '); putchar('\b'); }
            continue;
        }
        if (c >= 32 && pos + 1 < cap) { buf[pos++] = c; putchar(c); }
    }
}

void help() {
    printf("Available commands (Shell-builtin):\n");
    printf("help\n");
    printf("echo\n");
    printf("ticks\n");
}

void echo(char line[128]) {
    char *p = &line[4];
    while (*p == ' ') p++;
    if (*p == '\0') putchar('\n');
    else puts(p);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char line[128];

    while (1) {
        printf("leb:%s> ", cwd);
        if (!read_line(line, sizeof(line))) continue;
        if (line[0] == '\0') continue;
        if (strcmp(line, "help") == 0) {
            help();
        } else if (strncmp(line, "echo", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
            echo(line);
        } else if (strcmp(line, "ticks") == 0) {
            printf("Ticks: %u\n", getticks());
        } else if (is_executable_path(line)) {
            run_binary(line);
        } else {
            printf("Unknown command: %s\nType 'help' for available commands.\n", line);
        }
    }
}