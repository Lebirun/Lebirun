#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <syscall.h>

#define O_RDONLY 0
#define SHELL_PATH_MAX 256
#define MAX_ENV_VARS 64
#define MAX_ENV_KEY 64
#define MAX_ENV_VAL 256

static char cwd[SHELL_PATH_MAX] = "/";

static struct {
    char key[MAX_ENV_KEY];
    char value[MAX_ENV_VAL];
    int used;
} env_vars[MAX_ENV_VARS];

static int env_count = 0;

static void init_env(void) {
    strcpy(env_vars[0].key, "PATH");
    strcpy(env_vars[0].value, "/bin:/usr/bin:/sbin:/usr/sbin");
    env_vars[0].used = 1;
    
    strcpy(env_vars[1].key, "HOME");
    strcpy(env_vars[1].value, "/home");
    env_vars[1].used = 1;
    
    strcpy(env_vars[2].key, "SHELL");
    strcpy(env_vars[2].value, "/bin/sh");
    env_vars[2].used = 1;
    
    strcpy(env_vars[3].key, "USER");
    strcpy(env_vars[3].value, "root");
    env_vars[3].used = 1;
    
    strcpy(env_vars[4].key, "TERM");
    strcpy(env_vars[4].value, "vt100");
    env_vars[4].used = 1;
    
    strcpy(env_vars[5].key, "PWD");
    strcpy(env_vars[5].value, "/");
    env_vars[5].used = 1;
    
    env_count = 6;
}

static const char *shell_getenv(const char *key) {
    if (!key) return NULL;
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used && strcmp(env_vars[i].key, key) == 0) {
            return env_vars[i].value;
        }
    }
    return NULL;
}

static int shell_setenv(const char *key, const char *value) {
    if (!key || !value) return -1;
    if (strlen(key) >= MAX_ENV_KEY || strlen(value) >= MAX_ENV_VAL) return -1;
    
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used && strcmp(env_vars[i].key, key) == 0) {
            strncpy(env_vars[i].value, value, MAX_ENV_VAL - 1);
            env_vars[i].value[MAX_ENV_VAL - 1] = '\0';
            return 0;
        }
    }
    
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (!env_vars[i].used) {
            strncpy(env_vars[i].key, key, MAX_ENV_KEY - 1);
            env_vars[i].key[MAX_ENV_KEY - 1] = '\0';
            strncpy(env_vars[i].value, value, MAX_ENV_VAL - 1);
            env_vars[i].value[MAX_ENV_VAL - 1] = '\0';
            env_vars[i].used = 1;
            env_count++;
            return 0;
        }
    }
    return -1;
}

static int shell_unsetenv(const char *key) {
    if (!key) return -1;
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used && strcmp(env_vars[i].key, key) == 0) {
            env_vars[i].used = 0;
            env_vars[i].key[0] = '\0';
            env_vars[i].value[0] = '\0';
            env_count--;
            return 0;
        }
    }
    return -1;
}

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

static int file_exists(const char *path) {
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned int size = 0, type = 0;
    if (vfs_stat(fd, &size, &type) < 0) {
        vfs_close_fd(fd);
        return 0;
    }
    vfs_close_fd(fd);
    return (type & 0x02) == 0;
}

static int search_path(const char *cmd, char *resolved, size_t res_size) {
    const char *path = shell_getenv("PATH");
    if (!path) path = "/bin:/usr/bin";
    
    char pathcopy[MAX_ENV_VAL];
    strncpy(pathcopy, path, sizeof(pathcopy) - 1);
    pathcopy[sizeof(pathcopy) - 1] = '\0';
    
    char *dir = pathcopy;
    while (dir && *dir) {
        char *colon = dir;
        while (*colon && *colon != ':') colon++;
        
        char saved = *colon;
        *colon = '\0';
        
        if (dir[0] != '\0') {
            char trypath[SHELL_PATH_MAX];
            if (dir[strlen(dir) - 1] == '/') {
                snprintf(trypath, sizeof(trypath), "%s%s", dir, cmd);
            } else {
                snprintf(trypath, sizeof(trypath), "%s/%s", dir, cmd);
            }
            
            if (file_exists(trypath)) {
                strncpy(resolved, trypath, res_size - 1);
                resolved[res_size - 1] = '\0';
                return 1;
            }
        }
        
        if (saved == '\0') break;
        dir = colon + 1;
    }
    
    return 0;
}

