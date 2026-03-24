#include <kernel/lke.h>
#include <kernel/elf.h>
#include <kernel/vfs.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <string.h>

#define R_X86_64_32    10
#define R_X86_64_32S   11
#define R_X86_64_PLT32  4

static lke_module_t modules[LKE_MAX_MODULES];
static int lke_count = 0;

typedef struct {
    const char *name;
    uint64_t addr;
} lke_ksym_t;

extern void *kmalloc(size_t);
extern void kfree(void *);
extern int printf(const char *, ...);
extern void terminal_writestring(const char *);

static const lke_ksym_t ksym_table[] = {
    {"printf",             (uint64_t)&printf},
    {"kmalloc",            (uint64_t)&kmalloc},
    {"kfree",              (uint64_t)&kfree},
    {"terminal_writestring", (uint64_t)&terminal_writestring},
    {"memcpy",             (uint64_t)&memcpy},
    {"memset",             (uint64_t)&memset},
    {"strcmp",             (uint64_t)&strcmp},
    {"strlen",             (uint64_t)&strlen},
    {NULL, 0}
};

static uint64_t ksym_lookup(const char *name) {
    int i;
    for (i = 0; ksym_table[i].name; i++) {
        if (strcmp(ksym_table[i].name, name) == 0)
            return ksym_table[i].addr;
    }
    return 0;
}

void lke_init(void) {
    memset(modules, 0, sizeof(modules));
    lke_count = 0;
}

static int lke_find_slot(void) {
    int i;
    for (i = 0; i < LKE_MAX_MODULES; i++) {
        if (!modules[i].loaded) return i;
    }
    return -1;
}

static int lke_find_by_name(const char *name) {
    int i;
    for (i = 0; i < LKE_MAX_MODULES; i++) {
        if (modules[i].loaded && strcmp(modules[i].name, name) == 0) return i;
    }
    return -1;
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

static void strip_ext(char *dst, const char *src, uint64_t max) {
    const char *dot;
    uint64_t len;
    dot = NULL;
    for (len = 0; src[len]; len++) {
        if (src[len] == '.') dot = &src[len];
    }
    if (dot) len = (uint64_t)(dot - src);
    if (len >= max) len = max - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
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
    int slot;
    lke_module_t *mod;
    const char *bname;
    int (*init_fn)(void);
    const Elf64_Shdr *strtab_sh;
    uint64_t symtab_idx;
    uint64_t strtab_idx;
    const char *sym_name;
    int found_init;

    if (!path) return -1;

    slot = lke_find_slot();
    if (slot < 0) return -2;

    node = vfs_namei(path);
    if (!node) return -3;

    data_size = node->length;
    if (data_size < sizeof(Elf64_Ehdr)) return -4;

    data = (uint8_t *)kmalloc(data_size);
    if (!data) return -5;

    if ((uint64_t)vfs_read(node, 0, data_size, data) != data_size) {
        kfree(data);
        return -6;
    }

    ehdr = (const Elf64_Ehdr *)data;
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) {
        kfree(data);
        return -7;
    }
    if (ehdr->e_type != ET_REL) {
        kfree(data);
        return -8;
    }
    if (ehdr->e_machine != EM_X86_64) {
        kfree(data);
        return -9;
    }

    if (ehdr->e_shoff + (uint64_t)ehdr->e_shnum * sizeof(Elf64_Shdr) > data_size) {
        kfree(data);
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
        kfree(data);
        return -10;
    }

    mem = (uint8_t *)kmalloc(total_alloc);
    if (!mem) {
        kfree(data);
        return -11;
    }
    memset(mem, 0, total_alloc);

    sec_offsets = (uint64_t *)kmalloc(ehdr->e_shnum * sizeof(uint64_t));
    if (!sec_offsets) {
        kfree(mem);
        kfree(data);
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
        kfree(mem);
        kfree(data);
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
                    kfree(mem);
                    kfree(data);
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
                kfree(mem);
                kfree(data);
                return -15;
            }
        }
    }

    mod = &modules[slot];
    memset(mod, 0, sizeof(lke_module_t));

    bname = basename_of(path);
    strip_ext(mod->name, bname, LKE_NAME_MAX);

    mod->magic = LKE_MAGIC;
    mod->version = LKE_VERSION;
    mod->text_base = mem;
    mod->text_size = total_alloc;
    mod->init = NULL;
    mod->cleanup = NULL;

    found_init = 0;
    for (i = 0; i < symtab_sh->sh_size / sizeof(Elf64_Sym); i++) {
        sym = (const Elf64_Sym *)(data + symtab_sh->sh_offset + i * sizeof(Elf64_Sym));
        if (ELF64_ST_BIND(sym->st_info) != STB_GLOBAL) continue;
        if (ELF64_ST_TYPE(sym->st_info) != STT_FUNC) continue;
        if (sym->st_shndx == 0 || sym->st_shndx >= ehdr->e_shnum) continue;
        sym_name = strtab + sym->st_name;
        if (strcmp(sym_name, "lke_module_init") == 0) {
            mod->init = (int (*)(void))(sec_offsets[sym->st_shndx] + sym->st_value);
            found_init = 1;
        } else if (strcmp(sym_name, "lke_module_cleanup") == 0) {
            mod->cleanup = (void (*)(void))(sec_offsets[sym->st_shndx] + sym->st_value);
        }
    }

    kfree(sec_offsets);
    kfree(data);

    if (!found_init) {
        kfree(mem);
        return -16;
    }

    mod->loaded = 1;
    lke_count++;

    init_fn = mod->init;
    if (init_fn() != 0) {
        mod->loaded = 0;
        lke_count--;
        kfree(mem);
        return -17;
    }

    printf("LKE: loaded '%s' (%llu bytes)\n", mod->name, (unsigned long long)total_alloc);
    return 0;
}

