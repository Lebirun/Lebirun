#include "syscall_defs.h"
#include <lebirun/spinlock.h>


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
#define REGEX_COMPILED_MAGIC 0x4C45425245475831ULL
#define LEGACY_MATCH_COUNT 16

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

typedef struct {
    int type;
    int quant;
    int greedy;
    int min_rep;
    int max_rep;
    char ch;
    char *class_start;
    int class_negate;
    int group_num;
    int alt_next;
    int shorthand_type;
} regex_node_t;

typedef struct {
    uint64_t magic;
    size_t num_nodes;
    size_t num_groups;
    size_t node_capacity;
    regex_node_t *nodes;
} compiled_regex_t;

typedef struct {
    const char *start;
    const char *end;
} capture_t;

typedef long regoff_t;

typedef struct {
    size_t re_nsub;
    void *opaque;
    void *padding[4];
    size_t nsub2;
    char padding2;
} kernel_regex_t;

typedef struct regex_handle {
    uint64_t token;
    uint64_t owner_cr3;
    int cflags;
    uint64_t users;
    int removed;
    compiled_regex_t compiled;
    struct regex_handle *next;
} regex_handle_t;

static regex_handle_t *regex_handles;
static uint64_t regex_next_token = 1;
static spinlock_t regex_handles_lock = {0};

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

static void compiled_regex_release(compiled_regex_t *compiled) {
    size_t i;

    if (!compiled) return;
    for (i = 0; i < compiled->num_nodes; i++) {
        if (compiled->nodes[i].class_start)
            kfree(compiled->nodes[i].class_start);
    }
    if (compiled->nodes) kfree(compiled->nodes);
    compiled->nodes = NULL;
    compiled->num_nodes = 0;
    compiled->num_groups = 0;
    compiled->node_capacity = 0;
}

static int compiled_regex_copy(compiled_regex_t *dest,
                               const compiled_regex_t *source) {
    size_t i;
    size_t length;

    if (!dest || !source || source->magic != REGEX_COMPILED_MAGIC)
        return -1;
    memset(dest, 0, sizeof(*dest));
    if (source->num_nodes > SIZE_MAX / sizeof(regex_node_t)) return -1;
    if (source->num_nodes) {
        dest->nodes = (regex_node_t *)kmalloc(
            source->num_nodes * sizeof(regex_node_t));
        if (!dest->nodes) return -1;
        memcpy(dest->nodes, source->nodes,
               source->num_nodes * sizeof(regex_node_t));
        dest->num_nodes = source->num_nodes;
        dest->node_capacity = source->num_nodes;
        for (i = 0; i < dest->num_nodes; i++)
            dest->nodes[i].class_start = NULL;
        for (i = 0; i < dest->num_nodes; i++) {
            if (!source->nodes[i].class_start) continue;
            length = strlen(source->nodes[i].class_start);
            if (length == SIZE_MAX) {
                compiled_regex_release(dest);
                return -1;
            }
            dest->nodes[i].class_start = (char *)kmalloc(length + 1);
            if (!dest->nodes[i].class_start) {
                compiled_regex_release(dest);
                return -1;
            }
            memcpy(dest->nodes[i].class_start,
                   source->nodes[i].class_start, length + 1);
        }
    }
    dest->num_groups = source->num_groups;
    dest->magic = REGEX_COMPILED_MAGIC;
    return 0;
}

static uint64_t regex_owner_cr3(void) {
    if (!current_task) return 0;
    return current_task->pml4_phys;
}

static regex_handle_t *regex_handle_acquire(uint64_t token) {
    regex_handle_t *handle;
    uint64_t owner_cr3;

    owner_cr3 = regex_owner_cr3();
    spin_lock(&regex_handles_lock);
    handle = regex_handles;
    while (handle) {
        if (!handle->removed && handle->token == token &&
            handle->owner_cr3 == owner_cr3) {
            handle->users++;
            spin_unlock(&regex_handles_lock);
            return handle;
        }
        handle = handle->next;
    }
    spin_unlock(&regex_handles_lock);
    return NULL;
}

static uint64_t regex_allocate_token_locked(void) {
    uint64_t token;

    token = regex_next_token++;
    if (token == 0) token = regex_next_token++;
    return token;
}

static regex_handle_t *regex_handle_create(void) {
    regex_handle_t *handle;

    handle = (regex_handle_t *)kmalloc(sizeof(regex_handle_t));
    if (!handle) return NULL;
    memset(handle, 0, sizeof(regex_handle_t));
    handle->users = 1;
    handle->owner_cr3 = regex_owner_cr3();
    return handle;
}

static void regex_handle_publish(regex_handle_t *handle) {
    if (!handle) return;
    spin_lock(&regex_handles_lock);
    handle->token = regex_allocate_token_locked();
    handle->next = regex_handles;
    regex_handles = handle;
    spin_unlock(&regex_handles_lock);
}

static void regex_handle_destroy(regex_handle_t *handle) {
    if (!handle) return;
    compiled_regex_release(&handle->compiled);
    kfree(handle);
}

static void regex_handle_release(regex_handle_t *handle) {
    int destroy;

    if (!handle) return;
    destroy = 0;
    spin_lock(&regex_handles_lock);
    if (handle->users > 0) handle->users--;
    if (handle->removed && handle->users == 0) destroy = 1;
    spin_unlock(&regex_handles_lock);
    if (destroy) regex_handle_destroy(handle);
}