static int run_binary(const char *cmdline) {
    char cmd[SHELL_PATH_MAX];
    const char *p = cmdline;
    int i = 0;
    while (*p && *p != ' ' && i < SHELL_PATH_MAX - 1) {
        cmd[i++] = *p++;
    }
    cmd[i] = '\0';
    
    char resolved[SHELL_PATH_MAX];
    
    if (cmd[0] == '/' || (cmd[0] == '.' && cmd[1] == '/')) {
        resolve_path(cmd, resolved, sizeof(resolved));
    } else {
        if (!search_path(cmd, resolved, sizeof(resolved))) {
            printf("%s: command not found\n", cmd);
            return -1;
        }
    }

    int fd = -1;
    uint8_t *bin = 0;
    unsigned int size = 0, type = 0;

    fd = vfs_open(resolved, O_RDONLY);
    if (fd < 0) { printf("Cannot open '%s'\n", resolved); return -1; }
    if (vfs_stat(fd, &size, &type) < 0) { printf("Cannot stat '%s'\n", resolved); goto err; }
    if (type & 0x02) { printf("'%s' is a directory\n", resolved); goto err; }
    if (size == 0) { printf("'%s' is empty\n", resolved); goto err; }

    bin = (uint8_t *)sbrk((int)size);
    if (!bin || bin == (uint8_t *)-1) { printf("Failed to allocate %u bytes\n", size); goto err; }

    int rd = vfs_read_fd(fd, bin, size);
    if (rd <= 0) { printf("Failed to read '%s'\n", resolved); goto err; }
    vfs_close_fd(fd);
    fd = -1;

    if (rd < 4 || bin[0] != 0x7F || bin[1] != 'E' || bin[2] != 'L' || bin[3] != 'F') {
        printf("'%s' is not a valid ELF binary (read %d bytes, first 4: %02X %02X %02X %02X)\n", 
               resolved, rd, bin[0], bin[1], bin[2], bin[3]);
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
    return cmd && *cmd && cmd[0] != '\0';
}

static int read_line(char *buf, size_t cap) {
    size_t pos = 0;
    if (!buf || cap == 0) return 0;
    bool overflow = false;
    while (1) {
        int in = getchar();
        if (in < 0) continue;
        char c = (char)in;
        if (c == 0x03) {
            printf("^C\n");
            buf[0] = '\0';
            return 0;
        }
        if (c == '\n' || c == '\r') {
            putchar('\n');
            buf[pos] = '\0';
            if (overflow) printf("Input truncated to %zu characters\n", cap - 1);
            return 1;
        }
        if (c == '\b' || c == 127) {
            if (pos) { pos--; putchar('\b'); putchar(' '); putchar('\b'); fflush(stdout); }
            continue;
        }
        if (c >= 32) {
            if (pos + 1 < cap) { buf[pos++] = c; putchar(c); fflush(stdout); }
            else { overflow = true; putchar('\a'); fflush(stdout); }
        }
    }
}

void help() {
    printf("Available commands (Shell-builtin):\n");
    printf("  help              - Show this help\n");
    printf("  echo [text]       - Print text (supports $VAR expansion)\n");
    printf("  ticks             - Show system ticks\n");
    printf("  cd [path]         - Change directory\n");
    printf("  ls [path]         - List directory contents\n");
    printf("  cat <path>        - Display file contents\n");
    printf("  pwd               - Print working directory\n");
    printf("  touch <path>      - Create empty file\n");
    printf("  mkdir <path>      - Create directory\n");
    printf("  rm <path>         - Remove file\n");
    printf("  write <path> <text> - Write text to file\n");
    printf("  export VAR=value  - Set environment variable\n");
    printf("  unset VAR         - Unset environment variable\n");
    printf("  env               - List environment variables\n");
    printf("  set               - Same as env\n");
    printf("\nExternal commands are searched in PATH: %s\n", 
           shell_getenv("PATH") ? shell_getenv("PATH") : "(not set)");
}

static void expand_vars(const char *input, char *output, size_t outsize) {
    size_t oi = 0;
    const char *p = input;
    
    while (*p && oi < outsize - 1) {
        if (*p == '$') {
            p++;
            if (*p == '{') {
                p++;
                char varname[MAX_ENV_KEY];
                int vi = 0;
                while (*p && *p != '}' && vi < MAX_ENV_KEY - 1) {
                    varname[vi++] = *p++;
                }
                varname[vi] = '\0';
                if (*p == '}') p++;
                
                const char *val = shell_getenv(varname);
                if (val) {
                    while (*val && oi < outsize - 1) {
                        output[oi++] = *val++;
                    }
                }
            } else if (*p == '?') {
                p++;
                output[oi++] = '0';
            } else {
                char varname[MAX_ENV_KEY];
                int vi = 0;
                while (*p && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || 
                       (*p >= '0' && *p <= '9') || *p == '_') && vi < MAX_ENV_KEY - 1) {
                    varname[vi++] = *p++;
                }
                varname[vi] = '\0';
                
                if (vi > 0) {
                    const char *val = shell_getenv(varname);
                    if (val) {
                        while (*val && oi < outsize - 1) {
                            output[oi++] = *val++;
                        }
                    }
                } else {
                    output[oi++] = '$';
                }
            }
        } else {
            output[oi++] = *p++;
        }
    }
    output[oi] = '\0';
}

