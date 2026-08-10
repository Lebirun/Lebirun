#include <lebirun/cmdline.h>
#include <lebirun/common.h>
#include <lebirun/console.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdio.h>

static char *cmdline_buf;
static char *init_path;
static char *root_dev;
static int num_consoles;
static int text_mode KERNEL_INIT_BSS;
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

void KERNEL_INIT cmdline_reclaim_boot_values(void)
{
    kfree(init_path);
    kfree(root_dev);
    init_path = NULL;
    root_dev = NULL;
}
