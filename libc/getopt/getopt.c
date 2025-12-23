#include <getopt.h>
#include <string.h>
#include <stddef.h>

char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;

static int optpos = 1;

int getopt(int argc, char * const argv[], const char *optstring) {
    if (optind >= argc || !argv[optind]) return -1;
    
    const char *arg = argv[optind];
    
    if (arg[0] != '-' || arg[1] == '\0') return -1;
    
    if (arg[1] == '-' && arg[2] == '\0') {
        optind++;
        return -1;
    }
    
    char c = arg[optpos];
    const char *match = strchr(optstring, c);
    
    if (!match || c == ':') {
        optopt = c;
        if (optstring[0] != ':' && opterr) {
        }
        if (!arg[++optpos]) {
            optind++;
            optpos = 1;
        }
        return '?';
    }
    
    if (match[1] == ':') {
        if (arg[optpos + 1]) {
            optarg = (char *)&arg[optpos + 1];
            optind++;
            optpos = 1;
        } else if (optind + 1 < argc) {
            optarg = argv[++optind];
            optind++;
            optpos = 1;
        } else {
            optopt = c;
            if (optstring[0] != ':' && opterr) {
            }
            if (optstring[0] == ':') return ':';
            return '?';
        }
    } else {
        if (!arg[++optpos]) {
            optind++;
            optpos = 1;
        }
    }
    
    return c;
}

int getopt_long(int argc, char * const argv[], const char *optstring,
                const struct option *longopts, int *longindex) {
    if (optind >= argc || !argv[optind]) return -1;
    
    const char *arg = argv[optind];
    
    if (arg[0] != '-') return -1;
    
    if (arg[1] == '-' && arg[2]) {
        const char *name = arg + 2;
        const char *eq = strchr(name, '=');
        size_t namelen = eq ? (size_t)(eq - name) : strlen(name);
        
        for (int i = 0; longopts[i].name; i++) {
            if (strlen(longopts[i].name) == namelen && !memcmp(longopts[i].name, name, namelen)) {
                if (longindex) *longindex = i;
                optind++;
                
                if (longopts[i].has_arg == required_argument || longopts[i].has_arg == optional_argument) {
                    if (eq) {
                        optarg = (char *)eq + 1;
                    } else if (longopts[i].has_arg == required_argument && optind < argc) {
                        optarg = argv[optind++];
                    } else if (longopts[i].has_arg == required_argument) {
                        return '?';
                    }
                }
                
                if (longopts[i].flag) {
                    *longopts[i].flag = longopts[i].val;
                    return 0;
                }
                return longopts[i].val;
            }
        }
        return '?';
    }
    
    return getopt(argc, argv, optstring);
}

int getopt_long_only(int argc, char * const argv[], const char *optstring,
                     const struct option *longopts, int *longindex) {
    return getopt_long(argc, argv, optstring, longopts, longindex);
}