void echo(const char *line) {
    if (!line) { putchar('\n'); return; }
    const char *p = line + 4;
    while (*p == ' ') p++;
    if (*p == '\0') { putchar('\n'); return; }
    
    char expanded[512];
    expand_vars(p, expanded, sizeof(expanded));
    puts(expanded);
}

void ls(const char *arg) {
    char path[SHELL_PATH_MAX];
    if (arg && *arg) {
        resolve_path(arg, path, sizeof(path));
    } else {
        strncpy(path, cwd, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        printf("ls: cannot access '%s'\n", path);
        return;
    }

    unsigned int size = 0, type = 0;
    if (vfs_stat(fd, &size, &type) == 0) {
        if ((type & 0x02) == 0 && (type & 0x08) == 0) {
            printf("ls: '%s' is not a directory\n", path);
            vfs_close_fd(fd);
            return;
        }
    }

    printf("Contents of %s:\n", path);
    char name[64];
    unsigned int entry_type = 0;
    for (unsigned int i = 0; i < 100; i++) {
        if (vfs_readdir(fd, name, &entry_type, i) < 0) break;
        const char *type_str = (entry_type == 2 || entry_type == 8) ? "DIR " : "FILE";
        printf("  [%s] %s\n", type_str, name);
    }
    vfs_close_fd(fd);
}

void cd(const char *path) {
    char resolved[SHELL_PATH_MAX];
    if (!path || *path == '\0') {
        const char *home = shell_getenv("HOME");
        if (home && *home) {
            strncpy(resolved, home, sizeof(resolved) - 1);
            resolved[sizeof(resolved) - 1] = '\0';
        } else {
            strncpy(cwd, "/", sizeof(cwd));
            cwd[sizeof(cwd) - 1] = '\0';
            shell_setenv("PWD", cwd);
            return;
        }
    } else {
        resolve_path(path, resolved, sizeof(resolved));
    }

    int fd = vfs_open(resolved, O_RDONLY);
    if (fd < 0) {
        printf("cd: cannot access '%s'\n", resolved);
        return;
    }
    unsigned int size = 0, type = 0;
    if (vfs_stat(fd, &size, &type) < 0 || ((type & 0x02) == 0 && (type & 0x08) == 0)) {
        printf("cd: '%s' is not a directory\n", resolved);
        vfs_close_fd(fd);
        return;
    }
    vfs_close_fd(fd);
    strncpy(cwd, resolved, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = '\0';
    
    shell_setenv("PWD", cwd);
}

void cat(const char *arg) {
    char path[SHELL_PATH_MAX];
    if (!arg || *arg == '\0') {
        printf("cat: missing operand\n");
        return;
    }
    resolve_path(arg, path, sizeof(path));

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) { printf("cat: cannot open '%s'\n", path); return; }

    unsigned int size = 0, type = 0;
    if (vfs_stat(fd, &size, &type) < 0) { printf("cat: cannot stat '%s'\n", path); vfs_close_fd(fd); return; }
    if (type & 0x02) { printf("cat: '%s' is a directory\n", path); vfs_close_fd(fd); return; }

    char buf[256];
    int rd;
    while ((rd = vfs_read_fd(fd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, rd);
    }
    if (rd < 0) printf("cat: read error on '%s'\n", path);
    else putchar('\n');
    vfs_close_fd(fd);
}

void pwd() {
    printf("%s\n", cwd);
}

void touch(const char *arg) {
    if (!arg || *arg == '\0') {
        printf("touch: missing operand\n");
        return;
    }
    char path[SHELL_PATH_MAX];
    resolve_path(arg, path, sizeof(path));
    int ret = vfs_create(path, 0x06);
    if (ret < 0) {
        printf("touch: cannot create '%s'\n", path);
    } else {
        printf("Created '%s'\n", path);
    }
}

void cmd_mkdir(const char *arg) {
    if (!arg || *arg == '\0') {
        printf("mkdir: missing operand\n");
        return;
    }
    char path[SHELL_PATH_MAX];
    resolve_path(arg, path, sizeof(path));
    int ret = vfs_mkdir(path, 0x07);
    if (ret < 0) {
        printf("mkdir: cannot create directory '%s'\n", path);
    } else {
        printf("Created directory '%s'\n", path);
    }
}

void rm(const char *arg) {
    if (!arg || *arg == '\0') {
        printf("rm: missing operand\n");
        return;
    }
    char path[SHELL_PATH_MAX];
    resolve_path(arg, path, sizeof(path));
    int ret = vfs_unlink(path);
    if (ret < 0) {
        printf("rm: cannot remove '%s'\n", path);
    } else {
        printf("Removed '%s'\n", path);
    }
}

void writer(const char *arg) {
    if (!arg || *arg == '\0') {
        printf("write: usage: write <path> <text>\n");
        return;
    }
    const char *space = arg;
    while (*space && *space != ' ') space++;
    if (*space != ' ') { printf("write: usage: write <path> <text>\n"); return; }
    int pathlen = (int)(space - arg);
    if (pathlen <= 0 || pathlen >= SHELL_PATH_MAX) { printf("write: invalid path\n"); return; }
    char rawpath[SHELL_PATH_MAX];
    for (int i = 0; i < pathlen; i++) rawpath[i] = arg[i];
    rawpath[pathlen] = '\0';

    char path[SHELL_PATH_MAX];
    resolve_path(rawpath, path, sizeof(path));

    const char *text = space + 1;
    int textlen = 0;
    while (text[textlen]) textlen++;

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        int ret = vfs_create(path, 0x06);
        if (ret < 0) { printf("write: cannot create '%s'\n", path); return; }
        fd = vfs_open(path, O_RDONLY);
        if (fd < 0) { printf("write: cannot open '%s' after create\n", path); return; }
    }

    int written = vfs_write_fd(fd, text, (unsigned int)textlen);
    vfs_close_fd(fd);

    if (written < 0) {
        printf("write: failed to write to '%s'\n", path);
    } else {
        printf("Wrote %d bytes to '%s'\n", written, path);
    }
}

void cmd_export(const char *arg) {
    if (!arg || *arg == '\0') {
        for (int i = 0; i < MAX_ENV_VARS; i++) {
            if (env_vars[i].used) {
                printf("export %s=\"%s\"\n", env_vars[i].key, env_vars[i].value);
            }
        }
        return;
    }
    
    const char *eq = arg;
    while (*eq && *eq != '=') eq++;
    
    if (*eq != '=') {
        const char *val = shell_getenv(arg);
        if (val) {
            printf("%s=%s\n", arg, val);
        } else {
            printf("%s: not set\n", arg);
        }
        return;
    }
    
    char key[MAX_ENV_KEY];
    int keylen = (int)(eq - arg);
    if (keylen <= 0 || keylen >= MAX_ENV_KEY) {
        printf("export: invalid variable name\n");
        return;
    }
    
    for (int i = 0; i < keylen; i++) key[i] = arg[i];
    key[keylen] = '\0';
    
    const char *val = eq + 1;
    
    if (shell_setenv(key, val) < 0) {
        printf("export: failed to set %s\n", key);
    }
}

void cmd_unset(const char *arg) {
    if (!arg || *arg == '\0') {
        printf("unset: missing variable name\n");
        return;
    }
    
    if (shell_unsetenv(arg) < 0) {
        printf("unset: %s: not set\n", arg);
    }
}

void cmd_env(void) {
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used) {
            printf("%s=%s\n", env_vars[i].key, env_vars[i].value);
        }
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char line[128];

    init_env();

    while (1) {
        printf("leb:%s> ", cwd);
        fflush(stdout);
        if (!read_line(line, sizeof(line))) continue;
        if (line[0] == '\0') continue;
        if (strcmp(line, "help") == 0) {
            help();
        } else if (strncmp(line, "echo", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
            echo(line);
        } else if (strcmp(line, "ticks") == 0) {
            printf("Ticks: %u\n", getticks());
        } else if (strncmp(line, "cd", 2) == 0 && (line[2] == '\0' || line[2] == ' ')) {
            const char *arg = line + 2;
            while (*arg == ' ') arg++;
            cd((*arg == '\0') ? NULL : arg);
        } else if (strncmp(line, "ls", 2) == 0 && (line[2] == '\0' || line[2] == ' ')) {
            const char *arg = line + 2;
            while (*arg == ' ') arg++;
            ls((*arg == '\0') ? NULL : arg);
        } else if (strncmp(line, "cat", 3) == 0 && (line[3] == '\0' || line[3] == ' ')) {
            const char *arg = line + 3;
            while (*arg == ' ') arg++;
            cat((*arg == '\0') ? NULL : arg);
        } else if (strcmp(line, "pwd") == 0) {
            pwd();
        } else if (strncmp(line, "touch", 5) == 0 && (line[5] == '\0' || line[5] == ' ')) {
            const char *arg = line + 5;
            while (*arg == ' ') arg++;
            touch((*arg == '\0') ? NULL : arg);
        } else if (strncmp(line, "mkdir", 5) == 0 && (line[5] == '\0' || line[5] == ' ')) {
            const char *arg = line + 5;
            while (*arg == ' ') arg++;
            cmd_mkdir((*arg == '\0') ? NULL : arg);
        } else if (strncmp(line, "rm", 2) == 0 && (line[2] == '\0' || line[2] == ' ')) {
            const char *arg = line + 2;
            while (*arg == ' ') arg++;
            rm((*arg == '\0') ? NULL : arg);
        } else if (strncmp(line, "write", 5) == 0 && (line[5] == '\0' || line[5] == ' ')) {
            const char *arg = line + 5;
            while (*arg == ' ') arg++;
            writer((*arg == '\0') ? NULL : arg);
        } else if (strncmp(line, "export", 6) == 0 && (line[6] == '\0' || line[6] == ' ')) {
            const char *arg = line + 6;
            while (*arg == ' ') arg++;
            cmd_export((*arg == '\0') ? NULL : arg);
        } else if (strncmp(line, "unset", 5) == 0 && (line[5] == '\0' || line[5] == ' ')) {
            const char *arg = line + 5;
            while (*arg == ' ') arg++;
            cmd_unset((*arg == '\0') ? NULL : arg);
        } else if (strcmp(line, "env") == 0 || strcmp(line, "set") == 0) {
            cmd_env();
        } else if (is_executable_path(line)) {
            run_binary(line);
        } else {
            printf("Unknown command: %s\nType 'help' for available commands.\n", line);
        }
    }
}