#include <lebirun/lke.h>
#include <lebirun/elf.h>
#include <lebirun/vfs.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <lebirun/pit.h>
#include <lebirun/rng.h>
#include <lebirun/crypto.h>
#include <lebirun/task.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/drivers/net/netif.h>
#include <lebirun/drivers/net/ipv4.h>
#include <lebirun/drivers/net/dns.h>
#include <lebirun/drivers/net/udp.h>
#include <string.h>

#define R_X86_64_32    10
#define R_X86_64_32S   11
#define R_X86_64_PLT32  4

#define LKE_NR_SYSCALLS 284

static lke_module_t *modules;
static size_t lke_count;

static lke_ksym_t *ksym_table;
static int ksym_count = 0;

extern void **syscall_table;

int lke_register_syscall(int num, void *fn) {
    if (num < 0 || num >= LKE_NR_SYSCALLS || !fn) return -1;
    if (!syscall_table) return -1;
    if (syscall_table[num] && syscall_table[num] != fn) return -2;
    syscall_table[num] = fn;
    return 0;
}

void lke_unregister_syscall(int num, void *fn) {
    if (num < 0 || num >= LKE_NR_SYSCALLS) return;
    if (!syscall_table) return;
    if (syscall_table[num] == fn) syscall_table[num] = NULL;
}

void lke_register_symbol(const char *name, void *addr) {
    lke_ksym_t *new_tab;

    if (ksym_count == INT32_MAX) return;
    new_tab = (lke_ksym_t *)krealloc(
        ksym_table, ((size_t)ksym_count + 1) * sizeof(lke_ksym_t));
    if (!new_tab) return;
    ksym_table = new_tab;
    ksym_table[ksym_count].name = name;
    ksym_table[ksym_count].addr = (uint64_t)addr;
    ksym_count++;
}

static uint64_t ksym_lookup(const char *name) {
    int i;
    for (i = 0; i < ksym_count; i++) {
        if (strcmp(ksym_table[i].name, name) == 0)
            return ksym_table[i].addr;
    }
    return 0;
}

void lke_init(void) {
    modules = NULL;
    lke_count = 0;
    ksym_table = NULL;
    ksym_count = 0;

    lke_register_symbol("kmalloc", kmalloc);
    lke_register_symbol("kmalloc_aligned", kmalloc_aligned);
    lke_register_symbol("kfree", kfree);
    lke_register_symbol("printf", printf);
    lke_register_symbol("memcpy", memcpy);
    lke_register_symbol("memset", memset);
    lke_register_symbol("memcmp", memcmp);
    lke_register_symbol("strcmp", strcmp);
    lke_register_symbol("strlen", strlen);
    lke_register_symbol("strncpy", strncpy);
    lke_register_symbol("snprintf", snprintf);
    lke_register_symbol("vfs_namei", vfs_namei);
    lke_register_symbol("vfs_read", vfs_read);
    lke_register_symbol("vfs_write", vfs_write);
    lke_register_symbol("vfs_release", vfs_release);
    lke_register_symbol("pit_get_ticks64", pit_get_ticks64);
    lke_register_symbol("pit_get_uptime_ms", pit_get_uptime_ms);
    lke_register_symbol("pit_get_ticks", pit_get_ticks);
    lke_register_symbol("pit_ms_to_ticks", pit_ms_to_ticks);
    lke_register_symbol("rng_get_u64", rng_get_u64);
    lke_register_symbol("sha256_hash", sha256_hash);
    lke_register_symbol("hmac_sha256", hmac_sha256);
    lke_register_symbol("crypto_constant_compare", crypto_constant_compare);
    lke_register_symbol("netif_get_default", netif_get_default);
    lke_register_symbol("netif_poll_all", netif_poll_all);
    lke_register_symbol("ipv4_is_local", ipv4_is_local);
    lke_register_symbol("udp_send", udp_send);
    lke_register_symbol("udp_send6", udp_send6);
    lke_register_symbol("udp_register_port_hook", udp_register_port_hook);
    lke_register_symbol("udp_unregister_port_hook", udp_unregister_port_hook);
    lke_register_symbol("dns_resolve_timeout", dns_resolve_timeout);
    lke_register_symbol("dns_resolve6", dns_resolve6);
    lke_register_symbol("vmm_get_phys_in_pml4", vmm_get_phys_in_pml4);
    lke_register_symbol("task_current", task_current);
    lke_register_symbol("schedule", schedule);
    lke_register_symbol("net_get_ticks", net_get_ticks);
    lke_register_symbol("lke_register_syscall", lke_register_syscall);
    lke_register_symbol("lke_unregister_syscall", lke_unregister_syscall);
}

