#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <graphics.h>

#define O_RDONLY 0
#define SHELL_PATH_MAX 256
#define MAX_BIN_SIZE (64 * 1024)

static char cwd[SHELL_PATH_MAX] = "/";

static void resolve_path(const char *path, char *out, size_t outsize) {
    if (!path || !out || outsize == 0) return;

    char temp[SHELL_PATH_MAX * 2];
    if (path[0] == '/') {
        strncpy(temp, path, sizeof(temp)-1);
        temp[sizeof(temp)-1] = '\0';
    } else {
        const char *rel = path;
        if (rel[0] == '.' && rel[1] == '/') {
            rel += 2;
        }
        if (strcmp(cwd, "/") == 0) {
            snprintf(temp, sizeof(temp), "/%s", rel);
        } else {
            snprintf(temp, sizeof(temp), "%s/%s", cwd, rel);
        }
    }

    char comps[16][SHELL_PATH_MAX];
    int compc = 0;
    const char *p = temp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[SHELL_PATH_MAX];
        int i = 0;
        while (*p && *p != '/' && i < (int)sizeof(comp)-1) comp[i++] = *p++;
        comp[i] = '\0';
        if (strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) {
            if (compc > 0) compc--;
            continue;
        }
        if (compc < 16) {
            strncpy(comps[compc], comp, SHELL_PATH_MAX-1);
            comps[compc][SHELL_PATH_MAX-1] = '\0';
            compc++;
        }
    }

    if (compc == 0) {
        strncpy(out, "/", outsize);
        out[outsize-1] = '\0';
        return;
    }

    size_t pos = 0;
    out[pos++] = '/';
    for (int i = 0; i < compc; i++) {
        size_t need = strlen(comps[i]);
        if (pos + need + 1 >= outsize) break;
        memcpy(out + pos, comps[i], need);
        pos += need;
        if (i + 1 < compc) out[pos++] = '/';
    }
    out[pos] = '\0';
}

static int run_binary(const char *path) {
    char resolved[SHELL_PATH_MAX];
    resolve_path(path, resolved, sizeof(resolved));

    int fd = vfs_open(resolved, O_RDONLY);
    if (fd < 0) {
        printf("Cannot open '%s'\n", resolved);
        return -1;
    }

    unsigned int size = 0, type = 0;
    if (vfs_stat(fd, &size, &type) < 0) {
        printf("Cannot stat '%s'\n", resolved);
        vfs_close_fd(fd);
        return -1;
    }

    if (type & 0x02) {
        printf("'%s' is a directory\n", resolved);
        vfs_close_fd(fd);
        return -1;
    }

    if (size == 0) {
        printf("'%s' is empty\n", resolved);
        vfs_close_fd(fd);
        return -1;
    }

    if (size > MAX_BIN_SIZE) {
        printf("'%s' is too large (%u bytes, max %d)\n", resolved, size, MAX_BIN_SIZE);
        vfs_close_fd(fd);
        return -1;
    }

    uint8_t *bin = (uint8_t *)sbrk((int)size);
    if (!bin || bin == (uint8_t *)-1) {
        printf("Failed to allocate %u bytes for binary\n", size);
        vfs_close_fd(fd);
        return -1;
    }

    int rd = vfs_read_fd(fd, bin, size);
    vfs_close_fd(fd);

    if (rd <= 0) {
        printf("Failed to read '%s'\n", resolved);
        return -1;
    }

    if (size < 4 || bin[0] != 0x7F || bin[1] != 'E' || bin[2] != 'L' || bin[3] != 'F') {
        printf("'%s' is not a valid ELF binary\n", resolved);
        return -1;
    }

    int pid = fork();
    if (pid < 0) {
        printf("Fork failed\n");
        return -1;
    } else if (pid == 0) {
        int ret = exec(bin, (unsigned int)rd);
        printf("exec failed: %d\n", ret);
        exit(1);
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        return status;
    }
}

static int is_executable_path(const char *cmd) {
    if (!cmd || !*cmd) return 0;
    if (cmd[0] == '/') return 1;
    if (cmd[0] == '.' && cmd[1] == '/') return 1;
    return 0;
}

void help() {
    printf("Available commands (Shell-builtin):\n");
    printf("help\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    
    char line[128];
    int pos = 0;

    while(1) {
        printf("leb:%s> ", cwd);
        pos = 0;
        while (1) {
            int in = getchar();
            if (in < 0) {
                continue;
            }
            char c = (char)in;
            if (c == '\n' || c == '\r') {
                putchar('\n');
                line[pos] = '\0';
                break;
            } else if (c == '\b' || c == 127) {
                if (pos > 0) {
                    pos--;
                    putchar('\b');
                    putchar(' ');
                    putchar('\b');
                }
            } else if (c >= 32 && pos <= 62) {
                line[pos++] = c;
                putchar(c);
            }
        }

        if (line[0] == '\0') {
        } else if (strcmp(line, "help") == 0) {
            help();
        } else if (is_executable_path(line)) {
            run_binary(line);
        } else {
            printf("Unknown command: %s\n", line);
            printf("Type 'help' for available commands.\n");
        }
    }
}