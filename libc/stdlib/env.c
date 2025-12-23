#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#define MAX_ENV_VARS 128
#define MAX_ENV_SIZE 4096

static char env_storage[MAX_ENV_SIZE];
static int env_storage_used = 0;

static char *env_array[MAX_ENV_VARS + 1];
static int env_count = 0;

char **environ = env_array;

static char *env_strdup(const char *s) {
    int len = strlen(s) + 1;
    if (env_storage_used + len > MAX_ENV_SIZE) {
        return NULL;
    }
    char *dest = &env_storage[env_storage_used];
    memcpy(dest, s, len);
    env_storage_used += len;
    return dest;
}

static int env_find(const char *name, size_t len) {
    for (int i = 0; i < env_count; i++) {
        if (strncmp(env_array[i], name, len) == 0 && env_array[i][len] == '=') {
            return i;
        }
    }
    return -1;
}

char *getenv(const char *name) {
    if (!name) return NULL;
    size_t len = strlen(name);
    int idx = env_find(name, len);
    if (idx < 0) return NULL;
    return env_array[idx] + len + 1;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) {
        return -1;
    }
    size_t namelen = strlen(name);
    int idx = env_find(name, namelen);
    if (idx >= 0 && !overwrite) {
        return 0;
    }
    size_t vallen = strlen(value);
    size_t total = namelen + 1 + vallen + 1;
    if (env_storage_used + total > MAX_ENV_SIZE) {
        return -1;
    }
    char *entry = &env_storage[env_storage_used];
    memcpy(entry, name, namelen);
    entry[namelen] = '=';
    memcpy(entry + namelen + 1, value, vallen + 1);
    env_storage_used += total;
    if (idx >= 0) {
        env_array[idx] = entry;
    } else {
        if (env_count >= MAX_ENV_VARS) return -1;
        env_array[env_count++] = entry;
        env_array[env_count] = NULL;
    }
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) {
        return -1;
    }
    size_t len = strlen(name);
    int idx = env_find(name, len);
    if (idx < 0) return 0;
    for (int i = idx; i < env_count - 1; i++) {
        env_array[i] = env_array[i + 1];
    }
    env_count--;
    env_array[env_count] = NULL;
    return 0;
}

int putenv(char *string) {
    if (!string) return -1;
    char *eq = strchr(string, '=');
    if (!eq) return -1;
    size_t namelen = eq - string;
    int idx = env_find(string, namelen);
    char *dup = env_strdup(string);
    if (!dup) return -1;
    if (idx >= 0) {
        env_array[idx] = dup;
    } else {
        if (env_count >= MAX_ENV_VARS) return -1;
        env_array[env_count++] = dup;
        env_array[env_count] = NULL;
    }
    return 0;
}

int clearenv(void) {
    env_count = 0;
    env_storage_used = 0;
    env_array[0] = NULL;
    return 0;
}