static lke_module_t *lke_new_slot(void) {
    lke_module_t *new_arr;

    if (lke_count >= SIZE_MAX / sizeof(lke_module_t)) return NULL;
    new_arr = (lke_module_t *)krealloc(
        modules, (lke_count + 1) * sizeof(lke_module_t));
    if (!new_arr) return NULL;
    modules = new_arr;
    memset(&modules[lke_count], 0, sizeof(lke_module_t));
    return &modules[lke_count];
}

static void lke_shrink_registry(void) {
    lke_module_t *new_modules;

    if (lke_count == 0) {
        kfree(modules);
        modules = NULL;
        return;
    }
    new_modules = (lke_module_t *)krealloc(
        modules, lke_count * sizeof(lke_module_t));
    if (!new_modules) return;
    modules = new_modules;
}

static lke_module_t *lke_find_by_name(const char *name) {
    size_t i;

    for (i = 0; i < lke_count; i++) {
        if (strcmp(modules[i].name, name) == 0) return &modules[i];
    }
    return NULL;
}

static void *lke_alloc_pages(uint64_t size, uint64_t *out_pages) {
    uint64_t pages;
    uint64_t phys;
    void *ptr;

    pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;
    phys = pfa_alloc_contiguous(pages);
    if (!phys) return NULL;
    ptr = (void *)(phys + KERNEL_VMA);
    memset(ptr, 0, pages * PAGE_SIZE);
    if (out_pages) *out_pages = pages;
    return ptr;
}

static void lke_free_pages(void *ptr, uint64_t pages) {
    uint64_t phys;

    if (!ptr || pages == 0) return;
    phys = (uint64_t)(uintptr_t)ptr - KERNEL_VMA;
    pfa_free_contiguous(phys, pages);
}

static const char *basename_of(const char *path) {
    const char *p;
    const char *last;
    last = path;
    for (p = path; *p; p++) {
        if (*p == '/') last = p + 1;
    }
    return last;
}

static size_t lke_name_length(const char *src) {
    const char *dot;
    size_t len;

    dot = NULL;
    for (len = 0; src[len]; len++) {
        if (src[len] == '.') dot = &src[len];
    }
    if (dot) len = (size_t)(dot - src);
    return len;
}

static int lke_name_matches(const char *name, const char *candidate,
                            size_t candidate_len) {
    size_t name_len;

    name_len = strlen(name);
    if (name_len != candidate_len) return 0;
    return memcmp(name, candidate, candidate_len) == 0;
}

static lke_module_t *lke_find_by_name_slice(const char *name, size_t name_len) {
    size_t i;

    for (i = 0; i < lke_count; i++) {
        if (lke_name_matches(modules[i].name, name, name_len))
            return &modules[i];
    }
    return NULL;
}

