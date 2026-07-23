#include <lebirun/elf.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <lebirun/vfs.h>
#include <lebirun/task.h>
#include <string.h>
#include <stdio.h>

#define ELF_STREAM_CHUNK_SIZE 512

int elf_validate(const uint8_t *data, uint64_t size) {
    if (!data || size < sizeof(Elf64_Ehdr)) {
        return -1;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;

    if (ehdr->e_ident[0] != ELFMAG0 ||
        ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 ||
        ehdr->e_ident[3] != ELFMAG3) {
        return -2;
    }

    if (ehdr->e_ident[4] != ELFCLASS64) {
        return -3;
    }

    if (ehdr->e_ident[5] != ELFDATA2LSB) {
        return -4;
    }

    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        return -5;
    }

    if (ehdr->e_machine != EM_X86_64) {
        return -6;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return -7;
    }

    if (ehdr->e_phoff + ehdr->e_phnum * sizeof(Elf64_Phdr) > size) {
        return -8;
    }

    if (ehdr->e_entry < 0x1000) {
        return -9;
    }

    return 0;
}

int elf_validate_so(const uint8_t *data, uint64_t size) {
    if (!data || size < sizeof(Elf64_Ehdr)) {
        return -1;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;

    if (ehdr->e_ident[0] != ELFMAG0 ||
        ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 ||
        ehdr->e_ident[3] != ELFMAG3) {
        return -2;
    }

    if (ehdr->e_ident[4] != ELFCLASS64) {
        return -3;
    }

    if (ehdr->e_ident[5] != ELFDATA2LSB) {
        return -4;
    }

    if (ehdr->e_type != ET_DYN && ehdr->e_type != ET_EXEC) {
        return -5;
    }

    if (ehdr->e_machine != EM_X86_64) {
        return -6;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return -7;
    }

    if (ehdr->e_phoff + ehdr->e_phnum * sizeof(Elf64_Phdr) > size) {
        return -8;
    }

    return 0;
}

uint64_t elf_get_entry(const uint8_t *data) {
    if (!data) return 0;
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;
    return ehdr->e_entry;
}

static uint64_t count_loadable_pages(const uint8_t *data) {
    const Elf64_Ehdr *ehdr;
    const Elf64_Phdr *phdr;
    uint64_t total_pages;
    uint16_t i;
    uint64_t vaddr_start;
    uint64_t vaddr_end;

    ehdr = (const Elf64_Ehdr *)data;
    phdr = (const Elf64_Phdr *)(data + ehdr->e_phoff);
    total_pages = 0;

    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && phdr[i].p_memsz > 0) {
            vaddr_start = phdr[i].p_vaddr & ~0xFFFu;
            vaddr_end = (phdr[i].p_vaddr + phdr[i].p_memsz + 0xFFF) & ~0xFFFu;
            total_pages += (vaddr_end - vaddr_start) / PAGE_SIZE;
        }
    }

    return total_pages;
}

