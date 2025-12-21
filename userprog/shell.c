#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <syscall.h>

#define O_RDONLY 0
#define SHELL_PATH_MAX 256

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
            if (pos) { pos--; putchar('\b'); putchar(' '); putchar('\b'); }
            continue;
        }
        if (c >= 32) {
            if (pos + 1 < cap) { buf[pos++] = c; putchar(c); }
            else { overflow = true; putchar('\a'); }
        }
    }
}

void help() {
    printf("Available commands (Shell-builtin):\n");
    printf("help\n");
    printf("echo\n");
    printf("ticks\n");
    printf("cd <path>\n");
    printf("ls [path]\n");
    printf("cat <path>\n");
    printf("pwd\n");
    printf("touch <path>\n");
    printf("mkdir <path>\n");
    printf("rm <path>\n");
    printf("write <path> <text>\n");
    printf("sata [test|info|smart|irq]\n");
    printf("\nNetwork commands:\n");
    printf("ifconfig              - show network interface info (IP, MAC, gateway)\n");
    printf("ping <ip> [count]     - ping an IP address\n");
    printf("                        Examples: ping 10.0.2.2\n");
    printf("                                  ping 8.8.8.8 10\n");
    printf("arp                   - show ARP cache (IP to MAC mappings)\n");
    printf("dns <hostname>        - resolve DNS hostname to IP\n");
    printf("                        Example: dns google.com\n");
    printf("dhcp [renew]          - show DHCP status or request new lease\n");
}

void echo(const char *line) {
    if (!line) { putchar('\n'); return; }
    const char *p = line + 4;
    while (*p == ' ') p++;
    if (*p == '\0') putchar('\n');
    else puts(p);
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
        strncpy(cwd, "/", sizeof(cwd));
        cwd[sizeof(cwd) - 1] = '\0';
        return;
    }
    resolve_path(path, resolved, sizeof(resolved));

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