int lke_load(const char *path) {
    vfs_node_t *node;
    uint64_t data_size;
    uint8_t *data;
    const Elf64_Ehdr *ehdr;
    const Elf64_Shdr *shdr;
    uint64_t i;
    uint64_t total_alloc;
    uint8_t *mem;
    uint64_t *sec_offsets;
    uint64_t offset;
    const Elf64_Shdr *sh;
    const Elf64_Shdr *rela_sh;
    uint64_t j;
    const Elf64_Rela *rela;
    uint64_t sym_idx;
    uint64_t rtype;
    const Elf64_Shdr *symtab_sh;
    const Elf64_Sym *sym;
    const char *strtab;
    uint64_t S;
    uint64_t A;
    uint64_t P;
    uint8_t *target;
    lke_module_t *mod;
    const char *bname;
    char *mod_name;
    size_t mod_name_len;
    int (*init_fn)(void);
    void (*cleanup_fn)(void);
    const Elf64_Shdr *strtab_sh;
    uint64_t symtab_idx;
    uint64_t strtab_idx;
    const char *sym_name;
    int found_init;
    uint64_t data_pages;
    uint64_t mem_pages;

    if (!path) return -1;

    bname = basename_of(path);
    mod_name_len = lke_name_length(bname);
    if (lke_find_by_name_slice(bname, mod_name_len)) return -17;

    node = vfs_namei(path);
    if (!node) return -3;

    data_size = node->length;
    if (data_size < sizeof(Elf64_Ehdr)) { vfs_release(node); return -4; }

    data_pages = 0;
    mem_pages = 0;
    data = (uint8_t *)lke_alloc_pages(data_size, &data_pages);
    if (!data) { vfs_release(node); return -5; }

    if ((uint64_t)vfs_read(node, 0, data_size, data) != data_size) {
        vfs_release(node);
        lke_free_pages(data, data_pages);
        return -6;
    }
    vfs_release(node);

    ehdr = (const Elf64_Ehdr *)data;
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) {
        lke_free_pages(data, data_pages);
        return -7;
    }
    if (ehdr->e_type != ET_REL) {
        lke_free_pages(data, data_pages);
        return -8;
    }
    if (ehdr->e_machine != EM_X86_64) {
        lke_free_pages(data, data_pages);
        return -9;
    }

    if (ehdr->e_shoff + (uint64_t)ehdr->e_shnum * sizeof(Elf64_Shdr) > data_size) {
        lke_free_pages(data, data_pages);
        return -7;
    }

    shdr = (const Elf64_Shdr *)(data + ehdr->e_shoff);

    total_alloc = 0;
    for (i = 0; i < ehdr->e_shnum; i++) {
        sh = &shdr[i];
        if ((sh->sh_type == SHT_PROGBITS || sh->sh_type == SHT_NOBITS) &&
            (sh->sh_flags & 0x2)) {
            if (sh->sh_addralign > 1)
                total_alloc = (total_alloc + sh->sh_addralign - 1) & ~(sh->sh_addralign - 1);
            total_alloc += sh->sh_size;
        }
    }

    if (total_alloc == 0) {
        lke_free_pages(data, data_pages);
        return -10;
    }

    mem = (uint8_t *)lke_alloc_pages(total_alloc, &mem_pages);
    if (!mem) {
        lke_free_pages(data, data_pages);
        return -11;
    }

    sec_offsets = (uint64_t *)kmalloc(ehdr->e_shnum * sizeof(uint64_t));
    if (!sec_offsets) {
        lke_free_pages(mem, mem_pages);
        lke_free_pages(data, data_pages);
        return -12;
    }
    memset(sec_offsets, 0, ehdr->e_shnum * sizeof(uint64_t));

    offset = 0;
    for (i = 0; i < ehdr->e_shnum; i++) {
        sh = &shdr[i];
        if ((sh->sh_type == SHT_PROGBITS || sh->sh_type == SHT_NOBITS) &&
            (sh->sh_flags & 0x2)) {
            if (sh->sh_addralign > 1)
                offset = (offset + sh->sh_addralign - 1) & ~(sh->sh_addralign - 1);
            sec_offsets[i] = (uint64_t)(mem + offset) - 0;
            if (sh->sh_type == SHT_PROGBITS && sh->sh_size > 0) {
                memcpy(mem + offset, data + sh->sh_offset, sh->sh_size);
            }
            offset += sh->sh_size;
        }
    }

    symtab_idx = 0;
    strtab_idx = 0;
    for (i = 0; i < ehdr->e_shnum; i++) {
        if (shdr[i].sh_type == SHT_SYMTAB) {
            symtab_idx = i;
            strtab_idx = shdr[i].sh_link;
            break;
        }
    }

    if (symtab_idx == 0) {
        kfree(sec_offsets);
        lke_free_pages(mem, mem_pages);
        lke_free_pages(data, data_pages);
        return -13;
    }

    symtab_sh = &shdr[symtab_idx];
    strtab_sh = &shdr[strtab_idx];
    strtab = (const char *)(data + strtab_sh->sh_offset);

    for (i = 0; i < ehdr->e_shnum; i++) {
        rela_sh = &shdr[i];
        if (rela_sh->sh_type != SHT_RELA) continue;
        if (rela_sh->sh_info >= ehdr->e_shnum) continue;
        if (sec_offsets[rela_sh->sh_info] == 0) continue;

        for (j = 0; j < rela_sh->sh_size / sizeof(Elf64_Rela); j++) {
            rela = (const Elf64_Rela *)(data + rela_sh->sh_offset + j * sizeof(Elf64_Rela));
            sym_idx = ELF64_R_SYM(rela->r_info);
            rtype = ELF64_R_TYPE(rela->r_info);

            sym = (const Elf64_Sym *)(data + symtab_sh->sh_offset + sym_idx * sizeof(Elf64_Sym));
            sym_name = strtab + sym->st_name;

            S = 0;
            if (sym->st_shndx == 0 && sym->st_name != 0) {
                S = ksym_lookup(sym_name);
                if (S == 0) {
                    printf("LKE: unresolved symbol: %s\n", sym_name);
                    kfree(sec_offsets);
                    lke_free_pages(mem, mem_pages);
                    lke_free_pages(data, data_pages);
                    return -14;
                }
            } else if (sym->st_shndx != 0 && sym->st_shndx < ehdr->e_shnum) {
                S = sec_offsets[sym->st_shndx] + sym->st_value;
            }

            A = (uint64_t)rela->r_addend;
            P = sec_offsets[rela_sh->sh_info] + rela->r_offset;
            target = (uint8_t *)P;

            if (rtype == R_X86_64_64) {
                *(uint64_t *)target = S + A;
            } else if (rtype == R_X86_64_PC32 || rtype == R_X86_64_PLT32) {
                *(int32_t *)target = (int32_t)((int64_t)(S + A) - (int64_t)P);
            } else if (rtype == R_X86_64_32) {
                *(uint32_t *)target = (uint32_t)(S + A);
            } else if (rtype == R_X86_64_32S) {
                *(int32_t *)target = (int32_t)(S + A);
            } else {
                kfree(sec_offsets);
                lke_free_pages(mem, mem_pages);
                lke_free_pages(data, data_pages);
                return -15;
            }
        }
    }

    found_init = 0;
    init_fn = NULL;
    cleanup_fn = NULL;
    for (i = 0; i < symtab_sh->sh_size / sizeof(Elf64_Sym); i++) {
        sym = (const Elf64_Sym *)(data + symtab_sh->sh_offset + i * sizeof(Elf64_Sym));
        if (ELF64_ST_BIND(sym->st_info) != STB_GLOBAL) continue;
        if (ELF64_ST_TYPE(sym->st_info) != STT_FUNC) continue;
        if (sym->st_shndx == 0 || sym->st_shndx >= ehdr->e_shnum) continue;
        sym_name = strtab + sym->st_name;
        if (strcmp(sym_name, "lke_module_init") == 0) {
            init_fn = (int (*)(void))(
                sec_offsets[sym->st_shndx] + sym->st_value);
            found_init = 1;
        } else if (strcmp(sym_name, "lke_module_cleanup") == 0) {
            cleanup_fn = (void (*)(void))(
                sec_offsets[sym->st_shndx] + sym->st_value);
        }
    }

    kfree(sec_offsets);
    lke_free_pages(data, data_pages);

    if (!found_init) {
        lke_free_pages(mem, mem_pages);
        return -16;
    }

    mod_name = (char *)kmalloc(mod_name_len + 1);
    if (!mod_name) {
        lke_free_pages(mem, mem_pages);
        return -2;
    }
    memcpy(mod_name, bname, mod_name_len);
    mod_name[mod_name_len] = '\0';

    mod = lke_new_slot();
    if (!mod) {
        kfree(mod_name);
        lke_free_pages(mem, mem_pages);
        return -2;
    }
    memset(mod, 0, sizeof(lke_module_t));
    mod->name = mod_name;
    mod->text_base = mem;
    mod->text_pages = mem_pages;
    mod->cleanup = cleanup_fn;

    lke_count++;
    if (init_fn() != 0) {
        lke_count--;
        kfree(mod->name);
        lke_free_pages(mem, mem_pages);
        memset(mod, 0, sizeof(lke_module_t));
        lke_shrink_registry();
        return -17;
    }

    printf("LKE: loaded '%s' (%llu bytes)\n", mod->name, (unsigned long long)total_alloc);
    return 0;
}

