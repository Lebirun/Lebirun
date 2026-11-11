#include <lebirun/lke.h>
#include <lebirun/elf.h>
#include <lebirun/vfs.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <lebirun/pit.h>
#include <lebirun/rng.h>
#include <lebirun/crypto.h>
#include <lebirun/task.h>
#include <lebirun/uaccess.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/drivers/net/netif.h>
#include <lebirun/drivers/net/ipv4.h>
#include <lebirun/drivers/net/dns.h>
#include <lebirun/drivers/net/tcp.h>
#include <lebirun/drivers/net/tls.h>
#include <lebirun/rtc.h>
#include <lebirun/drivers/net/udp.h>
#include <lebirun/common.h>
#if CONFIG_DRIVER_MODULES
#include <lebirun/acpi.h>
#include <lebirun/blkdev.h>
#include <lebirun/pci.h>
#include <lebirun/devfs.h>
#include <lebirun/evdev.h>
#include <lebirun/framebuffer.h>
#include <lebirun/idt.h>
#include <lebirun/keyboard.h>
#include <lebirun/mouse.h>
#include <lebirun/mutex.h>
#include <lebirun/smp.h>
#include <lebirun/timekeeping.h>
#endif
#include "syscalls/syscall_defs.h"
#include <string.h>

#define R_X86_64_32    10
#define R_X86_64_32S   11
#define R_X86_64_PLT32  4

static lke_module_t *modules;
static size_t lke_count;

int lke_register_syscall(int num, void *fn) {
    size_t i;
    lke_module_t *owner;
    int *grown;

    if (num < 0 || !fn) return -1;
    if (syscall_table_get(num) && syscall_table_get(num) != fn) return -2;
    syscall_table_set(num, fn);
    if (syscall_table_get(num) != fn) return -1;
    owner = NULL;
    for (i = 0; i < lke_count; i++) {
        uint64_t base = (uint64_t)(uintptr_t)modules[i].text_base;
        uint64_t span = modules[i].text_pages * PAGE_SIZE;
        uint64_t addr = (uint64_t)(uintptr_t)fn;
        if (base && span > 0 && addr >= base && addr < base + span) {
            owner = &modules[i];
            break;
        }
    }
    if (!owner) return 0;
    for (i = 0; i < (size_t)owner->owned_count; i++) {
        if (owner->owned_syscalls[i] == num) return 0;
    }
    if (owner->owned_count == owner->owned_capacity) {
        int new_capacity = owner->owned_capacity ? owner->owned_capacity * 2 : 4;
        grown = (int *)krealloc(owner->owned_syscalls,
                                (size_t)new_capacity * sizeof(int));
        if (!grown) {
            if (syscall_table_get(num) == fn)
                syscall_table_set(num, NULL);
            return -1;
        }
        owner->owned_syscalls = grown;
        owner->owned_capacity = new_capacity;
    }
    owner->owned_syscalls[owner->owned_count++] = num;
    return 0;
}

void lke_unregister_syscall(int num, void *fn) {
    size_t i;
    size_t j;
    uint64_t addr;

    if (num < 0) return;
    if (syscall_table_get(num) == fn) syscall_table_set(num, NULL);
    if (!fn) return;
    addr = (uint64_t)(uintptr_t)fn;
    for (i = 0; i < lke_count; i++) {
        uint64_t base = (uint64_t)(uintptr_t)modules[i].text_base;
        uint64_t span = modules[i].text_pages * PAGE_SIZE;
        if (!base || !span || addr < base || addr >= base + span)
            continue;
        for (j = 0; j < (size_t)modules[i].owned_count; j++) {
            if (modules[i].owned_syscalls[j] == num) {
                modules[i].owned_count--;
                if ((int)j != modules[i].owned_count)
                    modules[i].owned_syscalls[j] =
                        modules[i].owned_syscalls[modules[i].owned_count];
                return;
            }
        }
        return;
    }
}

#define KSYM(sym) { #sym, (const void *)&sym }

#if CONFIG_DRIVER_MODULES
extern int scheduler_initialized;
#endif

