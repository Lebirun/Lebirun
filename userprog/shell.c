#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <graphics.h>

#define O_RDONLY 0

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

static void cmd_open(const char *arg) {
    if (!arg || *arg == '\0') {
        puts("Usage: open <path>");
        return;
    }
    int fd = open(arg, O_RDONLY);
    if (fd < 0) {
        printf("Failed to open '%s'\n", arg);
    } else {
        printf("Opened '%s' as fd %d\n", arg, fd);
    }
}

static void cmd_read(const char *arg) {
    if (!arg || *arg == '\0') {
        puts("Usage: read <fd|name>");
        return;
    }

    int is_num = 1;
    for (const char *p = arg; *p; p++) {
        if (*p < '0' || *p > '9') { is_num = 0; break; }
    }

    static char buf[4097];
    if (is_num) {
        int fd = 0;
        const char *p = arg;
        while (*p >= '0' && *p <= '9') { fd = fd * 10 + (*p - '0'); p++; }
        int rd = read(fd, buf, 4096);
        if (rd < 0) {
            printf("Failed to read fd %d\n", fd);
        } else if (rd == 0) {
            printf("EOF on fd %d\n", fd);
        } else {
            if (rd > 4096) rd = 4096;
            buf[rd] = '\0';
            printf("Read %d bytes:\n%s\n", rd, buf);
        }
        return;
    } else {
        int fd = open(arg, O_RDONLY);
        if (fd < 0) {
            printf("Failed to open '%s' for read\n", arg);
            return;
        }
        int rd = read(fd, buf, 4096);
        if (rd < 0) {
            printf("Failed to read '%s' (fd %d)\n", arg, fd);
            close(fd);
            return;
        } else if (rd == 0) {
            printf("EOF on '%s' (fd %d)\n", arg, fd);
            close(fd);
            return;
        } else {
            if (rd > 4096) rd = 4096;
            buf[rd] = '\0';
            printf("Read %d bytes from '%s':\n%s\n", rd, arg, buf);
        }
        close(fd);
    }
} 

static void cmd_close(const char *arg) {
    if (!arg || *arg == '\0') {
        puts("Usage: close <fd>");
        return;
    }
    if (arg[0] < '0' || arg[0] > '9') {
        printf("close: expected numeric fd, got '%s'\n", arg);
        return;
    }
    int fd = 0;
    const char *p = arg;
    while (*p >= '0' && *p <= '9') { fd = fd * 10 + (*p - '0'); p++; }

    int ret = close(fd);
    if (ret < 0) {
        printf("Failed to close fd %d\n", fd);
    } else {
        printf("Closed fd %d\n", fd);
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
            puts("  open <path> - open file, returns fd");
            puts("  read <fd|name>  - read from fd or open+read name");
            puts("  close <fd> - close fd");
            puts("  colors     - 32-bit color graphics demo!");
            puts("  console N  - switch to console N (0-8)");
            puts("  exit       - exit shell");
        } else if (strcmp(line, "fork") == 0) {
            test_fork();
        } else if (strcmp(line, "initrd ls") == 0) {
            cmd_initrd_ls();
        } else if (strncmp(line, "initrd cat ", 11) == 0) {
            cmd_initrd_cat(&line[11]);
        } else if (strncmp(line, "open ", 5) == 0) {
            cmd_open(&line[5]);
        } else if (strncmp(line, "read ", 5) == 0) {
            cmd_read(&line[5]);
        } else if (strncmp(line, "close ", 6) == 0) {
            cmd_close(&line[6]);
        } else if (strcmp(line, "colors") == 0) {
            cmd_colors();
        } else if (strncmp(line, "console ", 8) == 0) {
            cmd_console(&line[8]);
        } else if (strcmp(line, "console") == 0) {
            cmd_console(NULL);
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