void mkdir(const char *arg) {
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

void sata(const char *arg) {
    if (!arg || *arg == '\0') {
        printf("sata: usage: sata [test|info|smart|irq]\n");
        printf("  test - run SATA read/write test\n");
        printf("  info - show AHCI controller info\n");
        printf("  smart - show S.M.A.R.T. data\n");
        printf("  irq [on|off] - enable/disable interrupts\n");
        return;
    }
    
    if (strcmp(arg, "test") == 0) {
        printf("Running SATA read/write test...\n");
        int result = syscall0(SYS_SATA_TEST);
        if (result == 0) {
            printf("SATA test completed successfully!\n");
        } else {
            printf("SATA test failed (error %d)\n", result);
        }
    } else if (strcmp(arg, "info") == 0) {
        printf("AHCI Controller Information:\n");
        int num_ports = syscall0(SYS_SATA_INFO);
        if (num_ports >= 0) {
            printf("Active ports: %d\n", num_ports);
        }
    } else if (strcmp(arg, "smart") == 0) {
        printf("Reading S.M.A.R.T. data from port 0...\n");
        syscall1(SYS_SATA_SMART, 0);
    } else if (strncmp(arg, "irq", 3) == 0) {
        const char *sub = arg + 3;
        while (*sub == ' ') sub++;
        if (strcmp(sub, "on") == 0) {
            printf("Enabling SATA interrupts...\n");
            syscall1(SYS_SATA_IRQ, 1);
        } else if (strcmp(sub, "off") == 0) {
            printf("Disabling SATA interrupts...\n");
            syscall1(SYS_SATA_IRQ, 0);
        } else {
            printf("Usage: sata irq [on|off]\n");
        }
    } else {
        printf("sata: unknown subcommand '%s'\n", arg);
        printf("Use 'sata test', 'sata info', 'sata smart', or 'sata irq [on|off]'\n");
    }
}

static uint32_t parse_ip(const char *str) {
    uint32_t octets[4] = {0, 0, 0, 0};
    int idx = 0;
    const char *p = str;
    
    while (*p && idx < 4) {
        if (*p >= '0' && *p <= '9') {
            octets[idx] = octets[idx] * 10 + (*p - '0');
        } else if (*p == '.') {
            idx++;
        } else {
            break;
        }
        p++;
    }
    
    if (idx != 3) return 0;
    
    return (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
}

static void print_ip(uint32_t ip) {
    printf("%u.%u.%u.%u", 
           (ip >> 24) & 0xFF, 
           (ip >> 16) & 0xFF, 
           (ip >> 8) & 0xFF, 
           ip & 0xFF);
}

typedef struct {
    char name[16];
    uint8_t mac[6];
    uint8_t _pad[2];
    uint32_t ipv4;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint16_t mtu;
    uint8_t link_up;
    uint8_t dhcp_configured;
} __attribute__((packed)) netinfo_t;

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
} __attribute__((packed)) arp_entry_t;

void cmd_ifconfig(void) {
    printf("=== Network Interface Configuration ===\n");
    
    netinfo_t info;
    int result = syscall1(SYS_NET_GETINFO, (int)&info);
    
    if (result < 0) {
        printf("ifconfig: No network interface available\n");
        printf("========================================\n");
        return;
    }
    
    printf("Interface: %s\n", info.name);
    printf("  Link:    %s\n", info.link_up ? "UP" : "DOWN");
    printf("  MAC:     %02X:%02X:%02X:%02X:%02X:%02X\n",
           info.mac[0], info.mac[1], info.mac[2],
           info.mac[3], info.mac[4], info.mac[5]);
    printf("  IPv4:    ");
    print_ip(info.ipv4);
    printf("\n");
    printf("  Netmask: ");
    print_ip(info.netmask);
    printf("\n");
    printf("  Gateway: ");
    print_ip(info.gateway);
    printf("\n");
    printf("  DNS:     ");
    print_ip(info.dns);
    printf("\n");
    printf("  MTU:     %u\n", info.mtu);
    printf("  DHCP:    %s\n", info.dhcp_configured ? "Configured" : "Not configured");
    printf("========================================\n");
}

void cmd_ping(const char *arg) {
    if (!arg || *arg == '\0') {
        printf("ping: usage: ping <ip address> [count]\n");
        printf("  Example: ping 10.0.2.2\n");
        printf("  Example: ping 8.8.8.8 10\n");
        return;
    }
    
    char ip_str[32];
    int i = 0;
    while (arg[i] && arg[i] != ' ' && i < 31) {
        ip_str[i] = arg[i];
        i++;
    }
    ip_str[i] = '\0';
    
    int count = 4;
    if (arg[i] == ' ') {
        const char *count_str = arg + i + 1;
        while (*count_str == ' ') count_str++;
        if (*count_str) {
            count = 0;
            while (*count_str >= '0' && *count_str <= '9') {
                count = count * 10 + (*count_str - '0');
                count_str++;
            }
            if (count <= 0) count = 4;
            if (count > 100) count = 100;
        }
    }
    
    uint32_t ip = parse_ip(ip_str);
    if (ip == 0) {
        printf("ping: invalid IP address '%s'\n", ip_str);
        return;
    }
    
    printf("PING ");
    print_ip(ip);
    printf(" (%s) with %d packets:\n", ip_str, count);
    
    int received = 0;
    uint32_t total_rtt = 0;
    int interrupted = 0;
    
    for (int seq = 0; seq < count; seq++) {
        int result = syscall3(SYS_NET_PING_ONE, (int)ip, seq, 3000);
        
        if (result >= 0) {
            printf("Reply from ");
            print_ip(ip);
            printf(": seq=%d time=%d ms\n", seq, result);
            received++;
            total_rtt += (uint32_t)result;
        } else if (result == -1) {
            printf("Request timeout for seq %d\n", seq);
        } else if (result == -2) {
            printf("ping: interrupted\n");
            interrupted = 1;
            break;
        } else if (result == -3) {
            printf("ping: send failed for seq %d\n", seq);
        } else if (result == -4) {
            printf("ping: no network interface\n");
            return;
        }
    }
    
    printf("\n--- ");
    print_ip(ip);
    printf(" ping statistics ---\n");
    
    int sent = interrupted ? (received + (count - received)) : count;
    
    if (interrupted) {
        printf("%d packets transmitted, %d received\n", received + 1, received);
    } else {
        int loss = count > 0 ? ((count - received) * 100 / count) : 0;
        printf("%d packets transmitted, %d received, %d%% loss\n", count, received, loss);
    }
    
    if (received > 0) {
        printf("avg rtt = %u ms\n", total_rtt / (uint32_t)received);
    }
}

void cmd_arp(void) {
    printf("=== ARP Cache ===\n");
    printf("IP Address       MAC Address\n");
    printf("-----------------------------------------\n");
    
    arp_entry_t entries[16];
    int count = 0;
    
    int result = syscall3(SYS_NET_ARP_GET, (int)entries, (int)&count, 16);
    
    if (result < 0 || count == 0) {
        printf("(empty)\n");
    } else {
        for (int i = 0; i < count; i++) {
            print_ip(entries[i].ip);
            int ip_len = 0;
            uint32_t tmp = entries[i].ip;
            for (int j = 0; j < 4; j++) {
                uint8_t octet = tmp & 0xFF;
                if (octet >= 100) ip_len += 3;
                else if (octet >= 10) ip_len += 2;
                else ip_len += 1;
                tmp >>= 8;
            }
            ip_len += 3;
            for (int j = ip_len; j < 16; j++) printf(" ");
            
            printf(" %02X:%02X:%02X:%02X:%02X:%02X\n",
                   entries[i].mac[0], entries[i].mac[1], entries[i].mac[2],
                   entries[i].mac[3], entries[i].mac[4], entries[i].mac[5]);
        }
    }
    printf("-----------------------------------------\n");
    printf("Total entries: %d\n", count);
}

void cmd_dns(const char *arg) {
    if (!arg || *arg == '\0') {
        printf("dns: usage: dns <hostname>\n");
        printf("  Example: dns google.com\n");
        printf("  Example: dns example.org\n");
        return;
    }
    
    printf("=== DNS Lookup ===\n");
    printf("Resolving hostname: %s\n", arg);
    printf("Querying DNS server...\n");
    
    uint32_t resolved_ip = 0;
    int result = syscall3(SYS_NET_DNS_RESOLVE, (int)arg, (int)&resolved_ip, 0);
    
    if (result == 0) {
        printf("Result: %s -> ", arg);
        print_ip(resolved_ip);
        printf("\n");
        printf("DNS lookup successful!\n");
    } else {
        printf("DNS lookup failed for '%s'\n", arg);
        printf("Possible reasons:\n");
        printf("  - Hostname does not exist\n");
        printf("  - DNS server unreachable\n");
        printf("  - Network not configured\n");
    }
    printf("==================\n");
}

void cmd_dhcp(const char *arg) {
    printf("=== DHCP Status ===\n");
    if (!arg || *arg == '\0') {
        int status = syscall1(SYS_NET_DHCP, 0);
        if (status == 1) {
            printf("DHCP lease is active (bound)\n");
            printf("Run 'ifconfig' to see assigned addresses\n");
        } else if (status == 0) {
            printf("DHCP is not bound\n");
            printf("Run 'dhcp renew' to request an IP address\n");
        } else {
            printf("DHCP status check failed\n");
        }
    } else if (strcmp(arg, "renew") == 0) {
        printf("Requesting new DHCP lease...\n");
        printf("Sending DHCP Discover...\n");
        int result = syscall1(SYS_NET_DHCP, 1);
        if (result == 0) {
            printf("DHCP request sent. Waiting for response...\n");
            printf("Run 'ifconfig' to check if an IP was assigned\n");
        } else {
            printf("DHCP request failed\n");
        }
    } else if (strcmp(arg, "release") == 0) {
        printf("DHCP release not yet implemented\n");
    } else {
        printf("dhcp: usage: dhcp [renew|release]\n");
        printf("  dhcp        - show DHCP status\n");
        printf("  dhcp renew  - request new IP lease\n");
    }
    printf("===================\n");
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
            mkdir((*arg == '\0') ? NULL : arg);
        } else if (strncmp(line, "rm", 2) == 0 && (line[2] == '\0' || line[2] == ' ')) {
            const char *arg = line + 2;
            while (*arg == ' ') arg++;
            rm((*arg == '\0') ? NULL : arg);
        } else if (strncmp(line, "write", 5) == 0 && (line[5] == '\0' || line[5] == ' ')) {
            const char *arg = line + 5;
            while (*arg == ' ') arg++;
            writer((*arg == '\0') ? NULL : arg);
        } else if (strncmp(line, "sata", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
            const char *arg = line + 4;
            while (*arg == ' ') arg++;
            sata((*arg == '\0') ? NULL : arg);
        } else if (strcmp(line, "ifconfig") == 0) {
            cmd_ifconfig();
        } else if (strncmp(line, "ping", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
            const char *arg = line + 4;
            while (*arg == ' ') arg++;
            cmd_ping((*arg == '\0') ? NULL : arg);
        } else if (strcmp(line, "arp") == 0) {
            cmd_arp();
        } else if (strncmp(line, "dns", 3) == 0 && (line[3] == '\0' || line[3] == ' ')) {
            const char *arg = line + 3;
            while (*arg == ' ') arg++;
            cmd_dns((*arg == '\0') ? NULL : arg);
        } else if (strncmp(line, "dhcp", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
            const char *arg = line + 4;
            while (*arg == ' ') arg++;
            cmd_dhcp((*arg == '\0') ? NULL : arg);
        } else if (is_executable_path(line)) {
            run_binary(line);
        } else {
            printf("Unknown command: %s\nType 'help' for available commands.\n", line);
        }
    }
}