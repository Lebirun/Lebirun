#include <lebirun/cmdline.h>
#include <lebirun/console.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdio.h>

static char cmdline_buf[CMDLINE_MAX];
static char init_path[CMDLINE_INIT_PATH_MAX];
static char root_dev[64];
static int num_consoles;
static int text_mode;
static int lke_enabled;
static int vringtest_enabled;

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

void cmdline_parse(const char *cmdline_str)
{
    const char *val;
    int len;

    strcpy(init_path, "/init");
    num_consoles = 4;
    root_dev[0] = '\0';
    cmdline_buf[0] = '\0';
    text_mode = 0;
    lke_enabled = 1;
    vringtest_enabled = 0;

    if (!cmdline_str)
        return;

    len = 0;
    while (cmdline_str[len] && len < CMDLINE_MAX - 1)
        len++;
    memcpy(cmdline_buf, cmdline_str, len);
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

    val = find_param(cmdline_buf, "root");
    if (val)
        extract_value(val, root_dev, sizeof(root_dev));

    val = find_param(cmdline_buf, "text");
    if (val)
        text_mode = parse_int(val);

    val = find_param(cmdline_buf, "lke");
    if (val)
        lke_enabled = parse_int(val);

    val = find_param(cmdline_buf, "vringtest");
    if (val)
        vringtest_enabled = parse_int(val);

    printf("CMDLINE: init=%s consoles=%d root=%s text=%d lke=%d vringtest=%d\n", init_path, num_consoles, root_dev[0] ? root_dev : "(none)", text_mode, lke_enabled, vringtest_enabled);
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

int cmdline_get_lke(void)
{
    return lke_enabled;
}

const char *cmdline_get_root(void)
{
    return root_dev[0] ? root_dev : NULL;
}

int cmdline_get_text_mode(void)
{
    return text_mode;
}

int cmdline_get_vringtest(void)
{
    return vringtest_enabled;
}
