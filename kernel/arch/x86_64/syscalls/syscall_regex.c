#include "syscall_defs.h"

extern void *syscall_table[];

#define REG_EXTENDED    1
#define REG_ICASE       2
#define REG_NEWLINE     4
#define REG_NOSUB       8

#define REG_NOTBOL      1
#define REG_NOTEOL      2

#define REG_OK          0
#define REG_NOMATCH     1
#define REG_BADPAT      2
#define REG_ESPACE      12

#define FNM_PATHNAME    0x1
#define FNM_NOESCAPE    0x2
#define FNM_PERIOD      0x4
#define FNM_CASEFOLD    0x10
#define FNM_NOMATCH     1

#define GLOB_ERR        0x01
#define GLOB_MARK       0x02
#define GLOB_NOSORT     0x04
#define GLOB_DOOFFS     0x08
#define GLOB_NOCHECK    0x10
#define GLOB_APPEND     0x20
#define GLOB_NOESCAPE   0x40

#define GLOB_NOSPACE    1
#define GLOB_ABORTED    2
#define GLOB_NOMATCH    3

#define MAX_REGEX_SIZE  512
#define MAX_GROUPS      16

#define NODE_CHAR       1
#define NODE_ANY        2
#define NODE_CLASS      3
#define NODE_ANCHOR_START 4
#define NODE_ANCHOR_END 5
#define NODE_GROUP_START 6
#define NODE_GROUP_END  7
#define NODE_BACKREF    8
#define NODE_WORD_BOUND 9
#define NODE_NONWORD_BOUND 10
#define NODE_SHORTHAND  11
#define NODE_LOOKAHEAD  12
#define NODE_NEG_LOOKAHEAD 13
#define NODE_LOOKBEHIND 14
#define NODE_NEG_LOOKBEHIND 15

#define QUANT_NONE     0
#define QUANT_STAR     1
#define QUANT_PLUS     2
#define QUANT_QUESTION 3
#define QUANT_RANGE    4

#define MAX_NODES      512
#define MAX_CAPTURES   MAX_GROUPS

typedef struct {
    int type;
    int quant;
    int greedy;
    int min_rep;
    int max_rep;
    char ch;
    char class_start[64];
    int class_negate;
    int group_num;
    int alt_next;
    int shorthand_type;
} regex_node_t;

typedef struct {
    int num_nodes;
    int num_groups;
    regex_node_t nodes[MAX_NODES];
} compiled_regex_t;

typedef struct {
    const char *start;
    const char *end;
} capture_t;

typedef long regoff_t;

typedef struct {
    size_t re_nsub;
    char pattern[MAX_REGEX_SIZE];
    int cflags;
    int compiled;
    char compiled_data[4096];
} kernel_regex_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} kernel_regmatch_t;

typedef struct {
    size_t gl_pathc;
    char **gl_pathv;
    size_t gl_offs;
} kernel_glob_t;

