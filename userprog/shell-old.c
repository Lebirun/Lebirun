#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <graphics.h>

#define O_RDONLY 0
#define SHELL_PATH_MAX 256

static char cwd[SHELL_PATH_MAX] = "/";

static void test_fork(void) {
    printf("About to fork...\n");
    int pid = fork();
    if (pid < 0) {
        printf("Fork failed!\n");
    } else if (pid == 0) {
        printf("Child process! PID=%d\n", getpid());
        printf("Child exiting.\n");
        exit(42);
    } else {
        printf("Parent process! PID=%d, child PID=%d\n", getpid(), pid);
        int status = 0;
        printf("Parent waiting for child...\n");
        int ret = waitpid(pid, &status, 0);
        printf("waitpid returned %d, status=%d\n", ret, status);
    }
}

static void resolve_path(const char *path, char *out, size_t outsize) {
    if (!path || !out || outsize == 0) return;

    char temp[SHELL_PATH_MAX * 2];
    if (path[0] == '/') {
        strncpy(temp, path, sizeof(temp)-1);
        temp[sizeof(temp)-1] = '\0';
    } else {
        if (strcmp(cwd, "/") == 0) {
            snprintf(temp, sizeof(temp), "/%s", path);
        } else {
            snprintf(temp, sizeof(temp), "%s/%s", cwd, path);
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

static void cmd_initrd_ls(void) {
    int count = initrd_count();
    if (count <= 0) {
        puts("No files in initrd (or initrd not loaded)");
        return;
    }
    printf("Initrd files (%d):\n", count);
    for (int i = 0; i < count; i++) {
        char name[64];
        unsigned int len = 0;
        if (initrd_stat(i, name, &len) == 0) {
            printf("  [%d] %s (%u bytes)\n", i, name, len);
        }
    }
}

static void cmd_initrd_cat(const char *arg) {
    if (!arg || *arg == '\0') {
        puts("Usage: initrd cat <index|name>");
        return;
    }

    int index = -1;
    int all_digits = 1;
    for (const char *p = arg; *p; p++) {
        if (*p < '0' || *p > '9') { all_digits = 0; break; }
    }

    char name[64];
    unsigned int len = 0;

    if (all_digits) {
        int idx = 0;
        const char *p = arg;
        while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
        index = idx;
    } else {
        int count = initrd_count();
        if (count <= 0) {
            puts("No files in initrd (or initrd not loaded)");
            return;
        }
        int found = -1;
        for (int i = 0; i < count; i++) {
            if (initrd_stat(i, name, &len) == 0) {
                if (strcmp(name, arg) == 0) { found = i; break; }
            }
        }
        if (found < 0) {
            printf("File %s not found\n", arg);
            return;
        }
        index = found;
    }

    if (initrd_stat(index, name, &len) != 0) {
        printf("File index %d not found\n", index);
        return;
    }
    
    if (len > 4096) {
        printf("File too large (%u bytes), showing first 4096\n", len);
        len = 4096;
    }
    
    static char buf[4097];
    int rd = initrd_read(index, buf, len);
    if (rd > 0) {
        buf[rd] = '\0';
        puts(buf);
    } else {
        puts("Failed to read file");
    }
}

static void cmd_colors(void) {
    unsigned int width, height, bpp, font_h, rows, cursor_row;
    if (fb_getinfo(&width, &height, &bpp, &font_h, &rows, &cursor_row) != 0) {
        puts("Failed to get framebuffer info");
        return;
    }
    printf("Framebuffer: %ux%u @ %u bpp, font_h=%u, rows=%u, cursor_row=%u\n", width, height, bpp, font_h, rows, cursor_row);
    puts("Drawing colorful graphics demo...\n");

    gfx_demo();

    fb_setcolors(0x00FF00, 0x000000);
    puts("Green text!");
    fb_setcolors(0xFF00FF, 0x000000);
    puts("Magenta text!");
    fb_setcolors(0x00FFFF, 0x000000);
    puts("Cyan text!");
    fb_setcolors(0xFFFF00, 0x000000);
    puts("Yellow text!");
    fb_setcolors(0xFF8800, 0x000000);
    puts("Orange text!");
    fb_setcolors(0xAAAAAA, 0x000000);
    puts("\nColors demo complete! (default colors restored)");
}

static void cmd_console(const char *arg) {
    if (!arg || *arg == '\0') {
        int cur = console_getcur();
        printf("Current console: %d (F%d)\n", cur, cur + 1);
        puts("Usage: console <number>  (0-8, where 0=F1, 8=F9)");
        puts("You can also press Ctrl+Alt+F1-F9 to switch consoles.");
        return;
    }

    int console_num = 0;
    const char *p = arg;
    while (*p >= '0' && *p <= '9') {
        console_num = console_num * 10 + (*p - '0');
        p++;
    }

    if (console_num < 0 || console_num > 8) {
        printf("Invalid console number: %d (must be 0-8)\n", console_num);
        return;
    }

    printf("Switching to console %d (F%d)...\n", console_num, console_num + 1);
    console_switch(console_num);
}

static void cmd_vfs_mounts(void) {
    int count = vfs_mounts();
    printf("Total mounts: %d\n", count);
}

static void cmd_vfs_ls(const char *arg) {
    char path[SHELL_PATH_MAX];
    if (arg && *arg) resolve_path(arg, path, sizeof(path)); else strncpy(path, cwd, sizeof(path) - 1);

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        printf("Cannot open '%s'\n", path);
        return;
    }
    
    unsigned int size = 0, type = 0;
    if (vfs_stat(fd, &size, &type) == 0) {
        if ((type & 0x02) == 0 && (type & 0x08) == 0) {
            printf("'%s' is not a directory (type=%u)\n", path, type);
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

static void cmd_vfs_cat(const char *arg) {
    if (!arg || *arg == '\0') {
        puts("Usage: vcat <path>");
        return;
    }
    
    char path[SHELL_PATH_MAX];
    resolve_path(arg, path, sizeof(path));
    
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        printf("Cannot open '%s'\n", path);
        return;
    }
    
    unsigned int size = 0, type = 0;
    if (vfs_stat(fd, &size, &type) < 0) {
        printf("Cannot stat '%s'\n", path);
        vfs_close_fd(fd);
        return;
    }
    
    if (type & 0x02) {
        printf("'%s' is a directory\n", path);
        vfs_close_fd(fd);
        return;
    }
    
    if (size > 4096) {
        printf("File too large (%u bytes), showing first 4096\n", size);
        size = 4096;
    }
    
    static char buf[4097];
    int rd = vfs_read_fd(fd, buf, size);
    if (rd > 0) {
        buf[rd] = '\0';
        puts(buf);
    } else {
        puts("Failed to read file");
    }
    vfs_close_fd(fd);
}

static void cmd_cd(const char *arg) {
    char path[SHELL_PATH_MAX];
    if (!arg || !*arg) {
        strncpy(cwd, "/", sizeof(cwd));
        return;
    }
    resolve_path(arg, path, sizeof(path));
    
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        printf("cd: cannot access '%s'\n", path);
        return;
    }
    unsigned int size = 0, type = 0;
    if (vfs_stat(fd, &size, &type) < 0 || ((type & 0x02) == 0 && (type & 0x08) == 0)) {
        printf("cd: '%s' is not a directory\n", path);
        vfs_close_fd(fd);
        return;
    }
    vfs_close_fd(fd);
    strncpy(cwd, path, sizeof(cwd)-1);
    cwd[sizeof(cwd)-1] = '\0';
}

static void cmd_pwd(void) {
    printf("%s\n", cwd);
}

static void cmd_touch(const char *arg) {
    if (!arg || *arg == '\0') {
        puts("Usage: touch <path>");
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

static void cmd_mkdir_shell(const char *arg) {
    if (!arg || *arg == '\0') {
        puts("Usage: mkdir <path>");
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

static void cmd_rm(const char *arg) {
    if (!arg || *arg == '\0') {
        puts("Usage: rm <path>");
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

static void cmd_write_file(const char *arg) {
    if (!arg || *arg == '\0') {
        puts("Usage: write <path> <text>");
        return;
    }
    const char *space = arg;
    while (*space && *space != ' ') space++;
    if (*space != ' ') {
        puts("Usage: write <path> <text>");
        return;
    }
    int pathlen = (int)(space - arg);
    if (pathlen <= 0 || pathlen >= SHELL_PATH_MAX) {
        puts("Invalid path");
        return;
    }
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
        if (ret < 0) {
            printf("write: cannot create '%s'\n", path);
            return;
        }
        fd = vfs_open(path, O_RDONLY);
        if (fd < 0) {
            printf("write: cannot open '%s' after create\n", path);
            return;
        }
    }
    
    int written = vfs_write_fd(fd, text, (unsigned int)textlen);
    vfs_close_fd(fd);
    
    if (written < 0) {
        printf("write: failed to write to '%s'\n", path);
    } else {
        printf("Wrote %d bytes to '%s'\n", written, path);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("=== Lebirun Mini Shell ===\n");
    printf("Type 'help' for available commands.\n\n");

    char line[64];
    int pos = 0;

    while (1) {
        printf("leb> ");
        pos = 0;
        while (1) {
            int n = getchar();
            if (n < 0) continue;
            char c = (char)n;
            if (c == '\n' || c == '\r') {
                putchar('\n');
                line[pos] = '\0';
                break;
            } else if (c == '\b' || c == 127) {
                if (pos > 0) {
                    pos--;
                    putchar('\b'); putchar(' '); putchar('\b');
                }
            } else if (c >= 32 && pos < 62) {
                line[pos++] = c;
                putchar(c);
            }
        }

        if (strcmp(line, "help") == 0) {
            puts("Available commands:");
            puts("  help       - show this message");
            puts("  echo <t>   - echo text");
            puts("  pid        - show PID");
            puts("  ticks      - show tick count");
            puts("  sleep N    - sleep N milliseconds");
            puts("  fork       - test fork syscall");
            puts("  initrd ls  - list initrd files");
            puts("  initrd cat N|name - show file by index or name");
            puts("  colors     - 32-bit color graphics demo!");
            puts("  console N  - switch to console N (0-8)");
            puts("  mounts     - list VFS mounts");
            puts("  ls [path]  - list VFS directory");
            puts("  cd <path>  - change directory");
            puts("  pwd        - print working directory");
            puts("  vcat <path> - read file via VFS");
            puts("  touch <path> - create empty file");
            puts("  mkdir <path> - create directory");
            puts("  rm <path>  - remove file/empty dir");
            puts("  write <path> <text> - write text to file");
            puts("  exit       - exit shell");
        } else if (strcmp(line, "fork") == 0) {
            test_fork();
        } else if (strcmp(line, "pwd") == 0) {
            cmd_pwd();
        } else if (strcmp(line, "cd") == 0) {
            cmd_cd(NULL);
        } else if (strncmp(line, "cd ", 3) == 0) {
            cmd_cd(&line[3]);
        } else if (strcmp(line, "initrd ls") == 0) {
            cmd_initrd_ls();
        } else if (strncmp(line, "initrd cat ", 11) == 0) {
            cmd_initrd_cat(&line[11]);
        } else if (strcmp(line, "colors") == 0) {
            cmd_colors();
        } else if (strncmp(line, "console ", 8) == 0) {
            cmd_console(&line[8]);
        } else if (strcmp(line, "console") == 0) {
            cmd_console(NULL);
        } else if (strcmp(line, "mounts") == 0) {
            cmd_vfs_mounts();
        } else if (strcmp(line, "ls") == 0) {
            cmd_vfs_ls(NULL);
        } else if (strncmp(line, "ls ", 3) == 0) {
            cmd_vfs_ls(&line[3]);
        } else if (strncmp(line, "vcat ", 5) == 0) {
            cmd_vfs_cat(&line[5]);
        } else if (strncmp(line, "touch ", 6) == 0) {
            cmd_touch(&line[6]);
        } else if (strncmp(line, "mkdir ", 6) == 0) {
            cmd_mkdir_shell(&line[6]);
        } else if (strncmp(line, "rm ", 3) == 0) {
            cmd_rm(&line[3]);
        } else if (strncmp(line, "write ", 6) == 0) {
            cmd_write_file(&line[6]);
        } else if (strncmp(line, "echo ", 5) == 0) {
            puts(&line[5]);
        } else if (strcmp(line, "pid") == 0) {
            printf("PID: %d\n", getpid());
        } else if (strcmp(line, "ticks") == 0) {
            printf("Ticks: %u\n", getticks());
        } else if (strncmp(line, "sleep ", 6) == 0) {
            int ms = 0;
            const char *p = &line[6];
            while (*p >= '0' && *p <= '9') {
                ms = ms * 10 + (*p - '0');
                p++;
            }
            printf("Sleeping %d ms...\n", ms);
            sleep_ms(ms);
            puts("Awake!");
        } else if (strcmp(line, "exit") == 0) {
            puts("Goodbye!\n");
            exit(0);
        } else if (line[0] == '\0') {
        } else {
            printf("Unknown command: %s\n", line);
        }
    }

    return 0;
}