int elf_load_to_pd(uint64_t pd_phys, const uint8_t *data, uint64_t size, elf_info_t *info, uint64_t **out_pages, uint64_t *out_page_count) {
    int valid;
    const Elf64_Ehdr *ehdr;
    const Elf64_Phdr *phdr;
    uint64_t estimated_pages;
    uint64_t *page_list;
    uint64_t page_index;
    uint16_t i;
    uint64_t j;
    uint64_t vaddr;
    uint64_t memsz;
    uint64_t filesz;
    uint64_t offset;
    uint64_t flags;
    uint64_t vaddr_start;
    uint64_t vaddr_end;
    uint64_t segment_pages;
    uint64_t page;
    uint64_t page_vaddr;
    uint64_t existing_phys;
    uint64_t phys;
    uint64_t pie_base;
    int is_pie;

    if (!pd_phys || !data || !info) {
        return -1;
    }

    valid = elf_validate(data, size);
    if (valid != 0) {
        return valid;
    }

    ehdr = (const Elf64_Ehdr *)data;
    phdr = (const Elf64_Phdr *)(data + ehdr->e_phoff);

    is_pie = (ehdr->e_type == ET_DYN) ? 1 : 0;
    pie_base = is_pie ? 0x400000ULL : 0;

    info->entry_point = ehdr->e_entry + pie_base;
    info->load_base = 0xFFFFFFFFFFFFFFFFULL;
    info->load_end = 0;
    info->bss_end = 0;
    info->phent = ehdr->e_phentsize;
    info->phnum = ehdr->e_phnum;
    info->phdr_vaddr = 0;

    estimated_pages = count_loadable_pages(data);
    page_list = NULL;
    page_index = 0;

    if (out_pages && out_page_count) {
        page_list = (uint64_t *)kmalloc(estimated_pages * sizeof(uint64_t));
        if (!page_list) {
            return -10;
        }
        memset(page_list, 0, estimated_pages * sizeof(uint64_t));
    }

    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_PHDR) {
            info->phdr_vaddr = phdr[i].p_vaddr + pie_base;
            break;
        }
    }

    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) {
            continue;
        }

        if (phdr[i].p_memsz == 0) {
            continue;
        }

        vaddr = phdr[i].p_vaddr + pie_base;
        memsz = phdr[i].p_memsz;
        filesz = phdr[i].p_filesz;
        offset = phdr[i].p_offset;


        if (offset + filesz > size) {
            if (page_list) {
                for (j = 0; j < page_index; j++) {
                    pfa_free(page_list[j]);
                }
                kfree(page_list);
            }
            return -11;
        }

        flags = 0x5;
        if (phdr[i].p_flags & PF_W) {
            flags |= 0x2;
        }
        if (!(phdr[i].p_flags & PF_X) || (phdr[i].p_flags & PF_W)) {
            flags |= VMM_PTE_NX;
        }

        vaddr_start = vaddr & ~0xFFFu;
        vaddr_end = (vaddr + memsz + 0xFFF) & ~0xFFFu;
        segment_pages = (vaddr_end - vaddr_start) / PAGE_SIZE;

        for (page = 0; page < segment_pages; page++) {
            page_vaddr = vaddr_start + page * PAGE_SIZE;
            
            existing_phys = vmm_get_phys_in_pml4(pd_phys, page_vaddr);
            if (existing_phys != 0) {
                continue;
            }
            
            phys = pfa_alloc();
            if (!phys) {
                if (page_list) {
                    for (j = 0; j < page_index; j++) {
                        pfa_free(page_list[j]);
                    }
                    kfree(page_list);
                }
                return -12;
            }

            pmm_zero_page_phys(phys);
            vmm_map_page_in_pml4(pd_phys, page_vaddr, phys, flags);

            if (page_list && page_index < estimated_pages) {
                page_list[page_index++] = phys;
            }
        }

        if (filesz > 0) {
            vmm_copy_to_pml4(pd_phys, vaddr, data + offset, filesz);
        }

        if (vaddr < info->load_base) {
            info->load_base = vaddr;
        }
        if (vaddr + filesz > info->load_end) {
            info->load_end = vaddr + filesz;
        }
        if (vaddr + memsz > info->bss_end) {
            info->bss_end = vaddr + memsz;
        }
    }

    if (out_pages) {
        *out_pages = page_list;
    }
    if (out_page_count) {
        *out_page_count = page_index;
    }

    if (info->phdr_vaddr == 0 && info->load_base != 0xFFFFFFFFFFFFFFFFULL) {
        info->phnum = 0;
    }

    if (is_pie) {
        const Elf64_Dyn *dyn;
        uint64_t rela_off = 0;
        uint64_t rela_sz = 0;
        const Elf64_Rela *rel;
        uint64_t rel_cnt;
        uint64_t ri;
        uint64_t rtype;
        uint64_t raddr;
        uint64_t rval;

        for (i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].p_type == PT_DYNAMIC) {
                dyn = (const Elf64_Dyn *)(data + phdr[i].p_offset);
                while (dyn->d_tag != DT_NULL) {
                    if (dyn->d_tag == DT_RELA) {
                        rela_off = dyn->d_un.d_ptr;
                    } else if (dyn->d_tag == DT_RELASZ) {
                        rela_sz = dyn->d_un.d_val;
                    }
                    dyn++;
                }
                break;
            }
        }

        if (rela_sz > 0 && rela_off < size) {
            rel = (const Elf64_Rela *)(data + rela_off);
            rel_cnt = rela_sz / sizeof(Elf64_Rela);
            for (ri = 0; ri < rel_cnt; ri++) {
                rtype = ELF64_R_TYPE(rel[ri].r_info);
                if (rtype == R_X86_64_RELATIVE) {
                    raddr = pie_base + rel[ri].r_offset;
                    rval = pie_base + rel[ri].r_addend;
                    vmm_copy_to_pml4(pd_phys, raddr, &rval, sizeof(uint64_t));
                }
            }
        }
    }

    return 0;
}