static int char_tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static int is_alnum(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int is_digit(int c) {
    return c >= '0' && c <= '9';
}

static int is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int is_word(char c) {
    return is_alnum(c) || c == '_';
}

static int parse_escape(const char **p, regex_node_t *node) {
    char c = **p;
    (*p)++;
    
    switch (c) {
        case 'd':
            node->type = NODE_SHORTHAND;
            node->shorthand_type = 'd';
            return 1;
        case 'D':
            node->type = NODE_SHORTHAND;
            node->shorthand_type = 'D';
            return 1;
        case 'w':
            node->type = NODE_SHORTHAND;
            node->shorthand_type = 'w';
            return 1;
        case 'W':
            node->type = NODE_SHORTHAND;
            node->shorthand_type = 'W';
            return 1;
        case 's':
            node->type = NODE_SHORTHAND;
            node->shorthand_type = 's';
            return 1;
        case 'S':
            node->type = NODE_SHORTHAND;
            node->shorthand_type = 'S';
            return 1;
        case 'b':
            node->type = NODE_WORD_BOUND;
            return 1;
        case 'B':
            node->type = NODE_NONWORD_BOUND;
            return 1;
        case 'n':
            node->type = NODE_CHAR;
            node->ch = '\n';
            return 1;
        case 'r':
            node->type = NODE_CHAR;
            node->ch = '\r';
            return 1;
        case 't':
            node->type = NODE_CHAR;
            node->ch = '\t';
            return 1;
        case '0':
            node->type = NODE_CHAR;
            node->ch = '\0';
            return 1;
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            node->type = NODE_BACKREF;
            node->group_num = c - '0';
            return 1;
        default:
            node->type = NODE_CHAR;
            node->ch = c;
            return 1;
    }
}

static int parse_quantifier(const char **p, regex_node_t *node, int extended) {
    char c = **p;
    
    if (c == '*') {
        (*p)++;
        node->quant = QUANT_STAR;
        node->min_rep = 0;
        node->max_rep = -1;
    } else if (c == '+' && extended) {
        (*p)++;
        node->quant = QUANT_PLUS;
        node->min_rep = 1;
        node->max_rep = -1;
    } else if (c == '?' && extended) {
        (*p)++;
        node->quant = QUANT_QUESTION;
        node->min_rep = 0;
        node->max_rep = 1;
    } else if (c == '{' && extended) {
        int min_val;
        int max_val;
        (*p)++;
        min_val = 0;
        max_val = -1;
        
        while (is_digit(**p)) {
            min_val = min_val * 10 + (**p - '0');
            (*p)++;
        }
        
        if (**p == ',') {
            (*p)++;
            if (is_digit(**p)) {
                max_val = 0;
                while (is_digit(**p)) {
                    max_val = max_val * 10 + (**p - '0');
                    (*p)++;
                }
            }
        } else {
            max_val = min_val;
        }
        
        if (**p == '}') (*p)++;
        
        node->quant = QUANT_RANGE;
        node->min_rep = min_val;
        node->max_rep = max_val;
    } else {
        return 0;
    }
    
    node->greedy = 1;
    if (**p == '?') {
        node->greedy = 0;
        (*p)++;
    }
    
    return 1;
}

static int compile_regex(const char *pattern, compiled_regex_t *compiled, int cflags) {
    const char *p = pattern;
    int extended = cflags & REG_EXTENDED;
    int node_idx = 0;
    int group_count = 0;
    int group_stack[MAX_GROUPS];
    int group_depth = 0;
    int alt_stack[MAX_GROUPS];
    int alt_depth = 0;
    
    (void)alt_stack;
    (void)alt_depth;
    
    while (*p && node_idx < MAX_NODES - 1) {
        regex_node_t *node = &compiled->nodes[node_idx];
        node->quant = QUANT_NONE;
        node->greedy = 1;
        node->min_rep = 1;
        node->max_rep = 1;
        node->alt_next = -1;
        
        if (*p == '^') {
            node->type = NODE_ANCHOR_START;
            p++;
            node_idx++;
        } else if (*p == '$') {
            node->type = NODE_ANCHOR_END;
            p++;
            node_idx++;
        } else if (*p == '.') {
            node->type = NODE_ANY;
            p++;
            parse_quantifier(&p, node, extended);
            node_idx++;
        } else if (*p == '[') {
            int ci;
            node->type = NODE_CLASS;
            p++;
            ci = 0;
            node->class_negate = 0;
            
            if (*p == '^') {
                node->class_negate = 1;
                p++;
            }
            
            if (*p == ']') {
                node->class_start[ci++] = ']';
                p++;
            }
            
            while (*p && *p != ']' && ci < 62) {
                node->class_start[ci++] = *p++;
            }
            node->class_start[ci] = '\0';
            
            if (*p == ']') p++;
            parse_quantifier(&p, node, extended);
            node_idx++;
        } else if (*p == '\\') {
            p++;
            if (!*p) break;
            parse_escape(&p, node);
            if (node->type != NODE_WORD_BOUND && node->type != NODE_NONWORD_BOUND)
                parse_quantifier(&p, node, extended);
            node_idx++;
        } else if (*p == '(' && extended) {
            p++;
            node->type = NODE_GROUP_START;
            
            if (*p == '?' && p[1] == ':') {
                node->group_num = -1;
                p += 2;
            } else if (*p == '?' && p[1] == '=') {
                node->type = NODE_LOOKAHEAD;
                node->group_num = -1;
                p += 2;
            } else if (*p == '?' && p[1] == '!') {
                node->type = NODE_NEG_LOOKAHEAD;
                node->group_num = -1;
                p += 2;
            } else if (*p == '?' && p[1] == '<' && p[2] == '=') {
                node->type = NODE_LOOKBEHIND;
                node->group_num = -1;
                p += 3;
            } else if (*p == '?' && p[1] == '<' && p[2] == '!') {
                node->type = NODE_NEG_LOOKBEHIND;
                node->group_num = -1;
                p += 3;
            } else {
                group_count++;
                node->group_num = group_count;
            }
            
            if (group_depth < MAX_GROUPS)
                group_stack[group_depth++] = node_idx;
            node_idx++;
        } else if (*p == ')' && extended) {
            node->type = NODE_GROUP_END;
            if (group_depth > 0) {
                group_depth--;
                node->group_num = compiled->nodes[group_stack[group_depth]].group_num;
            } else {
                node->group_num = -1;
            }
            parse_quantifier(&p, node, extended);
            p++;
            node_idx++;
        } else if (*p == '|' && extended) {
            p++;
            node->type = NODE_CHAR;
            node->ch = '\0';
            node->alt_next = node_idx + 1;
            node_idx++;
        } else {
            node->type = NODE_CHAR;
            node->ch = *p++;
            parse_quantifier(&p, node, extended);
            node_idx++;
        }
    }
    
    compiled->num_nodes = node_idx;
    compiled->num_groups = group_count;
    
    return REG_OK;
}

static int match_shorthand(char c, int type) {
    switch (type) {
        case 'd': return is_digit(c);
        case 'D': return !is_digit(c);
        case 'w': return is_word(c);
        case 'W': return !is_word(c);
        case 's': return is_space(c);
        case 'S': return !is_space(c);
        default: return 0;
    }
}

static int match_class(const char *class_str, int negate, char c, int icase) {
    const char *p = class_str;
    int match = 0;
    
    while (*p) {
        char c1;
        if (*p == '[' && p[1] == ':') {
            p += 2;
            if (p[0] == 'a' && p[1] == 'l' && p[2] == 'n' && p[3] == 'u' && p[4] == 'm' && p[5] == ':' && p[6] == ']') {
                if (is_alnum(c)) match = 1;
                p += 7;
            } else if (p[0] == 'd' && p[1] == 'i' && p[2] == 'g' && p[3] == 'i' && p[4] == 't' && p[5] == ':' && p[6] == ']') {
                if (is_digit(c)) match = 1;
                p += 7;
            } else if (p[0] == 's' && p[1] == 'p' && p[2] == 'a' && p[3] == 'c' && p[4] == 'e' && p[5] == ':' && p[6] == ']') {
                if (is_space(c)) match = 1;
                p += 7;
            } else if (p[0] == 'a' && p[1] == 'l' && p[2] == 'p' && p[3] == 'h' && p[4] == 'a' && p[5] == ':' && p[6] == ']') {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) match = 1;
                p += 7;
            } else if (p[0] == 'u' && p[1] == 'p' && p[2] == 'p' && p[3] == 'e' && p[4] == 'r' && p[5] == ':' && p[6] == ']') {
                if (c >= 'A' && c <= 'Z') match = 1;
                p += 7;
            } else if (p[0] == 'l' && p[1] == 'o' && p[2] == 'w' && p[3] == 'e' && p[4] == 'r' && p[5] == ':' && p[6] == ']') {
                if (c >= 'a' && c <= 'z') match = 1;
                p += 7;
            } else if (p[0] == 'w' && p[1] == 'o' && p[2] == 'r' && p[3] == 'd' && p[4] == ':' && p[5] == ']') {
                if (is_word(c)) match = 1;
                p += 6;
            } else {
                while (*p && !(*p == ':' && p[1] == ']')) p++;
                if (*p == ':') p += 2;
            }
            continue;
        }
        
        c1 = *p++;
        if (*p == '-' && p[1]) {
            char c2;
            p++;
            c2 = *p++;
            if (icase) {
                if (char_tolower(c) >= char_tolower(c1) && char_tolower(c) <= char_tolower(c2))
                    match = 1;
            } else {
                if (c >= c1 && c <= c2)
                    match = 1;
            }
        } else {
            if (icase) {
                if (char_tolower(c) == char_tolower(c1))
                    match = 1;
            } else {
                if (c == c1)
                    match = 1;
            }
        }
    }
    
    return negate ? !match : match;
}