static const lke_ksym_t ksyms[] = {
    KSYM(kmalloc), KSYM(kmalloc_aligned), KSYM(kfree_aligned), KSYM(kfree), KSYM(krealloc),
    KSYM(printf), KSYM(snprintf), KSYM(memcpy), KSYM(memset),
    KSYM(memcmp), KSYM(memmove), KSYM(strcmp), KSYM(strlen),
    KSYM(strncpy), KSYM(vfs_namei), KSYM(vfs_read), KSYM(vfs_write),
    KSYM(vfs_release), KSYM(copy_from_user), KSYM(copy_to_user),
    KSYM(pit_get_ticks64), KSYM(pit_get_uptime_ms), KSYM(pit_get_ticks),
    KSYM(pit_ms_to_ticks), KSYM(rtc_get_time), KSYM(rng_get_u64),
    KSYM(sha256_hash), KSYM(hmac_sha256), KSYM(sha384_hash),
    KSYM(crypto_constant_compare), KSYM(vmm_get_phys_in_pml4),
    KSYM(task_current), KSYM(schedule), KSYM(task_has_pending_signals),
    KSYM(lke_register_syscall), KSYM(lke_unregister_syscall),
#if CONFIG_DRIVER_NET
    KSYM(netif_get_default), KSYM(netif_poll_all), KSYM(ipv4_is_local),
    KSYM(udp_send), KSYM(udp_send6), KSYM(udp_register_port_hook),
    KSYM(udp_unregister_port_hook), KSYM(dns_resolve_timeout),
    KSYM(dns_resolve6), KSYM(net_get_ticks), KSYM(tcp_socket_create),
    KSYM(tcp_socket_close), KSYM(tcp_connect), KSYM(tcp_send),
    KSYM(tcp_recv), KSYM(tcp_disconnect), KSYM(tls_register_provider),
    KSYM(tls_unregister_provider), KSYM(tls_get_provider),
#endif
#if CONFIG_DRIVER_MODULES
    KSYM(kcalloc), KSYM(strcpy), KSYM(pfa_alloc), KSYM(pfa_free),
    KSYM(pfa_alloc_contiguous), KSYM(pfa_free_contiguous),
    KSYM(pmm_alloc_mapped_page), KSYM(pt_ensure_phys_mapped),
    KSYM(vmm_map_page), KSYM(vmm_unmap_page), KSYM(vmm_get_kernel_cr3),
    KSYM(mutex_init), KSYM(mutex_lock), KSYM(mutex_unlock),
    KSYM(lock_scheduler), KSYM(unlock_scheduler), KSYM(add_task_to_runqueue),
    KSYM(create_kernel_task), KSYM(wake_task), KSYM(sleep_ms),
    KSYM(scheduler_initialized), KSYM(descriptor_ready_notify_irq),
    KSYM(smp_this_cpu), KSYM(smp_tlb_flush_poll), KSYM(tick_count),
    KSYM(pit_freq), KSYM(pit_register_callback),
    KSYM(pit_set_callback_interval), KSYM(tsc_available), KSYM(tsc_get_ns),
    KSYM(timekeeping_monotonic_ns), KSYM(irq_register_handler),
    KSYM(irq_unregister_handler), KSYM(irq_mask), KSYM(irq_unmask),
    KSYM(acpi_find_table), KSYM(acpi_read_phys), KSYM(blkdev_register),
    KSYM(blkdev_unregister), KSYM(blkdev_phys), KSYM(devfs_add_blkdevs),
    KSYM(devfs_remove_blkdevs), KSYM(keyboard_feed_ps2),
    KSYM(keyboard_usb_recent), KSYM(mouse_inject_packet),
    KSYM(mouse_get_packet_size), KSYM(mouse_usb_recent), KSYM(evdev_get_mouse),
    KSYM(evdev_mouse_set_abs), KSYM(evdev_push_event), KSYM(evdev_push_sync),
    KSYM(fb_get), KSYM(pci_get),
#if CONFIG_DRIVER_NET
    KSYM(net_register_driver), KSYM(netif_alloc), KSYM(netif_register),
    KSYM(netif_set_default), KSYM(netif_find), KSYM(eth_receive),
#endif
#endif
};

static int lke_enabled;
static int lke_booted;

static const void *ksym_lookup(const char *name) {
    extern uint8_t _kernel_init_start[];
    extern uint8_t _kernel_init_end[];
    const uint8_t *addr;
    size_t i;

    for (i = 0; i < sizeof(ksyms) / sizeof(ksyms[0]); i++) {
        if (strcmp(ksyms[i].name, name) != 0) continue;
        addr = (const uint8_t *)ksyms[i].addr;
        if (lke_booted && addr >= _kernel_init_start && addr < _kernel_init_end)
            return NULL;
        return addr;
    }
    return NULL;
}