static uint64_t count_loadable_pages_from_phdr(const Elf64_Phdr *phdr, uint16_t phnum) {
    uint64_t total_pages;
    uint16_t i;
    uint64_t vaddr_start;
    uint64_t vaddr_end;

    total_pages = 0;
    for (i = 0; i < phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && phdr[i].p_memsz > 0) {
            vaddr_start = phdr[i].p_vaddr & ~0xFFFu;
            vaddr_end = (phdr[i].p_vaddr + phdr[i].p_memsz + 0xFFF) & ~0xFFFu;
            total_pages += (vaddr_end - vaddr_start) / PAGE_SIZE;
        }
    }
    return total_pages;
}

static int elf_read_exact(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer) {
    uint64_t read_len;

    read_len = vfs_read(node, offset, size, (uint8_t *)buffer);
    if (read_len != size) {
        return -1;
    }
    return 0;
}

int elf_load_node_to_pd(uint64_t pd_phys, vfs_node_t *node, elf_info_t *info, uint64_t **out_pages, uint64_t *out_page_count) {
    Elf64_Ehdr ehdr;
    Elf64_Phdr *phdr;
    uint64_t phdr_size;
    uint64_t estimated_pages;
    uint64_t *page_list;
    uint64_t page_index;
    uint16_t i;
    uint64_t j;
    uint64_t vaddr;
    uint64_t memsz;
    uint64_t filesz;
    uint64_t offset;
    uint64_t flags;
    uint64_t vaddr_start;
    uint64_t vaddr_end;
    uint64_t segment_pages;
    uint64_t page;
    uint64_t page_vaddr;
    uint64_t existing_phys;
    uint64_t phys;
    uint64_t pie_base;
    uint64_t copied;
    uint64_t chunk;
    uint8_t tmp[ELF_STREAM_CHUNK_SIZE];
    int is_pie;
    int ret;
    Elf64_Dyn dyn;
    Elf64_Rela rel;
    uint64_t rela_off;
    uint64_t rela_sz;
    uint64_t rel_cnt;
    uint64_t ri;
    uint64_t rtype;
    uint64_t raddr;
    uint64_t rval;
    uint64_t tmp_size;

    if (!pd_phys || !node || !info) {
        return -1;
    }
    if (node->length < sizeof(Elf64_Ehdr)) {
        return -1;
    }
    if (elf_read_exact(node, 0, sizeof(ehdr), &ehdr) != 0) {
        return -2;
    }
    if (ehdr.e_ident[0] != ELFMAG0 ||
        ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 ||
        ehdr.e_ident[3] != ELFMAG3) {
        return -2;
    }
    if (ehdr.e_ident[4] != ELFCLASS64) {
        return -3;
    }
    if (ehdr.e_ident[5] != ELFDATA2LSB) {
        return -4;
    }
    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
        return -5;
    }
    if (ehdr.e_machine != EM_X86_64) {
        return -6;
    }
    if (ehdr.e_phoff == 0 || ehdr.e_phnum == 0) {
        return -7;
    }
    if (ehdr.e_phentsize != sizeof(Elf64_Phdr)) {
        return -8;
    }
    phdr_size = (uint64_t)ehdr.e_phnum * sizeof(Elf64_Phdr);
    if (ehdr.e_phoff + phdr_size > node->length) {
        return -8;
    }
    if (ehdr.e_entry < 0x1000) {
        return -9;
    }

    phdr = (Elf64_Phdr *)kmalloc(phdr_size);
    if (!phdr) {
        return -10;
    }
    if (elf_read_exact(node, ehdr.e_phoff, phdr_size, phdr) != 0) {
        kfree(phdr);
        return -8;
    }

    is_pie = (ehdr.e_type == ET_DYN) ? 1 : 0;
    pie_base = is_pie ? 0x400000ULL : 0;

    info->entry_point = ehdr.e_entry + pie_base;
    info->load_base = 0xFFFFFFFFFFFFFFFFULL;
    info->load_end = 0;
    info->bss_end = 0;
    info->phent = ehdr.e_phentsize;
    info->phnum = ehdr.e_phnum;
    info->phdr_vaddr = 0;

    if (!is_pie && current_task && current_task->is_user) {
        for (i = 0; i < ehdr.e_phnum; i++) {
            if (phdr[i].p_type == PT_PHDR) {
                info->phdr_vaddr = phdr[i].p_vaddr;
                break;
            }
        }

        for (i = 0; i < ehdr.e_phnum; i++) {
            if (phdr[i].p_type != PT_LOAD || phdr[i].p_memsz == 0) {
                continue;
            }
            flags = 0x5;
            if (phdr[i].p_flags & PF_W) {
                flags |= 0x2;
            }
            if (task_add_file_mapping(current_task, node, phdr[i].p_vaddr,
                                      phdr[i].p_memsz, phdr[i].p_filesz,
                                      phdr[i].p_offset, flags) != 0) {
                kfree(phdr);
                return -10;
            }
            vaddr = phdr[i].p_vaddr;
            if (vaddr < info->load_base) {
                info->load_base = vaddr;
            }
            if (vaddr + phdr[i].p_filesz > info->load_end) {
                info->load_end = vaddr + phdr[i].p_filesz;
            }
            if (vaddr + phdr[i].p_memsz > info->bss_end) {
                info->bss_end = vaddr + phdr[i].p_memsz;
            }
        }

        if (info->phdr_vaddr == 0 && info->load_base != 0xFFFFFFFFFFFFFFFFULL) {
            info->phnum = 0;
        }
        if (out_pages) {
            *out_pages = NULL;
        }
        if (out_page_count) {
            *out_page_count = 0;
        }
        kfree(phdr);
        return 0;
    }

    estimated_pages = count_loadable_pages_from_phdr(phdr, ehdr.e_phnum);
    page_list = NULL;
    page_index = 0;
    tmp_size = sizeof(tmp);
    ret = 0;

    if (out_pages && out_page_count) {
        page_list = (uint64_t *)kmalloc(estimated_pages * sizeof(uint64_t));
        if (!page_list) {
            kfree(phdr);
            return -10;
        }
        memset(page_list, 0, estimated_pages * sizeof(uint64_t));
    }

    for (i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].p_type == PT_PHDR) {
            info->phdr_vaddr = phdr[i].p_vaddr + pie_base;
            break;
        }
    }

    for (i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD || phdr[i].p_memsz == 0) {
            continue;
        }

        vaddr = phdr[i].p_vaddr + pie_base;
        memsz = phdr[i].p_memsz;
        filesz = phdr[i].p_filesz;
        offset = phdr[i].p_offset;

        if (offset + filesz > node->length) {
            ret = -11;
            break;
        }

        flags = 0x5;
        if (phdr[i].p_flags & PF_W) {
            flags |= 0x2;
        }
        if (!(phdr[i].p_flags & PF_X) || (phdr[i].p_flags & PF_W)) {
            flags |= VMM_PTE_NX;
        }

        vaddr_start = vaddr & ~0xFFFu;
        vaddr_end = (vaddr + memsz + 0xFFF) & ~0xFFFu;
        segment_pages = (vaddr_end - vaddr_start) / PAGE_SIZE;

        for (page = 0; page < segment_pages; page++) {
            page_vaddr = vaddr_start + page * PAGE_SIZE;
            existing_phys = vmm_get_phys_in_pml4(pd_phys, page_vaddr);
            if (existing_phys != 0) {
                continue;
            }
            phys = pfa_alloc();
            if (!phys) {
                ret = -12;
                break;
            }
            pmm_zero_page_phys(phys);
            vmm_map_page_in_pml4(pd_phys, page_vaddr, phys, flags);
            if (page_list && page_index < estimated_pages) {
                page_list[page_index++] = phys;
            }
        }
        if (ret != 0) {
            break;
        }

        copied = 0;
        while (copied < filesz) {
            chunk = filesz - copied;
            if (chunk > tmp_size) {
                chunk = tmp_size;
            }
            if (elf_read_exact(node, offset + copied, chunk, tmp) != 0) {
                ret = -11;
                break;
            }
            vmm_copy_to_pml4(pd_phys, vaddr + copied, tmp, chunk);
            copied += chunk;
        }
        if (ret != 0) {
            break;
        }

        if (vaddr < info->load_base) {
            info->load_base = vaddr;
        }
        if (vaddr + filesz > info->load_end) {
            info->load_end = vaddr + filesz;
        }
        if (vaddr + memsz > info->bss_end) {
            info->bss_end = vaddr + memsz;
        }
    }

    if (ret == 0 && info->phdr_vaddr == 0 && info->load_base != 0xFFFFFFFFFFFFFFFFULL) {
        info->phnum = 0;
    }

    if (ret == 0 && is_pie) {
        rela_off = 0;
        rela_sz = 0;
        for (i = 0; i < ehdr.e_phnum; i++) {
            if (phdr[i].p_type == PT_DYNAMIC) {
                copied = 0;
                while (copied + sizeof(Elf64_Dyn) <= phdr[i].p_filesz) {
                    if (elf_read_exact(node, phdr[i].p_offset + copied, sizeof(dyn), &dyn) != 0) {
                        ret = -11;
                        break;
                    }
                    if (dyn.d_tag == DT_NULL) {
                        break;
                    }
                    if (dyn.d_tag == DT_RELA) {
                        rela_off = dyn.d_un.d_ptr;
                    } else if (dyn.d_tag == DT_RELASZ) {
                        rela_sz = dyn.d_un.d_val;
                    }
                    copied += sizeof(Elf64_Dyn);
                }
                break;
            }
        }
        if (ret == 0 && rela_sz > 0 && rela_off < node->length) {
            rel_cnt = rela_sz / sizeof(Elf64_Rela);
            for (ri = 0; ri < rel_cnt; ri++) {
                if (elf_read_exact(node, rela_off + ri * sizeof(Elf64_Rela), sizeof(rel), &rel) != 0) {
                    ret = -11;
                    break;
                }
                rtype = ELF64_R_TYPE(rel.r_info);
                if (rtype == R_X86_64_RELATIVE) {
                    raddr = pie_base + rel.r_offset;
                    rval = pie_base + rel.r_addend;
                    vmm_copy_to_pml4(pd_phys, raddr, &rval, sizeof(uint64_t));
                }
            }
        }
    }

    if (ret != 0) {
        if (page_list) {
            for (j = 0; j < page_index; j++) {
                pfa_free(page_list[j]);
            }
            kfree(page_list);
        }
        kfree(phdr);
        return ret;
    }

    if (out_pages) {
        *out_pages = page_list;
    }
    if (out_page_count) {
        *out_page_count = page_index;
    }

    kfree(phdr);
    return 0;
}