static int match_node(regex_node_t *node, char c, int cflags) {
    int icase = cflags & REG_ICASE;
    
    switch (node->type) {
        case NODE_CHAR:
            if (icase)
                return char_tolower(c) == char_tolower(node->ch);
            return c == node->ch;
        case NODE_ANY:
            if ((cflags & REG_NEWLINE) && c == '\n')
                return 0;
            return c != '\0';
        case NODE_CLASS:
            return match_class(node->class_start, node->class_negate, c, icase);
        case NODE_SHORTHAND:
            return match_shorthand(c, node->shorthand_type);
        default:
            return 0;
    }
}

static int is_word_boundary(const char *text, const char *pos) {
    int prev_word = (pos > text) && is_word(pos[-1]);
    int curr_word = *pos && is_word(*pos);
    return prev_word != curr_word;
}

static int regex_exec_internal(compiled_regex_t *compiled, const char *text, const char *start,
                               capture_t *captures, int cflags, int eflags);

static int try_match(compiled_regex_t *compiled, int node_start, const char *text, const char *start,
                     const char **end_pos, capture_t *captures, int cflags, int eflags) {
    const char *pos = text;
    int node_idx = node_start;
    capture_t local_captures[MAX_CAPTURES];
    
    for (int i = 0; i < MAX_CAPTURES; i++) {
        local_captures[i].start = captures ? captures[i].start : NULL;
        local_captures[i].end = captures ? captures[i].end : NULL;
    }
    
    while (node_idx < compiled->num_nodes) {
        int min_rep;
        int max_rep;
        int greedy;
        int matched;
        const char *positions[256];
        int pos_count;
        regex_node_t *node = &compiled->nodes[node_idx];
        
        if (node->type == NODE_ANCHOR_START) {
            if (pos != start) {
                if ((cflags & REG_NEWLINE) && pos > start && pos[-1] == '\n') {
                } else if (eflags & REG_NOTBOL) {
                    return 0;
                } else if (pos != start) {
                    return 0;
                }
            }
            node_idx++;
            continue;
        }
        
        if (node->type == NODE_ANCHOR_END) {
            if (*pos != '\0') {
                if ((cflags & REG_NEWLINE) && *pos == '\n') {
                } else if (eflags & REG_NOTEOL) {
                    return 0;
                } else {
                    return 0;
                }
            }
            node_idx++;
            continue;
        }
        
        if (node->type == NODE_WORD_BOUND) {
            if (!is_word_boundary(start, pos))
                return 0;
            node_idx++;
            continue;
        }
        
        if (node->type == NODE_NONWORD_BOUND) {
            if (is_word_boundary(start, pos))
                return 0;
            node_idx++;
            continue;
        }
        
        if (node->type == NODE_GROUP_START) {
            if (node->group_num > 0 && node->group_num < MAX_CAPTURES)
                local_captures[node->group_num].start = pos;
            node_idx++;
            continue;
        }
        
        if (node->type == NODE_GROUP_END) {
            if (node->group_num > 0 && node->group_num < MAX_CAPTURES)
                local_captures[node->group_num].end = pos;
            
            if (node->quant != QUANT_NONE) {
                node_idx++;
                continue;
            }
            node_idx++;
            continue;
        }
        
        if (node->type == NODE_LOOKAHEAD) {
            int depth = 1;
            int la_start = node_idx + 1;
            int la_end = la_start;
            
            const char *dummy;
            while (la_end < compiled->num_nodes && depth > 0) {
                if (compiled->nodes[la_end].type == NODE_GROUP_START)
                    depth++;
                else if (compiled->nodes[la_end].type == NODE_GROUP_END)
                    depth--;
                la_end++;
            }
            
            compiled_regex_t sub_compiled;
            sub_compiled.num_nodes = la_end - la_start;
            sub_compiled.num_groups = compiled->num_groups;
            for (int i = 0; i < sub_compiled.num_nodes && i < MAX_NODES; i++)
                sub_compiled.nodes[i] = compiled->nodes[la_start + i];
            
            if (!try_match(&sub_compiled, 0, pos, start, &dummy, local_captures, cflags, eflags))
                return 0;
            
            node_idx = la_end;
            continue;
        }
        
        if (node->type == NODE_NEG_LOOKAHEAD) {
            int depth = 1;
            int la_start = node_idx + 1;
            int la_end = la_start;
            
            const char *dummy;
            while (la_end < compiled->num_nodes && depth > 0) {
                if (compiled->nodes[la_end].type == NODE_GROUP_START)
                    depth++;
                else if (compiled->nodes[la_end].type == NODE_GROUP_END)
                    depth--;
                la_end++;
            }
            
            compiled_regex_t sub_compiled;
            sub_compiled.num_nodes = la_end - la_start;
            sub_compiled.num_groups = compiled->num_groups;
            for (int i = 0; i < sub_compiled.num_nodes && i < MAX_NODES; i++)
                sub_compiled.nodes[i] = compiled->nodes[la_start + i];
            
            if (try_match(&sub_compiled, 0, pos, start, &dummy, local_captures, cflags, eflags))
                return 0;
            
            node_idx = la_end;
            continue;
        }
        
        if (node->type == NODE_BACKREF) {
            int grp = node->group_num;
            if (grp > 0 && grp < MAX_CAPTURES && local_captures[grp].start && local_captures[grp].end) {
                const char *cap_start = local_captures[grp].start;
                const char *cap_end = local_captures[grp].end;
                int cap_len = cap_end - cap_start;
                int icase = cflags & REG_ICASE;
                
                for (int i = 0; i < cap_len; i++) {
                    if (!*pos) return 0;
                    if (icase) {
                        if (char_tolower(*pos) != char_tolower(cap_start[i]))
                            return 0;
                    } else {
                        if (*pos != cap_start[i])
                            return 0;
                    }
                    pos++;
                }
            }
            node_idx++;
            continue;
        }
        
        if (node->alt_next > 0) {
            const char *alt_end;
            if (try_match(compiled, node_idx + 1, pos, start, &alt_end, local_captures, cflags, eflags)) {
                pos = alt_end;
                while (node_idx < compiled->num_nodes && compiled->nodes[node_idx].alt_next <= 0)
                    node_idx++;
                if (node_idx < compiled->num_nodes)
                    node_idx = compiled->nodes[node_idx].alt_next;
                continue;
            }
            node_idx = node->alt_next;
            continue;
        }
        
        min_rep = node->min_rep;
        max_rep = node->max_rep;
        greedy = node->greedy;
        matched = 0;
        pos_count = 0;
        
        positions[pos_count++] = pos;
        
        while ((max_rep < 0 || matched < max_rep) && *pos && pos_count < 255) {
            if (!match_node(node, *pos, cflags))
                break;
            pos++;
            matched++;
            positions[pos_count++] = pos;
        }
        
        if (matched < min_rep)
            return 0;
        
        if (greedy) {
            for (int i = pos_count - 1; i >= 0; i--) {
                const char *try_pos;
                const char *next_end;
                if (i < min_rep) break;
                try_pos = positions[i];
                
                if (node_idx + 1 >= compiled->num_nodes) {
                    pos = try_pos;
                    if (captures) {
                        for (int j = 0; j < MAX_CAPTURES; j++) {
                            captures[j].start = local_captures[j].start;
                            captures[j].end = local_captures[j].end;
                        }
                    }
                    *end_pos = pos;
                    return 1;
                }
                
                if (try_match(compiled, node_idx + 1, try_pos, start, &next_end, local_captures, cflags, eflags)) {
                    if (captures) {
                        for (int j = 0; j < MAX_CAPTURES; j++) {
                            captures[j].start = local_captures[j].start;
                            captures[j].end = local_captures[j].end;
                        }
                    }
                    *end_pos = next_end;
                    return 1;
                }
            }
            return 0;
        } else {
            for (int i = min_rep; i < pos_count; i++) {
                const char *try_pos = positions[i];
                const char *next_end;
                
                if (node_idx + 1 >= compiled->num_nodes) {
                    pos = try_pos;
                    if (captures) {
                        for (int j = 0; j < MAX_CAPTURES; j++) {
                            captures[j].start = local_captures[j].start;
                            captures[j].end = local_captures[j].end;
                        }
                    }
                    *end_pos = pos;
                    return 1;
                }
                
                if (try_match(compiled, node_idx + 1, try_pos, start, &next_end, local_captures, cflags, eflags)) {
                    if (captures) {
                        for (int j = 0; j < MAX_CAPTURES; j++) {
                            captures[j].start = local_captures[j].start;
                            captures[j].end = local_captures[j].end;
                        }
                    }
                    *end_pos = next_end;
                    return 1;
                }
            }
            return 0;
        }
    }
    
    if (captures) {
        for (int i = 0; i < MAX_CAPTURES; i++) {
            captures[i].start = local_captures[i].start;
            captures[i].end = local_captures[i].end;
        }
    }
    *end_pos = pos;
    return 1;
}

