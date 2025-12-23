#include <libgen.h>
#include <string.h>

char *basename(char *path) {
    static char dot[] = ".";
    char *p;
    
    if (path == NULL || *path == '\0') {
        return dot;
    }
    
    p = path + strlen(path) - 1;
    
    while (p > path && *p == '/') {
        *p-- = '\0';
    }
    
    if (p == path && *p == '/') {
        return path;
    }
    
    while (p > path && *(p - 1) != '/') {
        p--;
    }
    
    return p;
}

char *dirname(char *path) {
    static char dot[] = ".";
    char *p;
    
    if (path == NULL || *path == '\0') {
        return dot;
    }
    
    p = path + strlen(path) - 1;
    
    while (p > path && *p == '/') {
        p--;
    }
    
    while (p > path && *p != '/') {
        p--;
    }
    
    if (p == path) {
        if (*p == '/') {
            path[1] = '\0';
            return path;
        }
        return dot;
    }
    
    while (p > path && *(p - 1) == '/') {
        p--;
    }
    
    if (p == path && *p == '/') {
        path[1] = '\0';
        return path;
    }
    
    *p = '\0';
    return path;
}