int lke_unload(const char *name) {
    size_t idx;
    lke_module_t *mod;

    if (!name) return -1;

    mod = lke_find_by_name(name);
    if (!mod) return -2;
    idx = (size_t)(mod - modules);
    if (mod->cleanup) {
        mod->cleanup();
    }

    if (mod->text_base) {
        lke_free_pages(mod->text_base, mod->text_pages);
    }

    printf("LKE: unloaded '%s'\n", mod->name);
    kfree(mod->name);
    lke_count--;
    if (idx != lke_count) modules[idx] = modules[lke_count];
    memset(&modules[lke_count], 0, sizeof(lke_module_t));
    lke_shrink_registry();
    return 0;
}

int lke_list(char *buf, int size) {
    size_t i;
    size_t len;
    size_t required;
    size_t offset;

    required = 0;
    for (i = 0; i < lke_count; i++) {
        len = strlen(modules[i].name) + 1;
        if (len > (size_t)INT32_MAX) return -1;
        if (required > (size_t)INT32_MAX - len) return -1;
        required += len;
    }
    if (!buf || size < (int)required) return (int)required;
    offset = 0;
    for (i = 0; i < lke_count; i++) {
        len = strlen(modules[i].name) + 1;
        memcpy(buf + offset, modules[i].name, len);
        offset += len;
    }
    return (int)required;
}