static int regex_exec_internal(compiled_regex_t *compiled, const char *text, const char *start,
                               capture_t *captures, int cflags, int eflags) {
    const char *pos = text;
    const char *end_pos;
    
    int has_anchor = compiled->num_nodes > 0 && compiled->nodes[0].type == NODE_ANCHOR_START;
    
    if (has_anchor) {
        if (try_match(compiled, 0, pos, start, &end_pos, captures, cflags, eflags)) {
            if (captures) {
                captures[0].start = pos;
                captures[0].end = end_pos;
            }
            return 1;
        }
        return 0;
    }
    
    do {
        if (try_match(compiled, 0, pos, start, &end_pos, captures, cflags, eflags)) {
            if (captures) {
                captures[0].start = pos;
                captures[0].end = end_pos;
            }
            return 1;
        }
    } while (*pos++ != '\0');
    
    return 0;
}

static int sys_regcomp(int preg_ptr, const char *pattern_ptr, int cflags) {
    uint64_t preg_addr;
    uint64_t pat_addr;
    const char *pattern;
    int len;
    int result;
    if (!preg_ptr || !pattern_ptr) return -EINVAL;
    
    preg_addr = (uint64_t)preg_ptr;
    pat_addr = (uint64_t)(uintptr_t)pattern_ptr;
    
    if (preg_addr >= KERNEL_VMA || pat_addr >= KERNEL_VMA) return -EFAULT;
    
    kernel_regex_t *preg = (kernel_regex_t *)preg_addr;
    pattern = (const char *)pat_addr;
    
    len = 0;
    while (pattern[len] && len < MAX_REGEX_SIZE - 1) len++;
    
    for (int i = 0; i < len; i++)
        preg->pattern[i] = pattern[i];
    preg->pattern[len] = '\0';
    preg->cflags = cflags;
    
    compiled_regex_t *compiled = (compiled_regex_t *)preg->compiled_data;
    result = compile_regex(pattern, compiled, cflags);
    
    if (result != REG_OK) {
        preg->compiled = 0;
        return result;
    }
    
    preg->compiled = 1;
    preg->re_nsub = compiled->num_groups;
    
    return REG_OK;
}

static int sys_regexec(int preg_ptr, const char *string_ptr, int nmatch_arg, int pmatch_arg) {
    uint64_t preg_addr;
    uint64_t str_addr;
    const char *string;
    size_t nmatch;
    if (!preg_ptr || !string_ptr) return REG_NOMATCH;
    
    preg_addr = (uint64_t)preg_ptr;
    str_addr = (uint64_t)(uintptr_t)string_ptr;
    
    if (preg_addr >= KERNEL_VMA || str_addr >= KERNEL_VMA) return REG_NOMATCH;
    
    kernel_regex_t *preg = (kernel_regex_t *)preg_addr;
    string = (const char *)str_addr;
    
    if (!preg->compiled) return REG_BADPAT;
    
    nmatch = (size_t)nmatch_arg;
    kernel_regmatch_t *pmatch = pmatch_arg ? (kernel_regmatch_t *)(uintptr_t)pmatch_arg : NULL;
    
    compiled_regex_t *compiled = (compiled_regex_t *)preg->compiled_data;
    capture_t captures[MAX_CAPTURES];
    
    for (int i = 0; i < MAX_CAPTURES; i++) {
        captures[i].start = NULL;
        captures[i].end = NULL;
    }
    
    if (!regex_exec_internal(compiled, string, string, captures, preg->cflags, 0))
        return REG_NOMATCH;
    
    if (pmatch && nmatch > 0) {
        for (size_t i = 0; i < nmatch && i < MAX_CAPTURES; i++) {
            if (captures[i].start && captures[i].end) {
                pmatch[i].rm_so = captures[i].start - string;
                pmatch[i].rm_eo = captures[i].end - string;
            } else {
                pmatch[i].rm_so = -1;
                pmatch[i].rm_eo = -1;
            }
        }
    }
    
    return REG_OK;
}

