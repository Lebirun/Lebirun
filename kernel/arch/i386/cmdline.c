#include <kernel/cmdline.h>
#include <kernel/console.h>
#include <kernel/mem_map.h>
#include <string.h>
#include <stdio.h>

static char cmdline_buf[CMDLINE_MAX];
static char init_path[CMDLINE_INIT_PATH_MAX];
static int num_consoles;

static int parse_int(const char *s)
{
    int val;
    int i;

    val = 0;
    for (i = 0; s[i] >= '0' && s[i] <= '9'; i++)
        val = val * 10 + (s[i] - '0');
    return val;
}

static const char *find_param(const char *cmdline, const char *key)
{
    const char *p;
    int klen;

    klen = strlen(key);
    p = cmdline;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
            return p + klen + 1;
        while (*p && *p != ' ')
            p++;
        while (*p == ' ')
            p++;
    }
    return NULL;
}

static void extract_value(const char *start, char *out, int out_max)
{
    int i;

    for (i = 0; i < out_max - 1 && start[i] && start[i] != ' '; i++)
        out[i] = start[i];
    out[i] = '\0';
}

void cmdline_parse(uint32_t multiboot_flags, uint32_t cmdline_phys)
{
    const char *raw;
    const char *val;
    uint32_t page;
    int len;

    strcpy(init_path, "/bin/init");
    num_consoles = 4;
    cmdline_buf[0] = '\0';

    if (!(multiboot_flags & (1 << 2)))
        return;
    if (!cmdline_phys)
        return;

    page = cmdline_phys & ~0xFFF;
    vmm_map_page(page + 0xC0000000, page, 0x003);
    if (((cmdline_phys + CMDLINE_MAX) & ~0xFFF) != page)
        vmm_map_page(((cmdline_phys + CMDLINE_MAX) & ~0xFFF) + 0xC0000000,
                     (cmdline_phys + CMDLINE_MAX) & ~0xFFF, 0x003);

    raw = (const char *)(cmdline_phys + 0xC0000000);
    len = 0;
    while (raw[len] && len < CMDLINE_MAX - 1)
        len++;
    memcpy(cmdline_buf, raw, len);
    cmdline_buf[len] = '\0';

    printf("CMDLINE: \"%s\"\n", cmdline_buf);

    val = find_param(cmdline_buf, "init");
    if (val)
        extract_value(val, init_path, CMDLINE_INIT_PATH_MAX);

    val = find_param(cmdline_buf, "consoles");
    if (val) {
        num_consoles = parse_int(val);
        if (num_consoles < 1)
            num_consoles = 1;
        if (num_consoles > NUM_CONSOLES)
            num_consoles = NUM_CONSOLES;
    }

    printf("CMDLINE: init=%s consoles=%d\n", init_path, num_consoles);
}

const char *cmdline_get(void)
{
    return cmdline_buf;
}

const char *cmdline_get_init(void)
{
    return init_path;
}

int cmdline_get_consoles(void)
{
    return num_consoles;
}
