static int sys_write(int fd, const char *buf, int len) {
    int ret;
    asm volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(1), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static int sys_read(int fd, char *buf, int len) {
    int ret;
    asm volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(3), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static void sys_exit(int code) {
    asm volatile (
        "int $0x80"
        : 
        : "a"(0), "b"(code)
    );
    while (1);
}

static int strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int strncmp(const char *a, const char *b, int n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? ((unsigned char)*a - (unsigned char)*b) : 0;
}

static void puts(const char *s) {
    sys_write(1, s, strlen(s));
}

static void putchar(char c) {
    sys_write(1, &c, 1);
}

static void print_prompt(void) {
    puts("leb> ");
}

static void cmd_help(void) {
    puts("Available commands:\n");
    puts("  help   - Show this message\n");
    puts("  about  - About Lebirun\n");
    puts("  echo   - Echo back arguments\n");
    puts("  clear  - Clear screen (not implemented)\n");
    puts("  exit   - Exit shell\n");
}

static void cmd_about(void) {
    puts("Lebirun OS - A minimal hobby operating system\n");
    puts("User shell running in Ring 3!\n");
}

static void cmd_echo(const char *args) {
    if (args && *args) {
        puts(args);
    }
    puts("\n");
}

static void cmd_unknown(const char *cmd) {
    puts("Unknown command: ");
    puts(cmd);
    puts("\nType 'help' for commands.\n");
}

static void execute(char *line) {
    while (*line == ' ') line++;
    if (*line == '\0') return;

    char *args = line;
    while (*args && *args != ' ') args++;
    if (*args == ' ') {
        *args++ = '\0';
        while (*args == ' ') args++;
    }

    if (strcmp(line, "help") == 0) {
        cmd_help();
    } else if (strcmp(line, "about") == 0) {
        cmd_about();
    } else if (strcmp(line, "echo") == 0) {
        cmd_echo(args);
    } else if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
        puts("Goodbye!\n");
        sys_exit(0);
    } else if (strcmp(line, "clear") == 0) {
        puts("[clear not implemented]\n");
    } else {
        cmd_unknown(line);
    }
}

void __attribute__((section(".text.entry"))) _start(void) {
    char line[64];
    int pos = 0;

    puts("\n=== Lebirun Mini Shell ===\n");
    puts("Type 'help' for available commands.\n\n");

    while (1) {
        print_prompt();
        pos = 0;

        while (1) {
            char c;
            int n = sys_read(0, &c, 1);
            if (n <= 0) continue;

            if (c == '\n' || c == '\r') {
                putchar('\n');
                line[pos] = '\0';
                break;
            } else if (c == '\b' || c == 127) {
                if (pos > 0) {
                    pos--;
                    puts("\b \b");
                }
            } else if (c >= 32 && pos < 62) {
                line[pos++] = c;
                putchar(c);
            }
        }

        execute(line);
    }
}