static int sys_regfree(int preg_ptr, const char *unused1, int unused2) {
    uint64_t preg_addr;
    (void)unused1; (void)unused2;
    
    if (!preg_ptr) return 0;
    
    preg_addr = (uint64_t)preg_ptr;
    if (preg_addr >= KERNEL_VMA) return -EFAULT;
    
    kernel_regex_t *preg = (kernel_regex_t *)preg_addr;
    preg->compiled = 0;
    preg->pattern[0] = '\0';
    preg->re_nsub = 0;
    
    return 0;
}

static int sys_regerror(int errcode, int preg_arg, int errbuf_arg, int errbuf_size_arg) {
    char *errbuf;
    size_t errbuf_size;
    const char *msg;
    int len;
    (void)preg_arg;
    
    errbuf = errbuf_arg ? (char *)(uintptr_t)errbuf_arg : NULL;
    errbuf_size = (size_t)errbuf_size_arg;
    
    switch (errcode) {
        case REG_OK: msg = "Success"; break;
        case REG_NOMATCH: msg = "No match"; break;
        case REG_BADPAT: msg = "Invalid pattern"; break;
        case REG_ESPACE: msg = "Out of memory"; break;
        default: msg = "Unknown error"; break;
    }
    
    len = 0;
    while (msg[len]) len++;
    
    if (errbuf && errbuf_size > 0) {
        int copy = len < (int)errbuf_size - 1 ? len : (int)errbuf_size - 1;
        for (int i = 0; i < copy; i++)
            errbuf[i] = msg[i];
        errbuf[copy] = '\0';
    }
    
    return len + 1;
}

static int fnmatch_internal(const char *pattern, const char *string, int flags) {
    const char *p = pattern;
    const char *s = string;
    int icase = flags & FNM_CASEFOLD;
    int pathname = flags & FNM_PATHNAME;
    int period = flags & FNM_PERIOD;
    int noescape = flags & FNM_NOESCAPE;
    
    while (*p) {
        switch (*p) {
            case '?':
                if (*s == '\0') return FNM_NOMATCH;
                if (pathname && *s == '/') return FNM_NOMATCH;
                if (period && *s == '.' && (s == string || (pathname && s[-1] == '/')))
                    return FNM_NOMATCH;
                s++;
                p++;
                break;
                
            case '*': {
                while (*p == '*') p++;
                
                if (*p == '\0') {
                    if (pathname) {
                        while (*s) {
                            if (*s == '/') return FNM_NOMATCH;
                            s++;
                        }
                    }
                    return 0;
                }
                
                while (*s) {
                    if (fnmatch_internal(p, s, flags & ~FNM_PERIOD) == 0)
                        return 0;
                    if (pathname && *s == '/') break;
                    s++;
                }
                return FNM_NOMATCH;
            }
            
            case '[': {
                int negate;
                int match;
                if (*s == '\0') return FNM_NOMATCH;
                if (pathname && *s == '/') return FNM_NOMATCH;
                if (period && *s == '.' && (s == string || (pathname && s[-1] == '/')))
                    return FNM_NOMATCH;
                
                p++;
                negate = 0;
                if (*p == '!' || *p == '^') {
                    negate = 1;
                    p++;
                }
                
                match = 0;
                while (*p && *p != ']') {
                    char c1 = *p++;
                    if (c1 == '\\' && !noescape && *p)
                        c1 = *p++;
                    
                    if (*p == '-' && p[1] && p[1] != ']') {
                        char c2;
                        char sc;
                        char lc1;
                        char lc2;
                        p++;
                        c2 = *p++;
                        if (c2 == '\\' && !noescape && *p)
                            c2 = *p++;
                        
                        sc = icase ? char_tolower(*s) : *s;
                        lc1 = icase ? char_tolower(c1) : c1;
                        lc2 = icase ? char_tolower(c2) : c2;
                        if (sc >= lc1 && sc <= lc2)
                            match = 1;
                    } else {
                        if (icase ? char_tolower(*s) == char_tolower(c1) : *s == c1)
                            match = 1;
                    }
                }
                
                if (*p == ']') p++;
                
                if (negate ? match : !match)
                    return FNM_NOMATCH;
                s++;
                break;
            }
            
            case '\\':
                if (!noescape && p[1]) p++;
                __attribute__((fallthrough));
            default:
                if (*s == '\0') return FNM_NOMATCH;
                if (icase) {
                    if (char_tolower(*p) != char_tolower(*s))
                        return FNM_NOMATCH;
                } else {
                    if (*p != *s)
                        return FNM_NOMATCH;
                }
                p++;
                s++;
                break;
        }
    }
    
    return *s == '\0' ? 0 : FNM_NOMATCH;
}

static int sys_fnmatch(int pattern_ptr, const char *string_ptr, int flags) {
    uint64_t pat_addr;
    uint64_t str_addr;
    const char *pattern;
    const char *string;
    if (!pattern_ptr || !string_ptr) return FNM_NOMATCH;
    
    pat_addr = (uint64_t)pattern_ptr;
    str_addr = (uint64_t)(uintptr_t)string_ptr;
    
    if (pat_addr >= KERNEL_VMA || str_addr >= KERNEL_VMA) return FNM_NOMATCH;
    
    pattern = (const char *)pat_addr;
    string = (const char *)str_addr;
    
    return fnmatch_internal(pattern, string, flags);
}

static int glob_match_pattern(const char *pattern, const char *name) {
    return fnmatch_internal(pattern, name, 0) == 0;
}

