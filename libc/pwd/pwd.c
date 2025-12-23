#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>

static char root_name[] = "root";
static char root_passwd[] = "x";
static char root_gecos[] = "root";
static char root_dir[] = "/root";
static char root_shell[] = "/bin/sh";

static struct passwd root_passwd_entry = {
    .pw_name = root_name,
    .pw_passwd = root_passwd,
    .pw_uid = 0,
    .pw_gid = 0,
    .pw_gecos = root_gecos,
    .pw_dir = root_dir,
    .pw_shell = root_shell
};

struct passwd *getpwnam(const char *name) {
    if (strcmp(name, "root") == 0) {
        return &root_passwd_entry;
    }
    return NULL;
}

struct passwd *getpwuid(uid_t uid) {
    if (uid == 0) {
        return &root_passwd_entry;
    }
    return NULL;
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result) {
    (void)buf;
    if (strcmp(name, "root") == 0) {
        if (buflen < 64) {
            *result = NULL;
            return ERANGE;
        }
        *pwd = root_passwd_entry;
        *result = pwd;
        return 0;
    }
    *result = NULL;
    return 0;
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result) {
    (void)buf;
    if (uid == 0) {
        if (buflen < 64) {
            *result = NULL;
            return ERANGE;
        }
        *pwd = root_passwd_entry;
        *result = pwd;
        return 0;
    }
    *result = NULL;
    return 0;
}

static int pwent_index = 0;

void setpwent(void) {
    pwent_index = 0;
}

void endpwent(void) {
    pwent_index = 0;
}

struct passwd *getpwent(void) {
    if (pwent_index == 0) {
        pwent_index++;
        return &root_passwd_entry;
    }
    return NULL;
}

static char root_group_name[] = "root";
static char root_group_passwd[] = "x";
static char *root_group_members[] = { NULL };

static struct group root_group_entry = {
    .gr_name = root_group_name,
    .gr_passwd = root_group_passwd,
    .gr_gid = 0,
    .gr_mem = root_group_members
};

struct group *getgrnam(const char *name) {
    if (strcmp(name, "root") == 0) {
        return &root_group_entry;
    }
    return NULL;
}

struct group *getgrgid(gid_t gid) {
    if (gid == 0) {
        return &root_group_entry;
    }
    return NULL;
}

int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen, struct group **result) {
    (void)buf;
    if (strcmp(name, "root") == 0) {
        if (buflen < 32) {
            *result = NULL;
            return ERANGE;
        }
        *grp = root_group_entry;
        *result = grp;
        return 0;
    }
    *result = NULL;
    return 0;
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen, struct group **result) {
    (void)buf;
    if (gid == 0) {
        if (buflen < 32) {
            *result = NULL;
            return ERANGE;
        }
        *grp = root_group_entry;
        *result = grp;
        return 0;
    }
    *result = NULL;
    return 0;
}

static int grent_index = 0;

void setgrent(void) {
    grent_index = 0;
}

void endgrent(void) {
    grent_index = 0;
}

struct group *getgrent(void) {
    if (grent_index == 0) {
        grent_index++;
        return &root_group_entry;
    }
    return NULL;
}