void KERNEL_INIT lke_init(void) {
    lke_enabled = 1;
}

void lke_boot_done(void) {
    lke_booted = 1;
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

static int lke_range_ok(uint64_t off, uint64_t size, uint64_t total) {
    if (off > total || size > total - off) return 0;
    return 1;
}

static int lke_add_ok(uint64_t a, uint64_t b, uint64_t *out) {
    if (b > UINT64_MAX - a) return 0;
    *out = a + b;
    return 1;
}

static int lke_is_init_section(const uint8_t *data, uint64_t data_size,
                               const Elf64_Ehdr *ehdr,
                               const Elf64_Shdr *sh) {
    const Elf64_Shdr *names;

    if (ehdr->e_shstrndx >= ehdr->e_shnum) return 0;
    names = (const Elf64_Shdr *)(data + ehdr->e_shoff) + ehdr->e_shstrndx;
    if (!lke_range_ok(names->sh_offset, names->sh_size, data_size) ||
        sh->sh_name >= names->sh_size || names->sh_size - sh->sh_name < 6)
        return 0;
    return memcmp(data + names->sh_offset + sh->sh_name, ".init.", 6) == 0;
}

int lke_load(const char *path) {
    vfs_node_t *node;
    uint64_t data_size;
    uint8_t *data;
    const Elf64_Ehdr *ehdr;
    const Elf64_Shdr *shdr;
    const Elf64_Shdr *sh;
    const Elf64_Shdr *rela_sh;
    const Elf64_Shdr *symtab_sh;
    const Elf64_Shdr *strtab_sh;
    const Elf64_Rela *rela;
    const Elf64_Sym *sym;
    const char *strtab;
    const char *sym_name;
    const char *bname;
    uint64_t *sec_offsets;
    uint64_t size[2];
    uint64_t off[2];
    uint64_t pages[2];
    uint8_t *mem[2];
    uint64_t data_pages;
    uint64_t sym_count;
    uint64_t i;
    uint64_t j;
    uint64_t S;
    uint64_t A;
    uint64_t P;
    uint64_t rtype;
    uint8_t *target;
    lke_module_t *mod;
    char *mod_name;
    size_t mod_name_len;
    int (*init_fn)(void);
    void (*cleanup_fn)(void);
    int pass;
    int r;
    int rc;

    if (!path) return -22;
    if (!lke_enabled) return -38;

    bname = basename_of(path);
    mod_name_len = lke_name_length(bname);
    if (lke_find_by_name_slice(bname, mod_name_len)) return -17;

    node = vfs_namei(path);
    if (!node) return -2;

    data_size = node->length;
    if (data_size < sizeof(Elf64_Ehdr)) { vfs_release(node); return -8; }

    data = (uint8_t *)lke_alloc_pages(data_size, &data_pages);
    if (!data) { vfs_release(node); return -12; }

    if ((uint64_t)vfs_read(node, 0, data_size, data) != data_size) {
        vfs_release(node);
        lke_free_pages(data, data_pages);
        return -5;
    }
    vfs_release(node);

    sec_offsets = NULL;
    mem[0] = mem[1] = NULL;
    pages[0] = pages[1] = 0;
    size[0] = size[1] = 0;
    rc = -8;
    ehdr = (const Elf64_Ehdr *)data;
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3)
        goto fail;
    if (ehdr->e_type != ET_REL) goto fail;
    if (ehdr->e_machine != EM_X86_64) goto fail;
    if (ehdr->e_shoff > data_size ||
        (uint64_t)ehdr->e_shnum * sizeof(Elf64_Shdr) > data_size - ehdr->e_shoff)
        goto fail;

    shdr = (const Elf64_Shdr *)(data + ehdr->e_shoff);
    rc = -12;
    sec_offsets = (uint64_t *)kmalloc(ehdr->e_shnum * sizeof(uint64_t));
    if (!sec_offsets) goto fail;
    memset(sec_offsets, 0, ehdr->e_shnum * sizeof(uint64_t));

    for (pass = 0; pass < 2; pass++) {
        off[0] = off[1] = 0;
        for (i = 0; i < ehdr->e_shnum; i++) {
            sh = &shdr[i];
            if ((sh->sh_type != SHT_PROGBITS && sh->sh_type != SHT_NOBITS) ||
                !(sh->sh_flags & 0x2))
                continue;
            r = lke_is_init_section(data, data_size, ehdr, sh);
            rc = -8;
            if (sh->sh_addralign > 1) {
                uint64_t mask = sh->sh_addralign - 1;

                if ((sh->sh_addralign & mask) != 0 ||
                    mask > UINT64_MAX - off[r])
                    goto fail;
                off[r] = (off[r] + mask) & ~mask;
            }
            if (pass) {
                sec_offsets[i] = (uint64_t)(uintptr_t)(mem[r] + off[r]);
                if (sh->sh_type == SHT_PROGBITS && sh->sh_size > 0) {
                    if (!lke_range_ok(sh->sh_offset, sh->sh_size, data_size))
                        goto fail;
                    memcpy(mem[r] + off[r], data + sh->sh_offset, sh->sh_size);
                }
            }
            if (!lke_add_ok(off[r], sh->sh_size, &off[r])) goto fail;
        }
        if (pass) break;
        size[0] = off[0];
        size[1] = off[1];
        rc = -8;
        if (size[0] == 0) goto fail;
        rc = -12;
        mem[0] = (uint8_t *)lke_alloc_pages(size[0], &pages[0]);
        if (!mem[0]) goto fail;
        if (size[1]) {
            mem[1] = (uint8_t *)lke_alloc_pages(size[1], &pages[1]);
            if (!mem[1]) goto fail;
        }
    }

    rc = -8;
    symtab_sh = NULL;
    for (i = 0; i < ehdr->e_shnum; i++) {
        if (shdr[i].sh_type == SHT_SYMTAB) {
            symtab_sh = &shdr[i];
            break;
        }
    }
    if (!symtab_sh || symtab_sh->sh_link >= ehdr->e_shnum) goto fail;
    strtab_sh = &shdr[symtab_sh->sh_link];
    if (!lke_range_ok(symtab_sh->sh_offset, symtab_sh->sh_size, data_size) ||
        !lke_range_ok(strtab_sh->sh_offset, strtab_sh->sh_size, data_size))
        goto fail;
    strtab = (const char *)(data + strtab_sh->sh_offset);
    sym_count = symtab_sh->sh_size / sizeof(Elf64_Sym);

    for (i = 0; i < ehdr->e_shnum; i++) {
        rela_sh = &shdr[i];
        if (rela_sh->sh_type != SHT_RELA) continue;
        if (rela_sh->sh_info >= ehdr->e_shnum) continue;
        if (sec_offsets[rela_sh->sh_info] == 0) continue;
        if (!lke_range_ok(rela_sh->sh_offset, rela_sh->sh_size, data_size))
            goto fail;

        for (j = 0; j < rela_sh->sh_size / sizeof(Elf64_Rela); j++) {
            rela = (const Elf64_Rela *)(data + rela_sh->sh_offset + j * sizeof(Elf64_Rela));
            if (ELF64_R_SYM(rela->r_info) >= sym_count) goto fail;
            sym = (const Elf64_Sym *)(data + symtab_sh->sh_offset) +
                  ELF64_R_SYM(rela->r_info);
            if (sym->st_name >= strtab_sh->sh_size) goto fail;
            sym_name = strtab + sym->st_name;

            S = 0;
            if (sym->st_shndx == 0 && sym->st_name != 0) {
                S = (uint64_t)(uintptr_t)ksym_lookup(sym_name);
                if (S == 0) {
                    printf("LKE: unresolved symbol: %s\n", sym_name);
                    goto fail;
                }
            } else if (sym->st_shndx != 0 && sym->st_shndx < ehdr->e_shnum) {
                S = sec_offsets[sym->st_shndx] + sym->st_value;
            }

            A = (uint64_t)rela->r_addend;
            P = sec_offsets[rela_sh->sh_info] + rela->r_offset;
            target = (uint8_t *)P;
            rtype = ELF64_R_TYPE(rela->r_info);

            if (rtype == R_X86_64_64) {
                *(uint64_t *)target = S + A;
            } else if (rtype == R_X86_64_PC32 || rtype == R_X86_64_PLT32) {
                *(int32_t *)target = (int32_t)((int64_t)(S + A) - (int64_t)P);
            } else if (rtype == R_X86_64_32) {
                *(uint32_t *)target = (uint32_t)(S + A);
            } else if (rtype == R_X86_64_32S) {
                *(int32_t *)target = (int32_t)(S + A);
            } else {
                goto fail;
            }
        }
    }

    init_fn = NULL;
    cleanup_fn = NULL;
    for (i = 0; i < sym_count; i++) {
        sym = (const Elf64_Sym *)(data + symtab_sh->sh_offset) + i;
        if (ELF64_ST_BIND(sym->st_info) != STB_GLOBAL) continue;
        if (ELF64_ST_TYPE(sym->st_info) != STT_FUNC) continue;
        if (sym->st_shndx == 0 || sym->st_shndx >= ehdr->e_shnum) continue;
        if (sym->st_name >= strtab_sh->sh_size) continue;
        sym_name = strtab + sym->st_name;
        if (strcmp(sym_name, "lke_module_init") == 0) {
            init_fn = (int (*)(void))(
                sec_offsets[sym->st_shndx] + sym->st_value);
        } else if (strcmp(sym_name, "lke_module_cleanup") == 0) {
            cleanup_fn = (void (*)(void))(
                sec_offsets[sym->st_shndx] + sym->st_value);
        }
    }

    if (!init_fn) goto fail;
    rc = -12;
    mod_name = (char *)kmalloc(mod_name_len + 1);
    if (!mod_name) goto fail;
    memcpy(mod_name, bname, mod_name_len);
    mod_name[mod_name_len] = '\0';

    mod = lke_new_slot();
    if (!mod) {
        kfree(mod_name);
        goto fail;
    }
    kfree(sec_offsets);
    lke_free_pages(data, data_pages);

    mod->name = mod_name;
    mod->text_base = mem[0];
    mod->text_pages = pages[0];
    mod->cleanup = cleanup_fn;

    lke_count++;
    rc = init_fn();
    lke_free_pages(mem[1], pages[1]);
    if (rc != 0) {
        int k;

        lke_count--;
        for (k = 0; k < mod->owned_count; k++) {
            void *installed = syscall_table_get(mod->owned_syscalls[k]);
            uint64_t base = (uint64_t)(uintptr_t)mod->text_base;
            uint64_t addr = (uint64_t)(uintptr_t)installed;
            if (addr >= base &&
                addr < base + mod->text_pages * PAGE_SIZE)
                syscall_table_set(mod->owned_syscalls[k], NULL);
        }
        kfree(mod->owned_syscalls);
        kfree(mod->name);
        lke_free_pages(mem[0], pages[0]);
        memset(mod, 0, sizeof(lke_module_t));
        lke_shrink_registry();
        return rc < 0 ? rc : -5;
    }

    printf("LKE: loaded '%s' (%llu bytes)\n", mod->name,
           (unsigned long long)size[0]);
    return 0;

fail:
    kfree(sec_offsets);
    lke_free_pages(mem[1], pages[1]);
    lke_free_pages(mem[0], pages[0]);
    lke_free_pages(data, data_pages);
    return rc;
}

int lke_unload(const char *name) {
    size_t idx;
    lke_module_t *mod;

    if (!name) return -22;

    mod = lke_find_by_name(name);
    if (!mod) return -2;
    idx = (size_t)(mod - modules);
    if (mod->owned_count > 0 || !mod->cleanup) return -16;
    mod->cleanup();

    if (mod->text_base) {
        lke_free_pages(mod->text_base, mod->text_pages);
    }

    printf("LKE: unloaded '%s'\n", mod->name);
    kfree(mod->name);
    kfree(mod->owned_syscalls);
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

static void KERNEL_INIT lke_autoload_entry(char *line, size_t length) {
    int rc;

    while (length > 0 &&
           (line[length - 1] == ' ' || line[length - 1] == '\t')) {
        length--;
    }
    line[length] = '\0';
    if (length == 0 || line[0] == '#') return;
    rc = lke_load(line);
    if (rc < 0) KERNEL_INIT_LOG("LKE: autoload failed: %s (%d)\n", line, rc);
}

void KERNEL_INIT lke_autoload(void) {
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
    if (out_of_memory) KERNEL_INIT_LOG("LKE: autoload stopped: out of memory\n");
    kfree(line);
    vfs_release(node);
}