static int sys_glob(int pattern_ptr, const char *flags_errfunc_ptr, int pglob_ptr) {
    uint64_t pat_addr;
    uint64_t glob_addr;
    const char *pattern;
    int flags;
    char dir_path[256] = "/";
    char file_pattern[256];
    int last_slash;
    int i;
    char **results;
    size_t count;
    size_t capacity;
    uint64_t idx;
    char **pathv;
    if (!pattern_ptr || !pglob_ptr) return GLOB_NOMATCH;
    
    pat_addr = (uint64_t)pattern_ptr;
    glob_addr = (uint64_t)pglob_ptr;
    
    if (pat_addr >= KERNEL_VMA || glob_addr >= KERNEL_VMA) return GLOB_NOMATCH;
    
    pattern = (const char *)pat_addr;
    kernel_glob_t *pglob = (kernel_glob_t *)glob_addr;
    flags = (int)(uintptr_t)flags_errfunc_ptr;
    
    if (!(flags & GLOB_APPEND)) {
        pglob->gl_pathc = 0;
        pglob->gl_pathv = NULL;
    }
    
    last_slash = -1;
    
    i = 0;
    while (pattern[i]) {
        if (pattern[i] == '/') last_slash = i;
        i++;
    }
    
    if (last_slash >= 0) {
        int k;
        for (int j = 0; j < last_slash && j < 255; j++)
            dir_path[j] = pattern[j];
        dir_path[last_slash] = '\0';
        if (last_slash == 0) {
            dir_path[0] = '/';
            dir_path[1] = '\0';
        }
        
        k = 0;
        for (int j = last_slash + 1; pattern[j] && k < 255; j++, k++)
            file_pattern[k] = pattern[j];
        file_pattern[k] = '\0';
    } else {
        dir_path[0] = '.';
        dir_path[1] = '\0';
        for (int j = 0; pattern[j] && j < 255; j++)
            file_pattern[j] = pattern[j];
        file_pattern[i] = '\0';
    }
    
    vfs_node_t *dir = vfs_namei(dir_path);
    if (!dir) {
        if (flags & GLOB_NOCHECK) {
            int plen;
            char **pathv;
            pglob->gl_pathc = 1;
            plen = 0;
            while (pattern[plen]) plen++;
            pathv = (char **)kmalloc(2 * sizeof(char *));
            if (!pathv) return GLOB_NOSPACE;
            pathv[0] = (char *)kmalloc(plen + 1);
            if (!pathv[0]) {
                kfree(pathv);
                return GLOB_NOSPACE;
            }
            for (int j = 0; j <= plen; j++)
                pathv[0][j] = pattern[j];
            pathv[1] = NULL;
            pglob->gl_pathv = pathv;
            return 0;
        }
        return GLOB_NOMATCH;
    }
    
    results = NULL;
    count = 0;
    capacity = 0;
    
    idx = 0;
    dirent_t *dirent;
    
    while ((dirent = vfs_readdir(dir, idx)) != NULL) {
        if (dirent->name[0] == '\0') break;
        
        if (glob_match_pattern(file_pattern, dirent->name)) {
            int dlen;
            int nlen;
            int need_slash;
            int total;
            char *path;
            int pos;
            if (count >= capacity) {
                size_t new_cap = capacity ? capacity * 2 : 8;
                char **new_results = (char **)kmalloc(new_cap * sizeof(char *));
                if (!new_results) {
                    for (size_t j = 0; j < count; j++)
                        kfree(results[j]);
                    if (results) kfree(results);
                    return GLOB_NOSPACE;
                }
                for (size_t j = 0; j < count; j++)
                    new_results[j] = results[j];
                if (results) kfree(results);
                results = new_results;
                capacity = new_cap;
            }
            
            dlen = 0;
            while (dir_path[dlen]) dlen++;
            nlen = 0;
            while (dirent->name[nlen]) nlen++;
            
            need_slash = (dlen > 0 && dir_path[dlen-1] != '/') ? 1 : 0;
            total = dlen + need_slash + nlen + 1;
            
            path = (char *)kmalloc(total);
            if (!path) {
                for (size_t j = 0; j < count; j++)
                    kfree(results[j]);
                if (results) kfree(results);
                return GLOB_NOSPACE;
            }
            
            pos = 0;
            for (int j = 0; j < dlen; j++)
                path[pos++] = dir_path[j];
            if (need_slash)
                path[pos++] = '/';
            for (int j = 0; j < nlen; j++)
                path[pos++] = dirent->name[j];
            path[pos] = '\0';
            
            results[count++] = path;
        }
        idx++;
    }
    
    if (count == 0) {
        if (results) kfree(results);
        if (flags & GLOB_NOCHECK) {
            int plen;
            char **pathv;
            pglob->gl_pathc = 1;
            plen = 0;
            while (pattern[plen]) plen++;
            pathv = (char **)kmalloc(2 * sizeof(char *));
            if (!pathv) return GLOB_NOSPACE;
            pathv[0] = (char *)kmalloc(plen + 1);
            if (!pathv[0]) {
                kfree(pathv);
                return GLOB_NOSPACE;
            }
            for (int j = 0; j <= plen; j++)
                pathv[0][j] = pattern[j];
            pathv[1] = NULL;
            pglob->gl_pathv = pathv;
            return 0;
        }
        return GLOB_NOMATCH;
    }
    
    pathv = (char **)kmalloc((count + 1) * sizeof(char *));
    if (!pathv) {
        for (size_t j = 0; j < count; j++)
            kfree(results[j]);
        kfree(results);
        return GLOB_NOSPACE;
    }
    
    for (size_t j = 0; j < count; j++)
        pathv[j] = results[j];
    pathv[count] = NULL;
    
    kfree(results);
    
    pglob->gl_pathc = count;
    pglob->gl_pathv = pathv;
    
    return 0;
}

static int sys_globfree(int pglob_ptr, const char *unused1, int unused2) {
    uint64_t glob_addr;
    (void)unused1; (void)unused2;
    
    if (!pglob_ptr) return 0;
    
    glob_addr = (uint64_t)pglob_ptr;
    if (glob_addr >= KERNEL_VMA) return -EFAULT;
    
    kernel_glob_t *pglob = (kernel_glob_t *)glob_addr;
    
    if (pglob->gl_pathv) {
        for (size_t i = 0; i < pglob->gl_pathc; i++) {
            if (pglob->gl_pathv[i])
                kfree(pglob->gl_pathv[i]);
        }
        kfree(pglob->gl_pathv);
        pglob->gl_pathv = NULL;
    }
    pglob->gl_pathc = 0;
    
    return 0;
}

static int skip_whitespace(const char **str) {
    int count = 0;
    while (**str && is_space(**str)) {
        (*str)++;
        count++;
    }
    return count;
}

static int scan_int(const char **str, int *out, int width) {
    int neg;
    long val;
    int chars;
    skip_whitespace(str);
    
    if (!**str) return 0;
    
    neg = 0;
    if (**str == '-') {
        neg = 1;
        (*str)++;
        width--;
    } else if (**str == '+') {
        (*str)++;
        width--;
    }
    
    if (!is_digit(**str)) return 0;
    
    val = 0;
    chars = 0;
    while (is_digit(**str) && (width <= 0 || chars < width)) {
        val = val * 10 + (**str - '0');
        (*str)++;
        chars++;
    }
    
    if (neg) val = -val;
    *out = (int)val;
    return 1;
}