static void lke_autoload_entry(char *line, size_t length) {
    int rc;

    while (length > 0 &&
           (line[length - 1] == ' ' || line[length - 1] == '\t')) {
        length--;
    }
    line[length] = '\0';
    if (length == 0 || line[0] == '#') return;
    rc = lke_load(line);
    if (rc < 0) printf("LKE: autoload failed: %s (%d)\n", line, rc);
}

void lke_autoload(void) {
    vfs_node_t *node;
    uint8_t buf[512];
    uint32_t rd;
    uint32_t off;
    uint32_t i;
    char *line;
    char *new_line;
    size_t line_len;
    size_t line_capacity;
    size_t new_capacity;
    int out_of_memory;

    node = vfs_namei("/etc/lke.autostart");
    if (!node || node->length == 0) {
        vfs_release(node);
        return;
    }

    line = NULL;
    line_len = 0;
    line_capacity = 0;
    out_of_memory = 0;
    off = 0;
    while (off < node->length) {
        rd = node->length - off;
        if (rd > sizeof(buf)) rd = sizeof(buf);
        rd = vfs_read(node, off, rd, buf);
        if (rd == 0) break;
        for (i = 0; i < rd; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                if (line) lke_autoload_entry(line, line_len);
                line_len = 0;
            } else {
                if (line_len == SIZE_MAX - 1) {
                    out_of_memory = 1;
                    break;
                }
                if (line_len + 1 >= line_capacity) {
                    new_capacity = line_capacity ? line_capacity * 2 : 64;
                    if (new_capacity <= line_capacity) {
                        out_of_memory = 1;
                        break;
                    }
                    new_line = (char *)krealloc(line, new_capacity);
                    if (!new_line) {
                        out_of_memory = 1;
                        break;
                    }
                    line = new_line;
                    line_capacity = new_capacity;
                }
                line[line_len++] = (char)buf[i];
            }
        }
        if (out_of_memory) break;
        off += rd;
    }

    if (!out_of_memory && line && line_len > 0)
        lke_autoload_entry(line, line_len);
    if (out_of_memory) printf("LKE: autoload stopped: out of memory\n");
    kfree(line);
    vfs_release(node);
}
