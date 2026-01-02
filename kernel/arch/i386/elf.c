#include <kernel/elf.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <kernel/debug.h>
#include <string.h>
#include <stdio.h>

int elf_validate(const uint8_t *data, uint32_t size) {
    if (!data || size < sizeof(Elf32_Ehdr)) {
        return -1;
    }

    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;

    if (ehdr->e_ident[0] != ELFMAG0 ||
        ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 ||
        ehdr->e_ident[3] != ELFMAG3) {
        return -2;
    }

    if (ehdr->e_ident[4] != ELFCLASS32) {
        return -3;
    }

    if (ehdr->e_ident[5] != ELFDATA2LSB) {
        return -4;
    }

    if (ehdr->e_type != ET_EXEC) {
        return -5;
    }

    if (ehdr->e_machine != EM_386) {
        return -6;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return -7;
    }

    if (ehdr->e_phoff + ehdr->e_phnum * sizeof(Elf32_Phdr) > size) {
        return -8;
    }

    if (ehdr->e_entry >= 0xC0000000 || ehdr->e_entry < 0x1000) {
        return -9;
    }

    return 0;
}

int elf_validate_so(const uint8_t *data, uint32_t size) {
    if (!data || size < sizeof(Elf32_Ehdr)) {
        return -1;
    }

    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;

    if (ehdr->e_ident[0] != ELFMAG0 ||
        ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 ||
        ehdr->e_ident[3] != ELFMAG3) {
        return -2;
    }

    if (ehdr->e_ident[4] != ELFCLASS32) {
        return -3;
    }

    if (ehdr->e_ident[5] != ELFDATA2LSB) {
        return -4;
    }

    if (ehdr->e_type != ET_DYN && ehdr->e_type != ET_EXEC) {
        return -5;
    }

    if (ehdr->e_machine != EM_386) {
        return -6;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return -7;
    }

    if (ehdr->e_phoff + ehdr->e_phnum * sizeof(Elf32_Phdr) > size) {
        return -8;
    }

    return 0;
}

uint32_t elf_get_entry(const uint8_t *data) {
    if (!data) return 0;
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;
    return ehdr->e_entry;
}

static uint32_t count_loadable_pages(const uint8_t *data) {
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;
    const Elf32_Phdr *phdr = (const Elf32_Phdr *)(data + ehdr->e_phoff);
    uint32_t total_pages = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && phdr[i].p_memsz > 0) {
            uint32_t vaddr_start = phdr[i].p_vaddr & ~0xFFFu;
            uint32_t vaddr_end = (phdr[i].p_vaddr + phdr[i].p_memsz + 0xFFF) & ~0xFFFu;
            total_pages += (vaddr_end - vaddr_start) / PAGE_SIZE;
        }
    }

    return total_pages;
}

int elf_load_to_pd(uint32_t pd_phys, const uint8_t *data, uint32_t size, elf_info_t *info, uint32_t **out_pages, uint32_t *out_page_count) {
    if (!pd_phys || !data || !info) {
        return -1;
    }

    int valid = elf_validate(data, size);
    if (valid != 0) {
        DPRINTF1("elf_load: validation failed with code %d\n", valid);
        return valid;
    }

    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;
    const Elf32_Phdr *phdr = (const Elf32_Phdr *)(data + ehdr->e_phoff);

    info->entry_point = ehdr->e_entry;
    info->load_base = 0xFFFFFFFF;
    info->load_end = 0;
    info->bss_end = 0;

    uint32_t estimated_pages = count_loadable_pages(data);
    uint32_t *page_list = NULL;
    uint32_t page_index = 0;

    if (out_pages && out_page_count) {
        page_list = (uint32_t *)kmalloc(estimated_pages * sizeof(uint32_t));
        if (!page_list) {
            return -10;
        }
        memset(page_list, 0, estimated_pages * sizeof(uint32_t));
    }

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) {
            continue;
        }

        if (phdr[i].p_memsz == 0) {
            continue;
        }

        uint32_t vaddr = phdr[i].p_vaddr;
        uint32_t memsz = phdr[i].p_memsz;
        uint32_t filesz = phdr[i].p_filesz;
        uint32_t offset = phdr[i].p_offset;

        DPRINTF2("elf_load: segment %d vaddr=0x%08X memsz=0x%X filesz=0x%X offset=0x%X\n",
                 i, vaddr, memsz, filesz, offset);

        if (offset + filesz > size) {
            DPRINTF1("elf_load: segment extends beyond file\n");
            if (page_list) {
                for (uint32_t j = 0; j < page_index; j++) {
                    pfa_free(page_list[j]);
                }
                kfree(page_list);
            }
            return -11;
        }

        uint32_t flags = 0x7;

        uint32_t vaddr_start = vaddr & ~0xFFFu;
        uint32_t vaddr_end = (vaddr + memsz + 0xFFF) & ~0xFFFu;
        uint32_t segment_pages = (vaddr_end - vaddr_start) / PAGE_SIZE;

        for (uint32_t page = 0; page < segment_pages; page++) {
            uint32_t page_vaddr = vaddr_start + page * PAGE_SIZE;
            
            uint32_t existing_phys = vmm_get_phys_in_pd(pd_phys, page_vaddr);
            if (existing_phys != 0) {
                continue;
            }
            
            uint32_t phys = pfa_alloc();
            if (!phys) {
                DPRINTF1("elf_load: out of physical memory\n");
                if (page_list) {
                    for (uint32_t j = 0; j < page_index; j++) {
                        pfa_free(page_list[j]);
                    }
                    kfree(page_list);
                }
                return -12;
            }

            pmm_zero_page_phys(phys);
            vmm_map_page_in_pd(pd_phys, page_vaddr, phys, flags);

            if (page_list && page_index < estimated_pages) {
                page_list[page_index++] = phys;
            }
        }

        if (filesz > 0) {
            vmm_copy_to_pd(pd_phys, vaddr, data + offset, filesz);
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

    DPRINTF2("elf_load: done entry=0x%08X load_base=0x%08X load_end=0x%08X bss_end=0x%08X\n",
             info->entry_point, info->load_base, info->load_end, info->bss_end);

    return 0;
}