static int scan_uint(const char **str, unsigned int *out, int width, int base) {
    unsigned long val;
    int chars;
    int got_digit;
    skip_whitespace(str);
    
    if (!**str) return 0;
    
    if (base == 0) {
        if (**str == '0') {
            (*str)++;
            if (**str == 'x' || **str == 'X') {
                (*str)++;
                base = 16;
            } else {
                base = 8;
            }
        } else {
            base = 10;
        }
    }
    
    val = 0;
    chars = 0;
    got_digit = 0;
    
    while ((width <= 0 || chars < width) && **str) {
        int digit = -1;
        if (**str >= '0' && **str <= '9')
            digit = **str - '0';
        else if (**str >= 'a' && **str <= 'f')
            digit = **str - 'a' + 10;
        else if (**str >= 'A' && **str <= 'F')
            digit = **str - 'A' + 10;
        
        if (digit < 0 || digit >= base) break;
        
        val = val * base + digit;
        (*str)++;
        chars++;
        got_digit = 1;
    }
    
    if (!got_digit) return 0;
    *out = (unsigned int)val;
    return 1;
}

static int scan_string(const char **str, char *out, int width) {
    int chars;
    skip_whitespace(str);
    
    if (!**str) return 0;
    
    chars = 0;
    while (**str && !is_space(**str) && (width <= 0 || chars < width)) {
        if (out) *out++ = **str;
        (*str)++;
        chars++;
    }
    
    if (out) *out = '\0';
    return chars > 0 ? 1 : 0;
}

static int scan_char(const char **str, char *out, int width) {
    int chars;
    if (width <= 0) width = 1;
    
    chars = 0;
    while (chars < width && **str) {
        if (out) *out++ = **str;
        (*str)++;
        chars++;
    }
    
    return chars > 0 ? 1 : 0;
}

static int scan_scanset(const char **str, char *out, const char *set, int negate, int width) {
    int chars = 0;
    
    while (**str && (width <= 0 || chars < width)) {
        int in_set = 0;
        const char *s = set;
        while (*s) {
            if (*s == **str) {
                in_set = 1;
                break;
            }
            s++;
        }
        
        if (negate ? in_set : !in_set) break;
        
        if (out) *out++ = **str;
        (*str)++;
        chars++;
    }
    
    if (out) *out = '\0';
    return chars > 0 ? 1 : 0;
}

static int vsscanf_internal(const char *str, const char *format, uint64_t *args) {
    const char *s = str;
    const char *f = format;
    int count = 0;
    int arg_idx = 0;
    
    while (*f) {
        int suppress;
        int width;
        int length;
        char spec;
        if (is_space(*f)) {
            skip_whitespace(&s);
            while (is_space(*f)) f++;
            continue;
        }
        
        if (*f != '%') {
            if (*s != *f) return count;
            s++;
            f++;
            continue;
        }
        
        f++;
        
        if (*f == '%') {
            if (*s != '%') return count;
            s++;
            f++;
            continue;
        }
        
        suppress = 0;
        if (*f == '*') {
            suppress = 1;
            f++;
        }
        
        width = 0;
        while (is_digit(*f)) {
            width = width * 10 + (*f - '0');
            f++;
        }
        
        length = 0;
        if (*f == 'h') {
            length = 'h';
            f++;
            if (*f == 'h') { length = 'H'; f++; }
        } else if (*f == 'l') {
            length = 'l';
            f++;
            if (*f == 'l') { length = 'L'; f++; }
        } else if (*f == 'L' || *f == 'z' || *f == 't' || *f == 'j') {
            length = *f++;
        }
        
        spec = *f++;
        
        switch (spec) {
            case 'd':
            case 'i': {
                int val;
                if (!scan_int(&s, &val, width)) return count;
                if (!suppress) {
                    *(int *)args[arg_idx++] = val;
                    count++;
                }
                break;
            }
            
            case 'u': {
                unsigned int val;
                if (!scan_uint(&s, &val, width, 10)) return count;
                if (!suppress) {
                    *(unsigned int *)args[arg_idx++] = val;
                    count++;
                }
                break;
            }
            
            case 'x':
            case 'X': {
                unsigned int val;
                if (!scan_uint(&s, &val, width, 16)) return count;
                if (!suppress) {
                    *(unsigned int *)args[arg_idx++] = val;
                    count++;
                }
                break;
            }
            
            case 'o': {
                unsigned int val;
                if (!scan_uint(&s, &val, width, 8)) return count;
                if (!suppress) {
                    *(unsigned int *)args[arg_idx++] = val;
                    count++;
                }
                break;
            }
            
            case 's': {
                char *out = suppress ? NULL : (char *)args[arg_idx++];
                if (!scan_string(&s, out, width)) return count;
                if (!suppress) count++;
                break;
            }
            
            case 'c': {
                char *out = suppress ? NULL : (char *)args[arg_idx++];
                if (!scan_char(&s, out, width)) return count;
                if (!suppress) count++;
                break;
            }
            
            case '[': {
                char set[128];
                int set_idx = 0;
                int negate = 0;
                
                char *out;
                if (*f == '^') {
                    negate = 1;
                    f++;
                }
                
                if (*f == ']') {
                    set[set_idx++] = ']';
                    f++;
                }
                
                while (*f && *f != ']' && set_idx < 126) {
                    set[set_idx++] = *f++;
                }
                set[set_idx] = '\0';
                
                if (*f == ']') f++;
                
                out = suppress ? NULL : (char *)args[arg_idx++];
                if (!scan_scanset(&s, out, set, negate, width)) return count;
                if (!suppress) count++;
                break;
            }
            
            case 'n': {
                if (!suppress) {
                    *(int *)args[arg_idx++] = (int)(s - str);
                }
                break;
            }
            
            case 'p': {
                unsigned int val;
                if (!scan_uint(&s, &val, width, 16)) return count;
                if (!suppress) {
                    *(void **)args[arg_idx++] = (void *)(uintptr_t)val;
                    count++;
                }
                break;
            }
            
            default:
                return count;
        }
        
        (void)length;
    }
    
    return count;
}