static int strcmp_local(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

int elf_load_so(uint64_t pd_phys, const uint8_t *data, uint64_t size, uint64_t base_addr, dl_handle_t *handle) {
    int valid;
    const Elf64_Ehdr *ehdr;
    const Elf64_Phdr *phdr;
    uint64_t load_start;
    uint64_t load_end_val;
    uint16_t i;
    uint64_t j;
    uint64_t seg_end;
    uint64_t total_size;
    uint64_t total_pages;
    uint64_t vaddr;
    uint64_t memsz;
    uint64_t filesz;
    uint64_t offset;
    uint64_t flags;
    uint64_t vaddr_start;
    uint64_t vaddr_end;
    uint64_t segment_pages;
    uint64_t page;
    uint64_t page_vaddr;
    uint64_t phys;
    const Elf64_Shdr *shdr;
    uint64_t sym_count;
    const Elf64_Shdr *strtab_sh;

    const Elf64_Dyn *dyn;
    uint64_t rel_offset;
    uint64_t rel_size;
    const Elf64_Rela *rel;
    uint64_t rel_count;
    uint64_t r;
    uint64_t type;
    uint64_t addr;
    uint64_t value;
    uint64_t dyn_strtab_offset;
    uint64_t dyn_strtab_size;
    uint64_t needed_offsets[16];
    int needed_count;

    if (!pd_phys || !data || !handle) {
        return -1;
    }

    valid = elf_validate_so(data, size);
    if (valid != 0) {
        return valid;
    }

    ehdr = (const Elf64_Ehdr *)data;
    phdr = (const Elf64_Phdr *)(data + ehdr->e_phoff);

    load_start = 0xFFFFFFFFFFFFFFFFULL;
    load_end_val = 0;

    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && phdr[i].p_memsz > 0) {
            if (phdr[i].p_vaddr < load_start) {
                load_start = phdr[i].p_vaddr;
            }
            seg_end = phdr[i].p_vaddr + phdr[i].p_memsz;
            if (seg_end > load_end_val) {
                load_end_val = seg_end;
            }
        }
    }

    if (load_start == 0xFFFFFFFFFFFFFFFFULL) {
        return -10;
    }

    total_size = load_end_val - load_start;
    total_pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;

    handle->pages = (uint64_t *)kmalloc(total_pages * sizeof(uint64_t));
    if (!handle->pages) {
        return -11;
    }
    memset(handle->pages, 0, total_pages * sizeof(uint64_t));
    handle->page_count = 0;

    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD || phdr[i].p_memsz == 0) {
            continue;
        }

        vaddr = base_addr + phdr[i].p_vaddr;
        memsz = phdr[i].p_memsz;
        filesz = phdr[i].p_filesz;
        offset = phdr[i].p_offset;

        flags = 0x5;
        if (phdr[i].p_flags & PF_W) {
            flags |= 0x2;
        }
        if (!(phdr[i].p_flags & PF_X) || (phdr[i].p_flags & PF_W)) {
            flags |= VMM_PTE_NX;
        }

        if (offset + filesz > size) {
            for (j = 0; j < handle->page_count; j++) {
                pfa_free(handle->pages[j]);
            }
            kfree(handle->pages);
            handle->pages = NULL;
            return -12;
        }

        vaddr_start = vaddr & ~0xFFFu;
        vaddr_end = (vaddr + memsz + 0xFFF) & ~0xFFFu;
        segment_pages = (vaddr_end - vaddr_start) / PAGE_SIZE;

        for (page = 0; page < segment_pages; page++) {
            page_vaddr = vaddr_start + page * PAGE_SIZE;
            phys = pfa_alloc();
            if (!phys) {
                for (j = 0; j < handle->page_count; j++) {
                    pfa_free(handle->pages[j]);
                }
                kfree(handle->pages);
                handle->pages = NULL;
                return -13;
            }

            pmm_zero_page_phys(phys);
            vmm_map_page_in_pml4(pd_phys, page_vaddr, phys, flags);
            handle->pages[handle->page_count++] = phys;
        }

        if (filesz > 0) {
            vmm_copy_to_pml4(pd_phys, vaddr, data + offset, filesz);
        }
    }

    handle->load_base = base_addr;
    handle->load_size = total_size;

    handle->symtab = NULL;
    handle->symtab_count = 0;
    handle->strtab = NULL;
    handle->strtab_size = 0;
    handle->symtab2 = NULL;
    handle->symtab2_count = 0;
    handle->strtab2 = NULL;
    handle->strtab2_size = 0;
    handle->init_array_vaddr = 0;
    handle->init_array_size = 0;
    handle->fini_array_vaddr = 0;
    handle->fini_array_size = 0;
    handle->init_func = 0;
    handle->fini_func = 0;
    handle->needed_count = 0;

    if (ehdr->e_shoff && ehdr->e_shnum) {
        shdr = (const Elf64_Shdr *)(data + ehdr->e_shoff);
        
        for (i = 0; i < ehdr->e_shnum; i++) {
            if (shdr[i].sh_type == SHT_DYNSYM) {
                if (shdr[i].sh_offset + shdr[i].sh_size <= size) {
                    sym_count = shdr[i].sh_size / sizeof(Elf64_Sym);
                    handle->symtab = (Elf64_Sym *)kmalloc(shdr[i].sh_size);
                    if (handle->symtab) {
                        memcpy(handle->symtab, data + shdr[i].sh_offset, shdr[i].sh_size);
                        handle->symtab_count = sym_count;
                        
                        if (shdr[i].sh_link < ehdr->e_shnum) {
                            strtab_sh = &shdr[shdr[i].sh_link];
                            if (strtab_sh->sh_offset + strtab_sh->sh_size <= size) {
                                handle->strtab = (char *)kmalloc(strtab_sh->sh_size);
                                if (handle->strtab) {
                                    memcpy(handle->strtab, data + strtab_sh->sh_offset, strtab_sh->sh_size);
                                    handle->strtab_size = strtab_sh->sh_size;
                                }
                            }
                        }
                    }
                }
            } else if (shdr[i].sh_type == SHT_SYMTAB) {
                if (shdr[i].sh_offset + shdr[i].sh_size <= size) {
                    sym_count = shdr[i].sh_size / sizeof(Elf64_Sym);
                    handle->symtab2 = (Elf64_Sym *)kmalloc(shdr[i].sh_size);
                    if (handle->symtab2) {
                        memcpy(handle->symtab2, data + shdr[i].sh_offset, shdr[i].sh_size);
                        handle->symtab2_count = sym_count;
                        
                        if (shdr[i].sh_link < ehdr->e_shnum) {
                            strtab_sh = &shdr[shdr[i].sh_link];
                            if (strtab_sh->sh_offset + strtab_sh->sh_size <= size) {
                                handle->strtab2 = (char *)kmalloc(strtab_sh->sh_size);
                                if (handle->strtab2) {
                                    memcpy(handle->strtab2, data + strtab_sh->sh_offset, strtab_sh->sh_size);
                                    handle->strtab2_size = strtab_sh->sh_size;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = (const Elf64_Dyn *)(data + phdr[i].p_offset);
            rel_offset = 0;
            rel_size = 0;
            dyn_strtab_offset = 0;
            dyn_strtab_size = 0;
            needed_count = 0;
            
            while (dyn->d_tag != DT_NULL) {
                if (dyn->d_tag == DT_RELA) {
                    rel_offset = dyn->d_un.d_ptr;
                } else if (dyn->d_tag == DT_RELASZ) {
                    rel_size = dyn->d_un.d_val;
                } else if (dyn->d_tag == DT_INIT) {
                    handle->init_func = base_addr + dyn->d_un.d_ptr;
                } else if (dyn->d_tag == DT_FINI) {
                    handle->fini_func = base_addr + dyn->d_un.d_ptr;
                } else if (dyn->d_tag == DT_INIT_ARRAY) {
                    handle->init_array_vaddr = base_addr + dyn->d_un.d_ptr;
                } else if (dyn->d_tag == DT_INIT_ARRAYSZ) {
                    handle->init_array_size = dyn->d_un.d_val;
                } else if (dyn->d_tag == DT_FINI_ARRAY) {
                    handle->fini_array_vaddr = base_addr + dyn->d_un.d_ptr;
                } else if (dyn->d_tag == DT_FINI_ARRAYSZ) {
                    handle->fini_array_size = dyn->d_un.d_val;
                } else if (dyn->d_tag == DT_STRTAB) {
                    dyn_strtab_offset = dyn->d_un.d_ptr;
                } else if (dyn->d_tag == DT_STRSZ) {
                    dyn_strtab_size = dyn->d_un.d_val;
                } else if (dyn->d_tag == DT_NEEDED) {
                    if (needed_count < 16) {
                        needed_offsets[needed_count++] = dyn->d_un.d_val;
                    }
                }
                dyn++;
            }
            
            if (rel_size > 0 && rel_offset < size) {
                rel = (const Elf64_Rela *)(data + rel_offset);
                rel_count = rel_size / sizeof(Elf64_Rela);
                
                for (r = 0; r < rel_count; r++) {
                    type = ELF64_R_TYPE(rel[r].r_info);
                    if (type == R_X86_64_RELATIVE) {
                        addr = base_addr + rel[r].r_offset;
                        value = base_addr + rel[r].r_addend;
                        vmm_copy_to_pml4(pd_phys, addr, &value, sizeof(uint64_t));
                    }
                }
            }

            handle->needed_count = 0;
            if (dyn_strtab_offset > 0 && dyn_strtab_offset < size && needed_count > 0) {
                for (r = 0; r < (uint64_t)needed_count; r++) {
                    if (needed_offsets[r] < dyn_strtab_size &&
                        dyn_strtab_offset + needed_offsets[r] < size) {
                        strncpy(handle->needed[handle->needed_count],
                                (const char *)(data + dyn_strtab_offset + needed_offsets[r]),
                                63);
                        handle->needed[handle->needed_count][63] = '\0';
                        handle->needed_count++;
                    }
                }
            }

            break;
        }
    }

    handle->file_data = (uint8_t *)kmalloc(size);
    if (handle->file_data) {
        memcpy(handle->file_data, data, size);
        handle->file_size = size;
    } else {
        handle->file_size = 0;
    }

    return 0;
}

int elf_relocate_so(uint64_t pd_phys, dl_handle_t *handle, dl_handle_t *all_handles, int num_handles) {
    const Elf64_Ehdr *ehdr;
    const Elf64_Phdr *phdr;
    const Elf64_Dyn *dyn;
    const Elf64_Rela *rel;
    const Elf64_Sym *sym;
    uint64_t rela_offset;
    uint64_t rela_size;
    uint64_t jmprel_offset;
    uint64_t jmprel_size;
    uint64_t rel_count;
    uint64_t r;
    uint64_t type;
    uint64_t sym_idx;
    uint64_t addr;
    uint64_t value;
    const char *sym_name;
    uint64_t resolved;
    int h;
    uint16_t i;
    const uint8_t *data;
    uint64_t size;
    uint64_t base;

    if (!handle || !handle->file_data || handle->file_size == 0) return -1;

    data = handle->file_data;
    size = handle->file_size;
    base = handle->load_base;

    ehdr = (const Elf64_Ehdr *)data;
    phdr = (const Elf64_Phdr *)(data + ehdr->e_phoff);

    rela_offset = 0;
    rela_size = 0;
    jmprel_offset = 0;
    jmprel_size = 0;

    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = (const Elf64_Dyn *)(data + phdr[i].p_offset);
            while (dyn->d_tag != DT_NULL) {
                if (dyn->d_tag == DT_RELA)
                    rela_offset = dyn->d_un.d_ptr;
                else if (dyn->d_tag == DT_RELASZ)
                    rela_size = dyn->d_un.d_val;
                else if (dyn->d_tag == DT_JMPREL)
                    jmprel_offset = dyn->d_un.d_ptr;
                else if (dyn->d_tag == DT_PLTRELSZ)
                    jmprel_size = dyn->d_un.d_val;
                dyn++;
            }
            break;
        }
    }

    if (rela_size > 0 && rela_offset < size) {
        rel = (const Elf64_Rela *)(data + rela_offset);
        rel_count = rela_size / sizeof(Elf64_Rela);
        for (r = 0; r < rel_count; r++) {
            type = ELF64_R_TYPE(rel[r].r_info);
            sym_idx = ELF64_R_SYM(rel[r].r_info);
            addr = base + rel[r].r_offset;
            if (type == R_X86_64_RELATIVE) {
                value = base + rel[r].r_addend;
                vmm_copy_to_pml4(pd_phys, addr, &value, sizeof(uint64_t));
            } else if (type == R_X86_64_GLOB_DAT || type == R_X86_64_64 || type == R_X86_64_JUMP_SLOT) {
                if (handle->symtab && sym_idx < handle->symtab_count && handle->strtab) {
                    sym = &handle->symtab[sym_idx];
                    sym_name = handle->strtab + sym->st_name;
                    resolved = 0;
                    for (h = 0; h < num_handles && resolved == 0; h++) {
                        if (all_handles[h].in_use && &all_handles[h] != handle)
                            resolved = elf_so_find_symbol(&all_handles[h], sym_name);
                    }
                    if (resolved == 0 && sym->st_value != 0)
                        resolved = base + sym->st_value;
                    if (resolved != 0) {
                        if (type == R_X86_64_64)
                            resolved += (uint64_t)rel[r].r_addend;
                        vmm_copy_to_pml4(pd_phys, addr, &resolved, sizeof(uint64_t));
                    }
                }
            }
        }
    }

    if (jmprel_size > 0 && jmprel_offset < size) {
        rel = (const Elf64_Rela *)(data + jmprel_offset);
        rel_count = jmprel_size / sizeof(Elf64_Rela);
        for (r = 0; r < rel_count; r++) {
            type = ELF64_R_TYPE(rel[r].r_info);
            sym_idx = ELF64_R_SYM(rel[r].r_info);
            addr = base + rel[r].r_offset;
            if (type == R_X86_64_JUMP_SLOT) {
                if (handle->symtab && sym_idx < handle->symtab_count && handle->strtab) {
                    sym = &handle->symtab[sym_idx];
                    sym_name = handle->strtab + sym->st_name;
                    resolved = 0;
                    for (h = 0; h < num_handles && resolved == 0; h++) {
                        if (all_handles[h].in_use && &all_handles[h] != handle)
                            resolved = elf_so_find_symbol(&all_handles[h], sym_name);
                    }
                    if (resolved == 0 && sym->st_value != 0)
                        resolved = base + sym->st_value;
                    if (resolved != 0)
                        vmm_copy_to_pml4(pd_phys, addr, &resolved, sizeof(uint64_t));
                }
            }
        }
    }

    return 0;
}

uint64_t elf_so_find_symbol(dl_handle_t *handle, const char *name) {
    uint64_t i;
    Elf64_Sym *sym;
    uint8_t bind;
    uint8_t type;
    const char *sym_name;

    if (!handle || !name) {
        return 0;
    }

    if (handle->symtab && handle->strtab) {
        for (i = 0; i < handle->symtab_count; i++) {
            sym = &handle->symtab[i];
            
            if (sym->st_name >= handle->strtab_size) {
                continue;
            }
            
            bind = ELF64_ST_BIND(sym->st_info);
            type = ELF64_ST_TYPE(sym->st_info);
            
            if ((bind == STB_GLOBAL || bind == STB_WEAK) && 
                (type == STT_FUNC || type == STT_OBJECT || type == STT_NOTYPE) &&
                sym->st_value != 0) {
                
                sym_name = handle->strtab + sym->st_name;
                if (strcmp_local(sym_name, name) == 0) {
                    return handle->load_base + sym->st_value;
                }
            }
        }
    }

    if (handle->symtab2 && handle->strtab2) {
        for (i = 0; i < handle->symtab2_count; i++) {
            sym = &handle->symtab2[i];
            
            if (sym->st_name >= handle->strtab2_size) {
                continue;
            }
            
            bind = ELF64_ST_BIND(sym->st_info);
            type = ELF64_ST_TYPE(sym->st_info);
            
            if ((bind == STB_GLOBAL || bind == STB_WEAK) && 
                (type == STT_FUNC || type == STT_OBJECT || type == STT_NOTYPE) &&
                sym->st_value != 0) {
                
                sym_name = handle->strtab2 + sym->st_name;
                if (strcmp_local(sym_name, name) == 0) {
                    return handle->load_base + sym->st_value;
                }
            }
        }
    }

    return 0;
}