static int strcmp_local(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

int elf_load_so(uint32_t pd_phys, const uint8_t *data, uint32_t size, uint32_t base_addr, dl_handle_t *handle) {
    if (!pd_phys || !data || !handle) {
        return -1;
    }

    int valid = elf_validate_so(data, size);
    if (valid != 0) {
        DPRINTF1("elf_load_so: validation failed with code %d\n", valid);
        return valid;
    }

    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;
    const Elf32_Phdr *phdr = (const Elf32_Phdr *)(data + ehdr->e_phoff);

    uint32_t load_start = 0xFFFFFFFF;
    uint32_t load_end_val = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && phdr[i].p_memsz > 0) {
            if (phdr[i].p_vaddr < load_start) {
                load_start = phdr[i].p_vaddr;
            }
            uint32_t seg_end = phdr[i].p_vaddr + phdr[i].p_memsz;
            if (seg_end > load_end_val) {
                load_end_val = seg_end;
            }
        }
    }

    if (load_start == 0xFFFFFFFF) {
        return -10;
    }

    uint32_t total_size = load_end_val - load_start;
    uint32_t total_pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;

    handle->pages = (uint32_t *)kmalloc(total_pages * sizeof(uint32_t));
    if (!handle->pages) {
        return -11;
    }
    memset(handle->pages, 0, total_pages * sizeof(uint32_t));
    handle->page_count = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD || phdr[i].p_memsz == 0) {
            continue;
        }

        uint32_t vaddr = base_addr + phdr[i].p_vaddr;
        uint32_t memsz = phdr[i].p_memsz;
        uint32_t filesz = phdr[i].p_filesz;
        uint32_t offset = phdr[i].p_offset;

        if (offset + filesz > size) {
            for (uint32_t j = 0; j < handle->page_count; j++) {
                pfa_free(handle->pages[j]);
            }
            kfree(handle->pages);
            handle->pages = NULL;
            return -12;
        }

        uint32_t vaddr_start = vaddr & ~0xFFFu;
        uint32_t vaddr_end = (vaddr + memsz + 0xFFF) & ~0xFFFu;
        uint32_t segment_pages = (vaddr_end - vaddr_start) / PAGE_SIZE;

        for (uint32_t page = 0; page < segment_pages; page++) {
            uint32_t page_vaddr = vaddr_start + page * PAGE_SIZE;
            uint32_t phys = pfa_alloc();
            if (!phys) {
                for (uint32_t j = 0; j < handle->page_count; j++) {
                    pfa_free(handle->pages[j]);
                }
                kfree(handle->pages);
                handle->pages = NULL;
                return -13;
            }

            pmm_zero_page_phys(phys);
            vmm_map_page_in_pd(pd_phys, page_vaddr, phys, 0x7);
            handle->pages[handle->page_count++] = phys;
        }

        if (filesz > 0) {
            vmm_copy_to_pd(pd_phys, vaddr, data + offset, filesz);
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

    if (ehdr->e_shoff && ehdr->e_shnum) {
        const Elf32_Shdr *shdr = (const Elf32_Shdr *)(data + ehdr->e_shoff);
        
        for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
            if (shdr[i].sh_type == SHT_DYNSYM) {
                if (shdr[i].sh_offset + shdr[i].sh_size <= size) {
                    uint32_t sym_count = shdr[i].sh_size / sizeof(Elf32_Sym);
                    handle->symtab = (Elf32_Sym *)kmalloc(shdr[i].sh_size);
                    if (handle->symtab) {
                        memcpy(handle->symtab, data + shdr[i].sh_offset, shdr[i].sh_size);
                        handle->symtab_count = sym_count;
                        
                        if (shdr[i].sh_link < ehdr->e_shnum) {
                            const Elf32_Shdr *strtab_sh = &shdr[shdr[i].sh_link];
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
                    uint32_t sym_count = shdr[i].sh_size / sizeof(Elf32_Sym);
                    handle->symtab2 = (Elf32_Sym *)kmalloc(shdr[i].sh_size);
                    if (handle->symtab2) {
                        memcpy(handle->symtab2, data + shdr[i].sh_offset, shdr[i].sh_size);
                        handle->symtab2_count = sym_count;
                        
                        if (shdr[i].sh_link < ehdr->e_shnum) {
                            const Elf32_Shdr *strtab_sh = &shdr[shdr[i].sh_link];
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

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            const Elf32_Dyn *dyn = (const Elf32_Dyn *)(data + phdr[i].p_offset);
            uint32_t rel_offset = 0, rel_size = 0;
            
            while (dyn->d_tag != DT_NULL) {
                if (dyn->d_tag == DT_REL) {
                    rel_offset = dyn->d_un.d_ptr;
                } else if (dyn->d_tag == DT_RELSZ) {
                    rel_size = dyn->d_un.d_val;
                }
                dyn++;
            }
            
            if (rel_size > 0 && rel_offset < size) {
                const Elf32_Rel *rel = (const Elf32_Rel *)(data + rel_offset);
                uint32_t rel_count = rel_size / sizeof(Elf32_Rel);
                
                for (uint32_t r = 0; r < rel_count; r++) {
                    uint32_t type = ELF32_R_TYPE(rel[r].r_info);
                    if (type == R_386_RELATIVE) {
                        uint32_t addr = base_addr + rel[r].r_offset;
                        uint32_t value = base_addr;
                        vmm_copy_to_pd(pd_phys, addr, &value, sizeof(uint32_t));
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

uint32_t elf_so_find_symbol(dl_handle_t *handle, const char *name) {
    if (!handle || !name) {
        return 0;
    }

    if (handle->symtab && handle->strtab) {
        for (uint32_t i = 0; i < handle->symtab_count; i++) {
            Elf32_Sym *sym = &handle->symtab[i];
            
            if (sym->st_name >= handle->strtab_size) {
                continue;
            }
            
            uint8_t bind = ELF32_ST_BIND(sym->st_info);
            uint8_t type = ELF32_ST_TYPE(sym->st_info);
            
            if ((bind == STB_GLOBAL || bind == STB_WEAK) && 
                (type == STT_FUNC || type == STT_OBJECT || type == STT_NOTYPE) &&
                sym->st_value != 0) {
                
                const char *sym_name = handle->strtab + sym->st_name;
                if (strcmp_local(sym_name, name) == 0) {
                    return handle->load_base + sym->st_value;
                }
            }
        }
    }

    if (handle->symtab2 && handle->strtab2) {
        for (uint32_t i = 0; i < handle->symtab2_count; i++) {
            Elf32_Sym *sym = &handle->symtab2[i];
            
            if (sym->st_name >= handle->strtab2_size) {
                continue;
            }
            
            uint8_t bind = ELF32_ST_BIND(sym->st_info);
            uint8_t type = ELF32_ST_TYPE(sym->st_info);
            
            if ((bind == STB_GLOBAL || bind == STB_WEAK) && 
                (type == STT_FUNC || type == STT_OBJECT || type == STT_NOTYPE) &&
                sym->st_value != 0) {
                
                const char *sym_name = handle->strtab2 + sym->st_name;
                if (strcmp_local(sym_name, name) == 0) {
                    return handle->load_base + sym->st_value;
                }
            }
        }
    }

    return 0;
}