int lke_unload(const char *name) {
    int idx;
    lke_module_t *mod;

    if (!name) return -1;

    idx = lke_find_by_name(name);
    if (idx < 0) return -2;

    mod = &modules[idx];
    if (mod->cleanup) {
        mod->cleanup();
    }

    if (mod->text_base) {
        kfree(mod->text_base);
    }

    printf("LKE: unloaded '%s'\n", mod->name);
    memset(mod, 0, sizeof(lke_module_t));
    lke_count--;
    return 0;
}

int lke_list(lke_info_t *buf, int max) {
    int count;
    int i;

    count = 0;
    for (i = 0; i < LKE_MAX_MODULES && count < max; i++) {
        if (modules[i].loaded) {
            memcpy(buf[count].name, modules[i].name, LKE_NAME_MAX);
            buf[count].loaded = 1;
            count++;
        }
    }
    return count;
}

void lke_autoload(void) {
    vfs_node_t *node;
    uint8_t buf[512];
    uint32_t rd;
    uint32_t off;
    uint32_t i;
    char line[128];
    int llen;
    int rc;

    node = vfs_namei("/etc/lke.autostart");
    if (!node || node->length == 0)
        return;

    llen = 0;
    off = 0;
    while (off < node->length) {
        rd = node->length - off;
        if (rd > sizeof(buf)) rd = sizeof(buf);
        rd = vfs_read(node, off, rd, buf);
        if (rd == 0) break;
        for (i = 0; i < rd; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                while (llen > 0 && (line[llen-1] == ' ' || line[llen-1] == '\t'))
                    llen--;
                line[llen] = '\0';
                if (llen > 0 && line[0] != '#') {
                    rc = lke_load(line);
                    if (rc < 0)
                        printf("LKE: autoload failed: %s (%d)\n", line, rc);
                }
                llen = 0;
            } else {
                if (llen < (int)sizeof(line) - 1)
                    line[llen++] = buf[i];
            }
        }
        off += rd;
    }

    if (llen > 0) {
        while (llen > 0 && (line[llen-1] == ' ' || line[llen-1] == '\t'))
            llen--;
        line[llen] = '\0';
        if (llen > 0 && line[0] != '#') {
            rc = lke_load(line);
            if (rc < 0)
                printf("LKE: autoload failed: %s (%d)\n", line, rc);
        }
    }
}
