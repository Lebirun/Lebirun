#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>

static int is_pattern(const char *s) {
    while (*s) {
        if (*s == '*' || *s == '?' || *s == '[') {
            return 1;
        }
        s++;
    }
    return 0;
}

static char *dirname_part(const char *path) {
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        return strdup(".");
    }
    if (last_slash == path) {
        return strdup("/");
    }
    size_t len = last_slash - path;
    char *result = malloc(len + 1);
    if (!result) {
        return NULL;
    }
    memcpy(result, path, len);
    result[len] = '\0';
    return result;
}

static const char *basename_part(const char *path) {
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        return path;
    }
    return last_slash + 1;
}

static int add_path(glob_t *pglob, const char *path) {
    size_t count = pglob->gl_pathc + pglob->gl_offs;
    char **new_pathv = realloc(pglob->gl_pathv, (count + 2) * sizeof(char *));
    if (!new_pathv) {
        return GLOB_NOSPACE;
    }
    pglob->gl_pathv = new_pathv;
    pglob->gl_pathv[count] = strdup(path);
    if (!pglob->gl_pathv[count]) {
        return GLOB_NOSPACE;
    }
    pglob->gl_pathc++;
    pglob->gl_pathv[count + 1] = NULL;
    return 0;
}

static int compare_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *pglob) {
    if (!pattern || !pglob) {
        return GLOB_NOMATCH;
    }
    
    if (!(flags & GLOB_APPEND)) {
        pglob->gl_pathc = 0;
        pglob->gl_pathv = NULL;
        pglob->gl_offs = (flags & GLOB_DOOFFS) ? pglob->gl_offs : 0;
    }
    
    if (!is_pattern(pattern)) {
        struct stat st;
        if (stat(pattern, &st) == 0) {
            int ret = add_path(pglob, pattern);
            if (ret != 0) {
                return ret;
            }
        } else if (flags & GLOB_NOCHECK) {
            int ret = add_path(pglob, pattern);
            if (ret != 0) {
                return ret;
            }
        } else {
            return GLOB_NOMATCH;
        }
        return 0;
    }
    
    char *dir = dirname_part(pattern);
    if (!dir) {
        return GLOB_NOSPACE;
    }
    const char *base = basename_part(pattern);
    
    DIR *d = opendir(dir);
    if (!d) {
        if (errfunc && (flags & GLOB_ERR)) {
            errfunc(dir, errno);
        }
        free(dir);
        if (flags & GLOB_NOCHECK) {
            return add_path(pglob, pattern);
        }
        return GLOB_NOMATCH;
    }
    
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.' && !(flags & GLOB_PERIOD)) {
            if (base[0] != '.') {
                continue;
            }
        }
        
        if (fnmatch(base, entry->d_name, 0) == 0) {
            size_t path_len = strlen(dir) + strlen(entry->d_name) + 2;
            char *full_path = malloc(path_len);
            if (!full_path) {
                closedir(d);
                free(dir);
                return GLOB_NOSPACE;
            }
            
            if (strcmp(dir, "/") == 0) {
                snprintf(full_path, path_len, "/%s", entry->d_name);
            } else if (strcmp(dir, ".") == 0) {
                snprintf(full_path, path_len, "%s", entry->d_name);
            } else {
                snprintf(full_path, path_len, "%s/%s", dir, entry->d_name);
            }
            
            int ret = add_path(pglob, full_path);
            free(full_path);
            if (ret != 0) {
                closedir(d);
                free(dir);
                return ret;
            }
            found = 1;
        }
    }
    
    closedir(d);
    free(dir);
    
    if (!found) {
        if (flags & GLOB_NOCHECK) {
            return add_path(pglob, pattern);
        }
        return GLOB_NOMATCH;
    }
    
    if (!(flags & GLOB_NOSORT) && pglob->gl_pathc > 1) {
        qsort(pglob->gl_pathv + pglob->gl_offs, pglob->gl_pathc, sizeof(char *), compare_strings);
    }
    
    return 0;
}

void globfree(glob_t *pglob) {
    if (!pglob || !pglob->gl_pathv) {
        return;
    }
    
    for (size_t i = 0; i < pglob->gl_pathc + pglob->gl_offs; i++) {
        if (pglob->gl_pathv[i]) {
            free(pglob->gl_pathv[i]);
        }
    }
    
    free(pglob->gl_pathv);
    pglob->gl_pathv = NULL;
    pglob->gl_pathc = 0;
}