static void regex_handle_remove(regex_handle_t *handle) {
    regex_handle_t **link;
    int destroy;

    if (!handle) return;
    destroy = 0;
    spin_lock(&regex_handles_lock);
    if (handle->removed) {
        spin_unlock(&regex_handles_lock);
        return;
    }
    link = &regex_handles;
    while (*link && *link != handle) link = &(*link)->next;
    if (*link == handle) *link = handle->next;
    handle->removed = 1;
    if (handle->users == 0) destroy = 1;
    spin_unlock(&regex_handles_lock);
    if (destroy) regex_handle_destroy(handle);
}

void regex_release_address_space(uint64_t owner_cr3) {
    regex_handle_t *handle;
    regex_handle_t *next;
    regex_handle_t *release;
    regex_handle_t **link;

    release = NULL;
    spin_lock(&regex_handles_lock);
    link = &regex_handles;
    while (*link) {
        handle = *link;
        if (handle->owner_cr3 == owner_cr3) {
            *link = handle->next;
            handle->removed = 1;
            if (handle->users == 0) {
                handle->next = release;
                release = handle;
            } else {
                handle->next = NULL;
            }
        } else {
            link = &handle->next;
        }
    }
    spin_unlock(&regex_handles_lock);
    while (release) {
        next = release->next;
        if (release->users == 0) regex_handle_destroy(release);
        release = next;
    }
}

int regex_clone_address_space(uint64_t source_cr3, uint64_t dest_cr3) {
    regex_handle_t **snapshot;
    regex_handle_t *handle;
    regex_handle_t *clone;
    size_t capacity;
    size_t count;
    size_t i;
    int result;

    if (!source_cr3 || !dest_cr3 || source_cr3 == dest_cr3) return 0;
    snapshot = NULL;
    capacity = 0;
    for (;;) {
        spin_lock(&regex_handles_lock);
        count = 0;
        handle = regex_handles;
        while (handle) {
            if (!handle->removed && handle->owner_cr3 == source_cr3)
                count++;
            handle = handle->next;
        }
        spin_unlock(&regex_handles_lock);
        if (!count) return 0;
        if (count > SIZE_MAX / sizeof(*snapshot)) return -1;
        snapshot = (regex_handle_t **)kmalloc(count * sizeof(*snapshot));
        if (!snapshot) return -1;
        capacity = count;
        spin_lock(&regex_handles_lock);
        count = 0;
        handle = regex_handles;
        while (handle) {
            if (!handle->removed && handle->owner_cr3 == source_cr3)
                count++;
            handle = handle->next;
        }
        if (count <= capacity) {
            i = 0;
            handle = regex_handles;
            while (handle) {
                if (!handle->removed && handle->owner_cr3 == source_cr3) {
                    handle->users++;
                    snapshot[i++] = handle;
                }
                handle = handle->next;
            }
            spin_unlock(&regex_handles_lock);
            count = i;
            break;
        }
        spin_unlock(&regex_handles_lock);
        kfree(snapshot);
        snapshot = NULL;
    }
    result = 0;
    for (i = 0; i < count; i++) {
        handle = snapshot[i];
        clone = (regex_handle_t *)kmalloc(sizeof(*clone));
        if (!clone) {
            result = -1;
        } else {
            memset(clone, 0, sizeof(*clone));
            clone->token = handle->token;
            clone->owner_cr3 = dest_cr3;
            clone->cflags = handle->cflags;
            if (compiled_regex_copy(&clone->compiled,
                                    &handle->compiled) != 0) {
                kfree(clone);
                clone = NULL;
                result = -1;
            }
        }
        if (clone) {
            spin_lock(&regex_handles_lock);
            clone->next = regex_handles;
            regex_handles = clone;
            spin_unlock(&regex_handles_lock);
        }
        regex_handle_release(handle);
        if (result != 0) {
            i++;
            while (i < count) regex_handle_release(snapshot[i++]);
            break;
        }
    }
    kfree(snapshot);
    if (result != 0) regex_release_address_space(dest_cr3);
    return result;
}

static int regex_copy_user_string(const char *source, char **result) {
    char *buffer;
    char *resized;
    size_t length;
    size_t capacity;
    char value;

    if (!source || !result) return -EFAULT;
    buffer = NULL;
    length = 0;
    capacity = 0;
    for (;;) {
        if (copy_from_user(&value, source + length, 1) < 0) {
            if (buffer) kfree(buffer);
            return -EFAULT;
        }
        if (length == capacity) {
            if (capacity > SIZE_MAX / 2) {
                if (buffer) kfree(buffer);
                return -ENOMEM;
            }
            capacity = capacity ? capacity * 2 : 64;
            resized = (char *)krealloc(buffer, capacity);
            if (!resized) {
                if (buffer) kfree(buffer);
                return -ENOMEM;
            }
            buffer = resized;
        }
        buffer[length++] = value;
        if (value == '\0') break;
    }
    resized = (char *)krealloc(buffer, length);
    if (resized) buffer = resized;
    *result = buffer;
    return 0;
}

static int compiled_regex_grow(compiled_regex_t *compiled, size_t needed) {
    regex_node_t *nodes;
    size_t capacity;

    if (needed <= compiled->node_capacity) return 1;
    capacity = compiled->node_capacity ? compiled->node_capacity : 8;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) return 0;
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(regex_node_t)) return 0;
    nodes = (regex_node_t *)krealloc(compiled->nodes,
                                     capacity * sizeof(regex_node_t));
    if (!nodes) return 0;
    memset(nodes + compiled->node_capacity, 0,
           (capacity - compiled->node_capacity) * sizeof(regex_node_t));
    compiled->nodes = nodes;
    compiled->node_capacity = capacity;
    return 1;
}