static int sys_sscanf(int str_ptr, const char *format_ptr, int args_ptr) {
    uint64_t str_addr;
    uint64_t fmt_addr;
    uint64_t arg_addr;
    const char *str;
    const char *format;
    uint64_t *args;
    if (!str_ptr || !format_ptr) return -1;
    
    str_addr = (uint64_t)str_ptr;
    fmt_addr = (uint64_t)(uintptr_t)format_ptr;
    arg_addr = (uint64_t)args_ptr;
    
    if (str_addr >= KERNEL_VMA || fmt_addr >= KERNEL_VMA) return -1;
    
    str = (const char *)str_addr;
    format = (const char *)fmt_addr;
    args = arg_addr ? (uint64_t *)arg_addr : NULL;
    
    if (!args) return -1;
    
    return vsscanf_internal(str, format, args);
}

static int sys_scanf_getchar(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;

    return keyboard_getchar_nb();
}

static int sys_regsub(int preg_ptr, const char *string_ptr, int replacement_output) {
    uint64_t preg_addr;
    uint64_t str_addr;
    uint64_t repl_addr;
    uint64_t out_addr;
    const char *string;
    const char *replacement;
    char *output;
    int out_idx;
    const char *s;
    const char *r;
    if (!preg_ptr || !string_ptr) return -EINVAL;
    
    preg_addr = (uint64_t)preg_ptr;
    str_addr = (uint64_t)(uintptr_t)string_ptr;
    repl_addr = (replacement_output >> 16) ? (replacement_output & 0xFFFF0000) : 0;
    out_addr = replacement_output & 0xFFFF;
    
    if (preg_addr >= KERNEL_VMA || str_addr >= KERNEL_VMA) return -EFAULT;
    
    kernel_regex_t *preg = (kernel_regex_t *)preg_addr;
    string = (const char *)str_addr;
    replacement = repl_addr ? (const char *)repl_addr : "";
    output = out_addr ? (char *)out_addr : NULL;
    
    if (!preg->compiled) return -EINVAL;
    
    compiled_regex_t *compiled = (compiled_regex_t *)preg->compiled_data;
    capture_t captures[MAX_CAPTURES];
    
    for (int i = 0; i < MAX_CAPTURES; i++) {
        captures[i].start = NULL;
        captures[i].end = NULL;
    }
    
    if (!regex_exec_internal(compiled, string, string, captures, preg->cflags, 0)) {
        int len;
        if (output) {
            int i = 0;
            while (string[i]) {
                output[i] = string[i];
                i++;
            }
            output[i] = '\0';
            return i;
        }
        len = 0;
        while (string[len]) len++;
        return len;
    }
    
    out_idx = 0;
    s = string;
    
    while (s < captures[0].start) {
        if (output) output[out_idx] = *s;
        out_idx++;
        s++;
    }
    
    r = replacement;
    while (*r) {
        if (*r == '\\' && r[1] >= '0' && r[1] <= '9') {
            int grp = r[1] - '0';
            r += 2;
            if (grp < MAX_CAPTURES && captures[grp].start && captures[grp].end) {
                const char *cs = captures[grp].start;
                while (cs < captures[grp].end) {
                    if (output) output[out_idx] = *cs;
                    out_idx++;
                    cs++;
                }
            }
        } else if (*r == '$' && r[1] >= '0' && r[1] <= '9') {
            int grp = r[1] - '0';
            r += 2;
            if (grp < MAX_CAPTURES && captures[grp].start && captures[grp].end) {
                const char *cs = captures[grp].start;
                while (cs < captures[grp].end) {
                    if (output) output[out_idx] = *cs;
                    out_idx++;
                    cs++;
                }
            }
        } else if (*r == '&') {
            r++;
            if (captures[0].start && captures[0].end) {
                const char *cs = captures[0].start;
                while (cs < captures[0].end) {
                    if (output) output[out_idx] = *cs;
                    out_idx++;
                    cs++;
                }
            }
        } else {
            if (output) output[out_idx] = *r;
            out_idx++;
            r++;
        }
    }
    
    s = captures[0].end;
    while (*s) {
        if (output) output[out_idx] = *s;
        out_idx++;
        s++;
    }
    
    if (output) output[out_idx] = '\0';
    return out_idx;
}

static int sys_regexec_ex(int preg_ptr, const char *string_ptr, int pmatch_ptr) {
    uint64_t preg_addr;
    uint64_t str_addr;
    uint64_t pm_addr;
    const char *string;
    if (!preg_ptr || !string_ptr) return REG_NOMATCH;
    
    preg_addr = (uint64_t)preg_ptr;
    str_addr = (uint64_t)(uintptr_t)string_ptr;
    pm_addr = (uint64_t)pmatch_ptr;
    
    if (preg_addr >= KERNEL_VMA || str_addr >= KERNEL_VMA) return REG_NOMATCH;
    
    kernel_regex_t *preg = (kernel_regex_t *)preg_addr;
    string = (const char *)str_addr;
    kernel_regmatch_t *pmatch = pm_addr ? (kernel_regmatch_t *)pm_addr : NULL;
    
    if (!preg->compiled) return REG_BADPAT;
    
    compiled_regex_t *compiled = (compiled_regex_t *)preg->compiled_data;
    capture_t captures[MAX_CAPTURES];
    
    for (int i = 0; i < MAX_CAPTURES; i++) {
        captures[i].start = NULL;
        captures[i].end = NULL;
    }
    
    if (!regex_exec_internal(compiled, string, string, captures, preg->cflags, 0))
        return REG_NOMATCH;
    
    if (pmatch) {
        for (int i = 0; i < MAX_CAPTURES; i++) {
            if (captures[i].start && captures[i].end) {
                pmatch[i].rm_so = captures[i].start - string;
                pmatch[i].rm_eo = captures[i].end - string;
            } else {
                pmatch[i].rm_so = -1;
                pmatch[i].rm_eo = -1;
            }
        }
    }
    
    return REG_OK;
}

void syscalls_regex_init(void) {
    syscall_table[SYSCALL_REGCOMP] = sys_regcomp;
    syscall_table[SYSCALL_REGEXEC] = sys_regexec;
    syscall_table[SYSCALL_REGFREE] = sys_regfree;
    syscall_table[SYSCALL_REGERROR] = sys_regerror;
    syscall_table[SYSCALL_FNMATCH] = sys_fnmatch;
    syscall_table[SYSCALL_GLOB] = sys_glob;
    syscall_table[SYSCALL_GLOBFREE] = sys_globfree;
    syscall_table[SYSCALL_SSCANF] = sys_sscanf;
    syscall_table[SYSCALL_SCANF_GETCHAR] = sys_scanf_getchar;
    syscall_table[SYSCALL_REGSUB] = sys_regsub;
    syscall_table[SYSCALL_REGEXEC_EX] = sys_regexec_ex;
}
