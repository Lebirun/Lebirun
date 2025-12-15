// NOTE: THIS SHELL IS A STUB!!! IT WILL BE REPLACED WITH A BETTER SHELL LATER ONCE SOME THINGS ARE DONE.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("\n=== Lebirun Mini Shell ===\n");
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
            puts("  help     - show this message");
            puts("  echo <t> - echo text");
            puts("  pid      - show PID");
            puts("  ticks    - show tick count");
            puts("  sleep N  - sleep N milliseconds");
            puts("  exit     - exit shell");
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