static int compile_regex(const char *pattern, compiled_regex_t *compiled, int cflags) {
    const char *p;
    const char *class_begin;
    regex_node_t *node;
    int *group_stack;
    int *resized_stack;
    int extended;
    int node_idx;
    int group_count;
    int group_depth;
    int group_capacity;
    size_t class_len;

    p = pattern;
    extended = cflags & REG_EXTENDED;
    node_idx = 0;
    group_count = 0;
    group_stack = NULL;
    group_depth = 0;
    group_capacity = 0;
    memset(compiled, 0, sizeof(*compiled));
    compiled->magic = REGEX_COMPILED_MAGIC;

    while (*p) {
        if (!compiled_regex_grow(compiled, node_idx + 1)) {
            if (group_stack) kfree(group_stack);
            compiled_regex_release(compiled);
            return REG_ESPACE;
        }
        node = &compiled->nodes[node_idx];
        memset(node, 0, sizeof(*node));
        compiled->num_nodes = node_idx + 1;
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
            node->type = NODE_CLASS;
            p++;
            node->class_negate = 0;
            
            if (*p == '^') {
                node->class_negate = 1;
                p++;
            }
            
            class_begin = p;
            if (*p == ']') p++;
            while (*p && *p != ']') p++;
            class_len = (size_t)(p - class_begin);
            if (class_len == SIZE_MAX) {
                if (group_stack) kfree(group_stack);
                compiled_regex_release(compiled);
                return REG_ESPACE;
            }
            node->class_start = (char *)kmalloc(class_len + 1);
            if (!node->class_start) {
                if (group_stack) kfree(group_stack);
                compiled->num_nodes = node_idx + 1;
                compiled_regex_release(compiled);
                return REG_ESPACE;
            }
            if (class_len) memcpy(node->class_start, class_begin, class_len);
            node->class_start[class_len] = '\0';
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
                if (group_count == INT32_MAX) {
                    if (group_stack) kfree(group_stack);
                    compiled->num_nodes = node_idx + 1;
                    compiled_regex_release(compiled);
                    return REG_ESPACE;
                }
                group_count++;
                node->group_num = group_count;
            }
            
            if (group_depth == group_capacity) {
                if (group_capacity > INT32_MAX / 2) {
                    if (group_stack) kfree(group_stack);
                    compiled->num_nodes = node_idx + 1;
                    compiled_regex_release(compiled);
                    return REG_ESPACE;
                }
                group_capacity = group_capacity ? group_capacity * 2 : 8;
                resized_stack = (int *)krealloc(
                    group_stack, (size_t)group_capacity * sizeof(int));
                if (!resized_stack) {
                    if (group_stack) kfree(group_stack);
                    compiled->num_nodes = node_idx + 1;
                    compiled_regex_release(compiled);
                    return REG_ESPACE;
                }
                group_stack = resized_stack;
            }
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
    if (group_stack) kfree(group_stack);
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
                               capture_t *captures, size_t capture_count,
                               int cflags, int eflags);

