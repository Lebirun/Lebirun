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
