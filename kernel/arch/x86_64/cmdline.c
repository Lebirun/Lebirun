#include <lebirun/cmdline.h>
#include <lebirun/common.h>
#include <lebirun/console.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdio.h>

static char *cmdline_buf;
static char *init_path KERNEL_INIT_BSS;
static char *root_dev KERNEL_INIT_BSS;
static int num_consoles;
static int text_mode KERNEL_INIT_BSS;
static uint32_t screen_res_w KERNEL_INIT_BSS;
static uint32_t screen_res_h KERNEL_INIT_BSS;
static int lke_enabled KERNEL_INIT_BSS;

static int KERNEL_EARLY_INIT parse_int(const char *s)
{
    int val;
    int i;

    val = 0;
    for (i = 0; s[i] >= '0' && s[i] <= '9'; i++)
        val = val * 10 + (s[i] - '0');
    return val;
}

static int KERNEL_EARLY_INIT parse_resolution(const char *s, uint32_t *w, uint32_t *h)
{
    uint32_t width;
    uint32_t height;

    width = 0;
    while (*s >= '0' && *s <= '9') {
        width = width * 10 + (uint32_t)(*s - '0');
        if (width > 16384) return -1;
        s++;
    }
    if (width == 0) return -1;
    if (*s != 'x' && *s != 'X') return -1;
    s++;
    height = 0;
    while (*s >= '0' && *s <= '9') {
        height = height * 10 + (uint32_t)(*s - '0');
        if (height > 16384) return -1;
        s++;
    }
    if (height == 0) return -1;
    if (*s && *s != ' ') return -1;
    *w = width;
    *h = height;
    return 0;
}

static const char *KERNEL_EARLY_INIT find_param(const char *cmdline,
                                                const char *key)
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

static char *KERNEL_EARLY_INIT duplicate_value(const char *start)
{
    size_t length;
    char *value;

    length = 0;
    while (start[length] && start[length] != ' ') length++;
    if (length == SIZE_MAX) return NULL;
    value = (char *)kmalloc(length + 1);
    if (!value) return NULL;
    memcpy(value, start, length);
    value[length] = '\0';
    return value;
}

void KERNEL_EARLY_INIT cmdline_parse(const char *cmdline_str)
{
    const char *val;
    const char *raw;
    size_t raw_len;

    init_path = NULL;
    num_consoles = 2;
    root_dev = NULL;
    raw = cmdline_str ? cmdline_str : "";
    raw_len = strlen(raw);
    cmdline_buf = (char *)kmalloc(raw_len + 1);
    if (cmdline_buf)
        memcpy(cmdline_buf, raw, raw_len + 1);
    text_mode = 0;
    screen_res_w = 1024;
    screen_res_h = 768;
    lke_enabled = 1;

    if (cmdline_str) {
        val = find_param(raw, "init");
        if (val)
            init_path = duplicate_value(val);

        val = find_param(raw, "consoles");
        if (val) {
            num_consoles = parse_int(val);
            if (num_consoles < 1)
                num_consoles = 1;
        }

        val = find_param(raw, "root");
        if (val)
            root_dev = duplicate_value(val);

        val = find_param(raw, "text");
        if (val)
            text_mode = parse_int(val);

        val = find_param(raw, "screen.res");
        if (val)
            parse_resolution(val, &screen_res_w, &screen_res_h);

        val = find_param(raw, "lke");
        if (val)
            lke_enabled = parse_int(val);
    }

    if (raw[0]) printf("CMDLINE: \"%s\"\n", raw);
}

const char *cmdline_get(void)
{
    return cmdline_buf ? cmdline_buf : "";
}

const char *KERNEL_INIT cmdline_get_init(void)
{
    return init_path ? init_path : "/init";
}

int cmdline_get_consoles(void)
{
    return num_consoles;
}

int KERNEL_INIT cmdline_get_lke(void)
{
    return lke_enabled;
}

const char *KERNEL_INIT cmdline_get_root(void)
{
    return root_dev && root_dev[0] ? root_dev : NULL;
}

int KERNEL_INIT cmdline_get_text_mode(void)
{
    return text_mode;
}

uint32_t KERNEL_INIT cmdline_get_screen_width(void)
{
    return screen_res_w;
}

uint32_t KERNEL_INIT cmdline_get_screen_height(void)
{
    return screen_res_h;
}

void KERNEL_INIT cmdline_reclaim_boot_values(void)
{
    kfree(init_path);
    kfree(root_dev);
    init_path = NULL;
    root_dev = NULL;
}