static int try_match(compiled_regex_t *compiled, size_t node_start, const char *text, const char *start,
                     const char **end_pos, capture_t *captures,
                     size_t capture_count, int cflags, int eflags) {
    const char *pos;
    const char *dummy;
    const char *alt_end;
    const char *cap_start;
    const char *cap_end;
    const char *try_pos;
    const char *next_end;
    size_t node_idx;
    size_t la_start;
    size_t la_end;
    size_t cap_len;
    size_t cap_index;
    int min_rep;
    int max_rep;
    int greedy;
    int matched;
    int pos_count;
    int depth;
    int grp;
    int icase;
    int i;
    regex_node_t *node;
    compiled_regex_t sub_compiled;

    pos = text;
    node_idx = node_start;
    
    while (node_idx < compiled->num_nodes) {
        node = &compiled->nodes[node_idx];
        
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
            if (captures && node->group_num > 0 &&
                (size_t)node->group_num < capture_count)
                captures[node->group_num].start = pos;
            node_idx++;
            continue;
        }
        
        if (node->type == NODE_GROUP_END) {
            if (captures && node->group_num > 0 &&
                (size_t)node->group_num < capture_count)
                captures[node->group_num].end = pos;
            
            if (node->quant != QUANT_NONE) {
                node_idx++;
                continue;
            }
            node_idx++;
            continue;
        }
        
        if (node->type == NODE_LOOKAHEAD) {
            depth = 1;
            la_start = node_idx + 1;
            la_end = la_start;
            while (la_end < compiled->num_nodes && depth > 0) {
                if (compiled->nodes[la_end].type == NODE_GROUP_START)
                    depth++;
                else if (compiled->nodes[la_end].type == NODE_GROUP_END)
                    depth--;
                la_end++;
            }
            
            sub_compiled.num_nodes = la_end - la_start;
            sub_compiled.num_groups = compiled->num_groups;
            sub_compiled.node_capacity = sub_compiled.num_nodes;
            sub_compiled.nodes = compiled->nodes + la_start;
            
            if (!try_match(&sub_compiled, 0, pos, start, &dummy, captures,
                           capture_count, cflags, eflags))
                return 0;
            
            node_idx = la_end;
            continue;
        }
        
        if (node->type == NODE_NEG_LOOKAHEAD) {
            depth = 1;
            la_start = node_idx + 1;
            la_end = la_start;
            while (la_end < compiled->num_nodes && depth > 0) {
                if (compiled->nodes[la_end].type == NODE_GROUP_START)
                    depth++;
                else if (compiled->nodes[la_end].type == NODE_GROUP_END)
                    depth--;
                la_end++;
            }
            
            sub_compiled.num_nodes = la_end - la_start;
            sub_compiled.num_groups = compiled->num_groups;
            sub_compiled.node_capacity = sub_compiled.num_nodes;
            sub_compiled.nodes = compiled->nodes + la_start;
            
            if (try_match(&sub_compiled, 0, pos, start, &dummy, captures,
                          capture_count, cflags, eflags))
                return 0;
            
            node_idx = la_end;
            continue;
        }
        
        if (node->type == NODE_BACKREF) {
            grp = node->group_num;
            if (captures && grp > 0 && (size_t)grp < capture_count &&
                captures[grp].start && captures[grp].end) {
                cap_start = captures[grp].start;
                cap_end = captures[grp].end;
                cap_len = (size_t)(cap_end - cap_start);
                icase = cflags & REG_ICASE;
                
                for (cap_index = 0; cap_index < cap_len; cap_index++) {
                    if (!*pos) return 0;
                    if (icase) {
                        if (char_tolower(*pos) !=
                            char_tolower(cap_start[cap_index]))
                            return 0;
                    } else {
                        if (*pos != cap_start[cap_index])
                            return 0;
                    }
                    pos++;
                }
            }
            node_idx++;
            continue;
        }
        
        if (node->alt_next > 0) {
            if (try_match(compiled, node_idx + 1, pos, start, &alt_end,
                          captures, capture_count, cflags, eflags)) {
                pos = alt_end;
                while (node_idx < compiled->num_nodes && compiled->nodes[node_idx].alt_next <= 0)
                    node_idx++;
                if (node_idx < compiled->num_nodes)
                    node_idx = (size_t)compiled->nodes[node_idx].alt_next;
                continue;
            }
            node_idx = (size_t)node->alt_next;
            continue;
        }
        
        min_rep = node->min_rep;
        max_rep = node->max_rep;
        greedy = node->greedy;
        matched = 0;
        pos_count = 0;
        
        pos_count = 1;
        while ((max_rep < 0 || matched < max_rep) && *pos) {
            if (!match_node(node, *pos, cflags))
                break;
            pos++;
            matched++;
            pos_count++;
        }
        
        if (matched < min_rep)
            return 0;
        
        if (greedy) {
            for (i = pos_count - 1; i >= 0; i--) {
                if (i < min_rep) break;
                try_pos = text + i;
                
                if (node_idx + 1 >= compiled->num_nodes) {
                    pos = try_pos;
                    *end_pos = pos;
                    return 1;
                }
                
                if (try_match(compiled, node_idx + 1, try_pos, start,
                              &next_end, captures, capture_count,
                              cflags, eflags)) {
                    *end_pos = next_end;
                    return 1;
                }
            }
            return 0;
        } else {
            for (i = min_rep; i < pos_count; i++) {
                try_pos = text + i;
                
                if (node_idx + 1 >= compiled->num_nodes) {
                    pos = try_pos;
                    *end_pos = pos;
                    return 1;
                }
                
                if (try_match(compiled, node_idx + 1, try_pos, start,
                              &next_end, captures, capture_count,
                              cflags, eflags)) {
                    *end_pos = next_end;
                    return 1;
                }
            }
            return 0;
        }
    }
    
    *end_pos = pos;
    return 1;
}

static int regex_exec_internal(compiled_regex_t *compiled, const char *text, const char *start,
                               capture_t *captures, size_t capture_count,
                               int cflags, int eflags) {
    const char *pos = text;
    const char *end_pos;
    
    int has_anchor = compiled->num_nodes > 0 && compiled->nodes[0].type == NODE_ANCHOR_START;
    
    if (has_anchor) {
        if (try_match(compiled, 0, pos, start, &end_pos, captures,
                      capture_count, cflags, eflags)) {
            if (captures) {
                captures[0].start = pos;
                captures[0].end = end_pos;
            }
            return 1;
        }
        return 0;
    }
    
    do {
        if (try_match(compiled, 0, pos, start, &end_pos, captures,
                      capture_count, cflags, eflags)) {
            if (captures) {
                captures[0].start = pos;
                captures[0].end = end_pos;
            }
            return 1;
        }
    } while (*pos++ != '\0');
    
    return 0;
}

static int sys_regcomp(uint64_t preg_ptr, const char *pattern_ptr, int cflags) {
    uint64_t preg_addr;
    uint64_t pat_addr;
    char *pattern;
    kernel_regex_t value;
    regex_handle_t *handle;
    int result;

    if (!preg_ptr || !pattern_ptr) return -EINVAL;
    
    preg_addr = (uint64_t)preg_ptr;
    pat_addr = (uint64_t)(uintptr_t)pattern_ptr;
    
    if (preg_addr >= KERNEL_VMA || pat_addr >= KERNEL_VMA) return -EFAULT;
    
    result = regex_copy_user_string((const char *)(uintptr_t)pat_addr,
                                    &pattern);
    if (result < 0) return result;
    handle = regex_handle_create();
    if (!handle) {
        kfree(pattern);
        return REG_ESPACE;
    }
    handle->cflags = cflags;
    result = compile_regex(pattern, &handle->compiled, cflags);
    kfree(pattern);
    if (result != REG_OK) {
        regex_handle_remove(handle);
        regex_handle_release(handle);
        return result;
    }
    regex_handle_publish(handle);
    memset(&value, 0, sizeof(value));
    value.re_nsub = handle->compiled.num_groups;
    value.nsub2 = handle->compiled.num_groups;
    value.opaque = (void *)(uintptr_t)handle->token;
    if (copy_to_user((void *)(uintptr_t)preg_addr, &value,
                     sizeof(value)) < 0) {
        regex_handle_remove(handle);
        regex_handle_release(handle);
        return -EFAULT;
    }
    regex_handle_release(handle);
    return REG_OK;
}

static int sys_regexec(uint64_t preg_ptr, const char *string_ptr,
                       uint64_t nmatch_arg, uint64_t pmatch_arg) {
    uint64_t preg_addr;
    uint64_t str_addr;
    char *string;
    kernel_regex_t value;
    kernel_regmatch_t match;
    compiled_regex_t *compiled;
    regex_handle_t *handle;
    capture_t *captures;
    size_t nmatch;
    size_t capture_count;
    size_t i;
    int copy_result;
    int matched;

    if (!preg_ptr || !string_ptr) return REG_NOMATCH;
    
    preg_addr = (uint64_t)preg_ptr;
    str_addr = (uint64_t)(uintptr_t)string_ptr;
    
    if (preg_addr >= KERNEL_VMA || str_addr >= KERNEL_VMA) return REG_NOMATCH;
    
    copy_result = regex_copy_user_string(
        (const char *)(uintptr_t)str_addr, &string);
    if (copy_result < 0)
        return copy_result == -ENOMEM ? REG_ESPACE : REG_NOMATCH;
    if (copy_from_user(&value, (const void *)(uintptr_t)preg_addr,
                       sizeof(value)) < 0) {
        kfree(string);
        return REG_BADPAT;
    }
    handle = regex_handle_acquire((uint64_t)(uintptr_t)value.opaque);
    if (!handle) {
        kfree(string);
        return REG_BADPAT;
    }
    compiled = &handle->compiled;
    if (compiled->magic != REGEX_COMPILED_MAGIC) {
        regex_handle_release(handle);
        kfree(string);
        return REG_BADPAT;
    }
    nmatch = (size_t)nmatch_arg;
    if (handle->cflags & REG_NOSUB) {
        nmatch = 0;
        pmatch_arg = 0;
    }
    if (pmatch_arg && (pmatch_arg >= KERNEL_VMA ||
        nmatch > (KERNEL_VMA - pmatch_arg) / sizeof(match))) {
        regex_handle_release(handle);
        kfree(string);
        return REG_ESPACE;
    }
    if (compiled->num_groups == SIZE_MAX) {
        regex_handle_release(handle);
        kfree(string);
        return REG_ESPACE;
    }
    capture_count = (size_t)compiled->num_groups + 1;
    if (capture_count > SIZE_MAX / sizeof(capture_t)) {
        regex_handle_release(handle);
        kfree(string);
        return REG_ESPACE;
    }
    captures = (capture_t *)kmalloc(capture_count * sizeof(capture_t));
    if (!captures) {
        regex_handle_release(handle);
        kfree(string);
        return REG_ESPACE;
    }
    for (i = 0; i < capture_count; i++) {
        captures[i].start = NULL;
        captures[i].end = NULL;
    }
    matched = regex_exec_internal(compiled, string, string, captures,
                                  capture_count, handle->cflags, 0);
    if (!matched) {
        kfree(captures);
        regex_handle_release(handle);
        kfree(string);
        return REG_NOMATCH;
    }
    if (pmatch_arg && nmatch > 0) {
        for (i = 0; i < nmatch; i++) {
            if (i >= capture_count) {
                match.rm_so = -1;
                match.rm_eo = -1;
            } else if (captures[i].start && captures[i].end) {
                match.rm_so = captures[i].start - string;
                match.rm_eo = captures[i].end - string;
            } else {
                match.rm_so = -1;
                match.rm_eo = -1;
            }
            if (copy_to_user((void *)(uintptr_t)(pmatch_arg +
                             i * sizeof(match)), &match,
                             sizeof(match)) < 0) {
                kfree(captures);
                regex_handle_release(handle);
                kfree(string);
                return REG_ESPACE;
            }
        }
    }
    kfree(captures);
    regex_handle_release(handle);
    kfree(string);
    return REG_OK;
}

static int sys_regfree(uint64_t preg_ptr, const char *unused1, int unused2) {
    uint64_t preg_addr;
    kernel_regex_t value;
    regex_handle_t *handle;

    (void)unused1; (void)unused2;
    
    if (!preg_ptr) return 0;
    
    preg_addr = (uint64_t)preg_ptr;
    if (preg_addr >= KERNEL_VMA) return -EFAULT;
    
    if (copy_from_user(&value, (const void *)(uintptr_t)preg_addr,
                       sizeof(value)) < 0) return -EFAULT;
    handle = regex_handle_acquire((uint64_t)(uintptr_t)value.opaque);
    if (handle) {
        regex_handle_remove(handle);
        regex_handle_release(handle);
    }
    memset(&value, 0, sizeof(value));
    if (copy_to_user((void *)(uintptr_t)preg_addr, &value,
                     sizeof(value)) < 0) return -EFAULT;
    return 0;
}

static int sys_regerror(int errcode, uint64_t preg_arg, uint64_t errbuf_arg,
                        uint64_t errbuf_size_arg) {
    char *errbuf;
    size_t errbuf_size;
    const char *msg;
    size_t copy;
    size_t i;
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
        copy = (size_t)len;
        if (copy >= errbuf_size) copy = errbuf_size - 1;
        for (i = 0; i < copy; i++)
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

static int sys_fnmatch(uint64_t pattern_ptr, const char *string_ptr, int flags) {
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

static int sys_glob(uint64_t pattern_ptr, const char *flags_errfunc_ptr,
                    uint64_t pglob_ptr) {
    uint64_t pat_addr;
    uint64_t glob_addr;
    const char *pattern;
    int flags;
    char *dir_path;
    char *file_pattern;
    size_t pattern_len;
    size_t dir_len;
    size_t file_len;
    size_t last_slash;
    size_t i;
    size_t j;
    char **results;
    size_t count;
    size_t capacity;
    size_t new_capacity;
    uint64_t idx;
    char **pathv;
    char **new_results;
    char *path;
    const char *entry_name;
    kernel_glob_t *pglob;
    vfs_node_t *dir;
    dirent_t *dirent;
    size_t entry_len;
    size_t total;
    size_t pos;
    int need_slash;
    if (!pattern_ptr || !pglob_ptr) return GLOB_NOMATCH;
    
    pat_addr = (uint64_t)pattern_ptr;
    glob_addr = (uint64_t)pglob_ptr;
    
    if (pat_addr >= KERNEL_VMA || glob_addr >= KERNEL_VMA) return GLOB_NOMATCH;
    
    pattern = (const char *)pat_addr;
    pglob = (kernel_glob_t *)glob_addr;
    flags = (int)(uintptr_t)flags_errfunc_ptr;
    
    if (!(flags & GLOB_APPEND)) {
        pglob->gl_pathc = 0;
        pglob->gl_pathv = NULL;
    }
    
    pattern_len = strlen(pattern);
    last_slash = pattern_len;
    for (i = 0; i < pattern_len; i++) {
        if (pattern[i] == '/') last_slash = i;
    }

    if (last_slash < pattern_len) {
        dir_len = last_slash ? last_slash : 1;
        file_len = pattern_len - last_slash - 1;
    } else {
        dir_len = 1;
        file_len = pattern_len;
    }
    dir_path = (char *)kmalloc(dir_len + 1);
    file_pattern = (char *)kmalloc(file_len + 1);
    if (!dir_path || !file_pattern) {
        if (dir_path) kfree(dir_path);
        if (file_pattern) kfree(file_pattern);
        return GLOB_NOSPACE;
    }

    if (last_slash < pattern_len) {
        if (last_slash == 0) {
            dir_path[0] = '/';
            dir_path[1] = '\0';
        } else {
            memcpy(dir_path, pattern, last_slash);
            dir_path[last_slash] = '\0';
        }
        memcpy(file_pattern, pattern + last_slash + 1, file_len);
        file_pattern[file_len] = '\0';
    } else {
        dir_path[0] = '.';
        dir_path[1] = '\0';
        memcpy(file_pattern, pattern, file_len);
        file_pattern[file_len] = '\0';
    }
    
    dir = vfs_namei(dir_path);
    if (!dir) {
        if (flags & GLOB_NOCHECK) {
            pglob->gl_pathc = 1;
            pathv = (char **)kmalloc(2 * sizeof(char *));
            if (!pathv) {
                kfree(file_pattern);
                kfree(dir_path);
                return GLOB_NOSPACE;
            }
            pathv[0] = (char *)kmalloc(pattern_len + 1);
            if (!pathv[0]) {
                kfree(pathv);
                kfree(file_pattern);
                kfree(dir_path);
                return GLOB_NOSPACE;
            }
            memcpy(pathv[0], pattern, pattern_len + 1);
            pathv[1] = NULL;
            pglob->gl_pathv = pathv;
            kfree(file_pattern);
            kfree(dir_path);
            return 0;
        }
        kfree(file_pattern);
        kfree(dir_path);
        return GLOB_NOMATCH;
    }
    
    results = NULL;
    count = 0;
    capacity = 0;
    
    idx = 0;
    
    while ((dirent = vfs_readdir(dir, idx)) != NULL) {
        entry_name = vfs_dirent_name(dirent);
        if (entry_name[0] == '\0') break;
        
        if (glob_match_pattern(file_pattern, entry_name)) {
            if (count >= capacity) {
                new_capacity = capacity ? capacity * 2 : 8;
                if (new_capacity < capacity ||
                    new_capacity > SIZE_MAX / sizeof(char *)) {
                    for (j = 0; j < count; j++) kfree(results[j]);
                    if (results) kfree(results);
                    vfs_release(dir);
                    kfree(file_pattern);
                    kfree(dir_path);
                    return GLOB_NOSPACE;
                }
                new_results = (char **)kmalloc(new_capacity * sizeof(char *));
                if (!new_results) {
                    for (j = 0; j < count; j++) kfree(results[j]);
                    if (results) kfree(results);
                    vfs_release(dir);
                    kfree(file_pattern);
                    kfree(dir_path);
                    return GLOB_NOSPACE;
                }
                for (j = 0; j < count; j++) new_results[j] = results[j];
                if (results) kfree(results);
                results = new_results;
                capacity = new_capacity;
            }
            
            entry_len = strlen(entry_name);
            need_slash = (dir_len > 0 && dir_path[dir_len - 1] != '/') ? 1 : 0;
            if (entry_len > SIZE_MAX - dir_len - (size_t)need_slash - 1) {
                for (j = 0; j < count; j++) kfree(results[j]);
                if (results) kfree(results);
                vfs_release(dir);
                kfree(file_pattern);
                kfree(dir_path);
                return GLOB_NOSPACE;
            }
            total = dir_len + (size_t)need_slash + entry_len + 1;
            
            path = (char *)kmalloc(total);
            if (!path) {
                for (j = 0; j < count; j++) kfree(results[j]);
                if (results) kfree(results);
                vfs_release(dir);
                kfree(file_pattern);
                kfree(dir_path);
                return GLOB_NOSPACE;
            }
            
            pos = 0;
            memcpy(path + pos, dir_path, dir_len);
            pos += dir_len;
            if (need_slash) path[pos++] = '/';
            memcpy(path + pos, entry_name, entry_len);
            pos += entry_len;
            path[pos] = '\0';
            
            results[count++] = path;
        }
        idx++;
    }
    
    if (count == 0) {
        if (results) kfree(results);
        vfs_release(dir);
        if (flags & GLOB_NOCHECK) {
            pglob->gl_pathc = 1;
            pathv = (char **)kmalloc(2 * sizeof(char *));
            if (!pathv) {
                kfree(file_pattern);
                kfree(dir_path);
                return GLOB_NOSPACE;
            }
            pathv[0] = (char *)kmalloc(pattern_len + 1);
            if (!pathv[0]) {
                kfree(pathv);
                kfree(file_pattern);
                kfree(dir_path);
                return GLOB_NOSPACE;
            }
            memcpy(pathv[0], pattern, pattern_len + 1);
            pathv[1] = NULL;
            pglob->gl_pathv = pathv;
            kfree(file_pattern);
            kfree(dir_path);
            return 0;
        }
        kfree(file_pattern);
        kfree(dir_path);
        return GLOB_NOMATCH;
    }
    
    vfs_release(dir);
    kfree(file_pattern);
    kfree(dir_path);
    if (count == SIZE_MAX / sizeof(char *)) {
        for (j = 0; j < count; j++) kfree(results[j]);
        kfree(results);
        return GLOB_NOSPACE;
    }
    pathv = (char **)kmalloc((count + 1) * sizeof(char *));
    if (!pathv) {
        for (j = 0; j < count; j++) kfree(results[j]);
        kfree(results);
        return GLOB_NOSPACE;
    }
    
    for (j = 0; j < count; j++) pathv[j] = results[j];
    pathv[count] = NULL;
    
    kfree(results);
    
    pglob->gl_pathc = count;
    pglob->gl_pathv = pathv;
    
    return 0;
}

static int sys_globfree(uint64_t pglob_ptr, const char *unused1, int unused2) {
    uint64_t glob_addr;
    kernel_glob_t *pglob;
    size_t i;

    (void)unused1; (void)unused2;
    
    if (!pglob_ptr) return 0;
    
    glob_addr = (uint64_t)pglob_ptr;
    if (glob_addr >= KERNEL_VMA) return -EFAULT;
    
    pglob = (kernel_glob_t *)glob_addr;
    
    if (pglob->gl_pathv) {
        for (i = 0; i < pglob->gl_pathc; i++) {
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

static int sys_sscanf(uint64_t str_ptr, const char *format_ptr,
                      uint64_t args_ptr) {
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

static int sys_regsub(uint64_t preg_ptr, const char *string_ptr,
                      int replacement_output) {
    uint64_t preg_addr;
    uint64_t str_addr;
    uint64_t repl_addr;
    uint64_t out_addr;
    const char *string;
    const char *replacement;
    char *output;
    kernel_regex_t value;
    regex_handle_t *handle;
    compiled_regex_t *compiled;
    capture_t *captures;
    size_t capture_count;
    size_t i;
    int out_idx;
    int matched;
    int len;
    int copy_index;
    const char *s;
    const char *r;
    if (!preg_ptr || !string_ptr) return -EINVAL;
    
    preg_addr = (uint64_t)preg_ptr;
    str_addr = (uint64_t)(uintptr_t)string_ptr;
    repl_addr = (replacement_output >> 16) ? (replacement_output & 0xFFFF0000) : 0;
    out_addr = replacement_output & 0xFFFF;
    
    if (preg_addr >= KERNEL_VMA || str_addr >= KERNEL_VMA) return -EFAULT;
    
    string = (const char *)str_addr;
    replacement = repl_addr ? (const char *)repl_addr : "";
    output = out_addr ? (char *)out_addr : NULL;
    
    if (copy_from_user(&value, (const void *)(uintptr_t)preg_addr,
                       sizeof(value)) < 0) return -EFAULT;
    handle = regex_handle_acquire((uint64_t)(uintptr_t)value.opaque);
    if (!handle) return -EINVAL;
    compiled = &handle->compiled;
    if (compiled->magic != REGEX_COMPILED_MAGIC) {
        regex_handle_release(handle);
        return -EINVAL;
    }
    capture_count = (size_t)compiled->num_groups + 1;
    if (capture_count > SIZE_MAX / sizeof(capture_t)) {
        regex_handle_release(handle);
        return -ENOMEM;
    }
    captures = (capture_t *)kmalloc(capture_count * sizeof(capture_t));
    if (!captures) {
        regex_handle_release(handle);
        return -ENOMEM;
    }
    for (i = 0; i < capture_count; i++) {
        captures[i].start = NULL;
        captures[i].end = NULL;
    }
    matched = regex_exec_internal(compiled, string, string, captures,
                                  capture_count, handle->cflags, 0);
    if (!matched) {
        if (output) {
            copy_index = 0;
            while (string[copy_index]) {
                output[copy_index] = string[copy_index];
                copy_index++;
            }
            output[copy_index] = '\0';
            kfree(captures);
            regex_handle_release(handle);
            return copy_index;
        }
        len = 0;
        while (string[len]) len++;
        kfree(captures);
        regex_handle_release(handle);
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
            if ((size_t)grp < capture_count && captures[grp].start && captures[grp].end) {
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
            if ((size_t)grp < capture_count && captures[grp].start && captures[grp].end) {
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
    kfree(captures);
    regex_handle_release(handle);
    return out_idx;
}

static int regex_exec_matches(uint64_t preg_ptr, const char *string_ptr,
                              uint64_t pmatch_ptr, size_t nmatch) {
    uint64_t preg_addr;
    uint64_t str_addr;
    uint64_t pm_addr;
    char *string;
    kernel_regex_t value;
    regex_handle_t *handle;
    kernel_regmatch_t match;
    compiled_regex_t *compiled;
    capture_t *captures;
    size_t capture_count;
    size_t i;
    int copy_result;
    int matched;

    if (!preg_ptr || !string_ptr) return REG_NOMATCH;
    
    preg_addr = (uint64_t)preg_ptr;
    str_addr = (uint64_t)(uintptr_t)string_ptr;
    pm_addr = (uint64_t)pmatch_ptr;
    
    if (preg_addr >= KERNEL_VMA || str_addr >= KERNEL_VMA) return REG_NOMATCH;
    
    if (pm_addr && (pm_addr >= KERNEL_VMA ||
        nmatch > (KERNEL_VMA - pm_addr) / sizeof(match)))
        return REG_ESPACE;
    copy_result = regex_copy_user_string(
        (const char *)(uintptr_t)str_addr, &string);
    if (copy_result < 0)
        return copy_result == -ENOMEM ? REG_ESPACE : REG_NOMATCH;
    if (copy_from_user(&value, (const void *)(uintptr_t)preg_addr,
                       sizeof(value)) < 0) {
        kfree(string);
        return REG_BADPAT;
    }
    handle = regex_handle_acquire((uint64_t)(uintptr_t)value.opaque);
    if (!handle) {
        kfree(string);
        return REG_BADPAT;
    }
    compiled = &handle->compiled;
    if (compiled->magic != REGEX_COMPILED_MAGIC) {
        regex_handle_release(handle);
        kfree(string);
        return REG_BADPAT;
    }
    if (compiled->num_groups == SIZE_MAX) {
        regex_handle_release(handle);
        kfree(string);
        return REG_ESPACE;
    }
    capture_count = (size_t)compiled->num_groups + 1;
    if (capture_count > SIZE_MAX / sizeof(capture_t)) {
        regex_handle_release(handle);
        kfree(string);
        return REG_ESPACE;
    }
    captures = (capture_t *)kmalloc(capture_count * sizeof(capture_t));
    if (!captures) {
        regex_handle_release(handle);
        kfree(string);
        return REG_ESPACE;
    }
    for (i = 0; i < capture_count; i++) {
        captures[i].start = NULL;
        captures[i].end = NULL;
    }
    matched = regex_exec_internal(compiled, string, string, captures,
                                  capture_count, handle->cflags, 0);
    if (!matched) {
        kfree(captures);
        regex_handle_release(handle);
        kfree(string);
        return REG_NOMATCH;
    }
    if (pm_addr) {
        for (i = 0; i < nmatch; i++) {
            if (i < capture_count && captures[i].start && captures[i].end) {
                match.rm_so = captures[i].start - string;
                match.rm_eo = captures[i].end - string;
            } else {
                match.rm_so = -1;
                match.rm_eo = -1;
            }
            if (copy_to_user((void *)(uintptr_t)(pm_addr +
                             i * sizeof(match)), &match,
                             sizeof(match)) < 0) {
                kfree(captures);
                regex_handle_release(handle);
                kfree(string);
                return REG_ESPACE;
            }
        }
    }
    kfree(captures);
    regex_handle_release(handle);
    kfree(string);
    return REG_OK;
}

static int sys_regexec_ex(uint64_t preg_ptr, const char *string_ptr,
                          uint64_t pmatch_ptr) {
    return regex_exec_matches(preg_ptr, string_ptr, pmatch_ptr,
                              LEGACY_MATCH_COUNT);
}

static int sys_regexec_ex2(uint64_t preg_ptr, const char *string_ptr,
                           uint64_t pmatch_ptr, uint64_t nmatch) {
    return regex_exec_matches(preg_ptr, string_ptr, pmatch_ptr,
                              (size_t)nmatch);
}

void syscalls_regex_init(void) {
    syscall_table_set(SYSCALL_REGCOMP, (void *)(sys_regcomp));
    syscall_table_set(SYSCALL_REGEXEC, (void *)(sys_regexec));
    syscall_table_set(SYSCALL_REGFREE, (void *)(sys_regfree));
    syscall_table_set(SYSCALL_REGERROR, (void *)(sys_regerror));
    syscall_table_set(SYSCALL_FNMATCH, (void *)(sys_fnmatch));
    syscall_table_set(SYSCALL_GLOB, (void *)(sys_glob));
    syscall_table_set(SYSCALL_GLOBFREE, (void *)(sys_globfree));
    syscall_table_set(SYSCALL_SSCANF, (void *)(sys_sscanf));
    syscall_table_set(SYSCALL_SCANF_GETCHAR, (void *)(sys_scanf_getchar));
    syscall_table_set(SYSCALL_REGSUB, (void *)(sys_regsub));
    syscall_table_set(SYSCALL_REGEXEC_EX, (void *)(sys_regexec_ex));
    syscall_table_set(SYSCALL_REGEXEC_EX2, (void *)(sys_regexec_ex2));
}
