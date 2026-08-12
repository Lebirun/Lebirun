#include <lebirun/partition.h>
#include <lebirun/drivers/sata/ahci.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdio.h>

static int partition_table_reserve(partition_table_t *table, int needed);

typedef struct {
    uint64_t inline_lbas[8];
    uint64_t *lbas;
    size_t count;
    size_t capacity;
} ebr_visit_set_t;

static int ebr_visit_add(ebr_visit_set_t *set, uint64_t lba) {
    uint64_t *grown;
    size_t new_capacity;
    size_t i;

    for (i = 0; i < set->count; i++) {
        if (set->lbas[i] == lba) return 1;
    }
    if (set->count == set->capacity) {
        if (set->capacity > SIZE_MAX / 2) return -1;
        new_capacity = set->capacity * 2;
        if (new_capacity > SIZE_MAX / sizeof(*grown)) return -1;
        grown = (uint64_t *)kmalloc(new_capacity * sizeof(*grown));
        if (!grown) return -1;
        memcpy(grown, set->lbas, set->count * sizeof(*grown));
        if (set->lbas != set->inline_lbas) kfree(set->lbas);
        set->lbas = grown;
        set->capacity = new_capacity;
    }
    set->lbas[set->count++] = lba;
    return 0;
}

static int partition_add_mbr_entry(partition_table_t *table, int *count,
                                   uint64_t port_index, int part_number,
                                   uint64_t start_lba, uint64_t sector_count,
                                   uint8_t type) {
    partition_info_t *part;

    if (partition_table_reserve(table, *count + 1) != 0) return -1;
    part = &table->parts[*count];
    part->valid = 1;
    part->port_index = port_index;
    part->part_number = part_number;
    part->start_lba = start_lba;
    part->sector_count = sector_count;
    part->mbr_type = type;
    part->is_gpt = 0;
    memset(part->gpt_type_guid, 0, 16);
    (*count)++;
    return 0;
}

static int partition_table_reserve(partition_table_t *table, int needed) {
    partition_info_t *parts;
    int old_capacity;
    int capacity;

    if (needed < 0) return -1;
    if (needed <= table->capacity) return 0;
    old_capacity = table->capacity;
    capacity = old_capacity ? old_capacity : MBR_PARTITION_COUNT;
    while (capacity < needed) {
        if (capacity > INT32_MAX / 2) return -1;
        capacity *= 2;
    }
    if ((uint64_t)capacity > UINT64_MAX / sizeof(partition_info_t)) return -1;
    parts = (partition_info_t *)krealloc(table->parts,
                                         (uint64_t)capacity * sizeof(partition_info_t));
    if (!parts) return -1;
    memset(parts + old_capacity, 0,
           (uint64_t)(capacity - old_capacity) * sizeof(partition_info_t));
    table->parts = parts;
    table->capacity = capacity;
    return 0;
}

void partition_table_free(partition_table_t *table) {
    if (!table) return;
    if (table->parts) kfree(table->parts);
    table->parts = NULL;
    table->count = 0;
    table->capacity = 0;
    table->is_gpt = 0;
}

int partition_is_guid_zero(const uint8_t *guid) {
    int i;
    for (i = 0; i < 16; i++) {
        if (guid[i] != 0)
            return 0;
    }
    return 1;
}

int partition_is_guid_equal(const uint8_t *a, const uint8_t *b) {
    int i;
    for (i = 0; i < 16; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

const char *partition_type_name(uint8_t mbr_type) {
    switch (mbr_type) {
    case PART_TYPE_EMPTY:       return "Empty";
    case PART_TYPE_FAT12:       return "FAT12";
    case PART_TYPE_FAT16_SMALL: return "FAT16 (<32MB)";
    case PART_TYPE_EXTENDED:    return "Extended";
    case PART_TYPE_FAT16_LARGE: return "FAT16 (>32MB)";
    case PART_TYPE_NTFS:        return "NTFS/exFAT";
    case PART_TYPE_FAT32:       return "FAT32";
    case PART_TYPE_FAT32_LBA:   return "FAT32 LBA";
    case PART_TYPE_FAT16_LBA:   return "FAT16 LBA";
    case PART_TYPE_EXTENDED_LBA:return "Extended LBA";
    case PART_TYPE_LINUX_SWAP:  return "Linux swap";
    case PART_TYPE_LINUX:       return "Linux";
    case PART_TYPE_LINUX_LVM:   return "Linux LVM";
    case PART_TYPE_GPT_PROTECT: return "GPT Protective";
    default:                    return "Unknown";
    }
}

int partition_scan_mbr(uint64_t port_index, partition_table_t *table) {
    ahci_port_t *port;
    uint8_t *buf;
    mbr_t *mbr;
    mbr_partition_entry_t primary[MBR_PARTITION_COUNT];
    mbr_partition_entry_t *pe;
    mbr_partition_entry_t *logical;
    mbr_partition_entry_t *link;
    ebr_visit_set_t visited;
    uint64_t extended_base;
    uint64_t ebr_lba;
    uint64_t next_lba;
    uint64_t logical_start;
    int visit_result;
    int logical_number;
    int i;
    int count;

    port = ahci_get_port(port_index);
    if (!port)
        return -1;

    buf = (uint8_t *)kmalloc(512);
    if (!buf)
        return -1;

    if (ahci_read_sectors(port, 0, 1, buf) != 0) {
        kfree(buf);
        return -1;
    }

    mbr = (mbr_t *)buf;

    if (mbr->signature != MBR_SIGNATURE) {
        kfree(buf);
        return -1;
    }
    memcpy(primary, mbr->partitions, sizeof(primary));

    table->is_gpt = 0;
    count = 0;
    logical_number = 5;

    for (i = 0; i < MBR_PARTITION_COUNT; i++) {
        pe = &primary[i];
        if (pe->type == PART_TYPE_EMPTY)
            continue;
        if (pe->lba_start == 0 || pe->sector_count == 0)
            continue;
        if (pe->type == PART_TYPE_EXTENDED ||
            pe->type == PART_TYPE_EXTENDED_LBA) {
            extended_base = pe->lba_start;
            ebr_lba = extended_base;
            memset(&visited, 0, sizeof(visited));
            visited.lbas = visited.inline_lbas;
            visited.capacity = sizeof(visited.inline_lbas) /
                               sizeof(visited.inline_lbas[0]);
            while (ebr_lba) {
                if (ebr_lba >= port->sector_count) break;
                visit_result = ebr_visit_add(&visited, ebr_lba);
                if (visit_result != 0) {
                    if (visit_result < 0) {
                        if (visited.lbas != visited.inline_lbas)
                            kfree(visited.lbas);
                        kfree(buf);
                        partition_table_free(table);
                        return -1;
                    }
                    break;
                }
                if (ahci_read_sectors(port, ebr_lba, 1, buf) != 0) break;
                mbr = (mbr_t *)buf;
                if (mbr->signature != MBR_SIGNATURE) break;
                logical = &mbr->partitions[0];
                if (logical->type != PART_TYPE_EMPTY &&
                    logical->type != PART_TYPE_EXTENDED &&
                    logical->type != PART_TYPE_EXTENDED_LBA &&
                    logical->lba_start && logical->sector_count) {
                    if ((uint64_t)logical->lba_start <=
                        UINT64_MAX - ebr_lba) {
                        logical_start = ebr_lba + logical->lba_start;
                        if (logical_start < port->sector_count &&
                            (uint64_t)logical->sector_count <=
                            port->sector_count - logical_start) {
                            if (partition_add_mbr_entry(
                                    table, &count, port_index,
                                    logical_number, logical_start,
                                    logical->sector_count,
                                    logical->type) != 0) {
                                if (visited.lbas != visited.inline_lbas)
                                    kfree(visited.lbas);
                                kfree(buf);
                                partition_table_free(table);
                                return -1;
                            }
                            logical_number++;
                        }
                    }
                }
                link = &mbr->partitions[1];
                next_lba = 0;
                if ((link->type == PART_TYPE_EXTENDED ||
                     link->type == PART_TYPE_EXTENDED_LBA) &&
                    link->lba_start &&
                    (uint64_t)link->lba_start <=
                    UINT64_MAX - extended_base) {
                    next_lba = extended_base + link->lba_start;
                }
                ebr_lba = next_lba;
            }
            if (visited.lbas != visited.inline_lbas) kfree(visited.lbas);
            continue;
        }

        if ((uint64_t)pe->lba_start >= port->sector_count ||
            (uint64_t)pe->sector_count >
            port->sector_count - (uint64_t)pe->lba_start)
            continue;
        if (partition_add_mbr_entry(table, &count, port_index, i + 1,
                                    pe->lba_start, pe->sector_count,
                                    pe->type) != 0) {
            kfree(buf);
            partition_table_free(table);
            return -1;
        }
    }

    table->count = count;
    kfree(buf);
    return 0;
}

int partition_scan_gpt(uint64_t port_index, partition_table_t *table) {
    ahci_port_t *port;
    uint8_t *buf;
    gpt_header_t *hdr;
    uint64_t entry_lba;
    uint64_t num_entries;
    uint64_t entry_size;
    uint64_t sectors_needed;
    uint64_t entries_size;
    uint64_t entry_offset;
    uint64_t entry_sector;
    uint64_t entry_sector_offset;
    uint64_t entry_sectors;
    uint8_t entry_buf[1024];
    uint64_t i;
    int count;

    port = ahci_get_port(port_index);
    if (!port)
        return -1;

    buf = (uint8_t *)kmalloc(512);
    if (!buf)
        return -1;

    if (ahci_read_sectors(port, GPT_HEADER_LBA, 1, buf) != 0) {
        kfree(buf);
        return -1;
    }

    hdr = (gpt_header_t *)buf;

    if (hdr->signature != GPT_SIGNATURE) {
        kfree(buf);
        return -1;
    }

    if (hdr->revision < GPT_REVISION_1_0) {
        kfree(buf);
        return -1;
    }

    entry_lba = (uint64_t)hdr->partition_entry_lba;
    num_entries = hdr->num_partition_entries;
    entry_size = hdr->partition_entry_size;

    kfree(buf);

    if (entry_size < sizeof(gpt_partition_entry_t))
        return -1;
    if (num_entries == 0)
        return -1;
    if (entry_size > UINT64_MAX / num_entries)
        return -1;
    entries_size = num_entries * entry_size;
    if (entries_size > UINT64_MAX - 511)
        return -1;
    sectors_needed = (entries_size + 511) / 512;
    if (sectors_needed > UINT64_MAX / 512)
        return -1;
    if (entry_lba > port->sector_count ||
        sectors_needed > port->sector_count - entry_lba)
        return -1;
    table->is_gpt = 1;
    count = 0;

    for (i = 0; i < num_entries; i++) {
        gpt_partition_entry_t *gpe;

        entry_offset = i * entry_size;
        entry_sector = entry_offset / 512;
        entry_sector_offset = entry_offset % 512;
        entry_sectors = (entry_sector_offset +
                         sizeof(gpt_partition_entry_t) + 511) / 512;
        if (ahci_read_sectors(port, entry_lba + entry_sector,
                              entry_sectors, entry_buf) != 0) {
            partition_table_free(table);
            return -1;
        }
        gpe = (gpt_partition_entry_t *)(entry_buf + entry_sector_offset);

        if (partition_is_guid_zero(gpe->type_guid))
            continue;
        if (gpe->starting_lba == 0 || gpe->ending_lba == 0)
            continue;
        if (gpe->ending_lba < gpe->starting_lba)
            continue;
        if (i >= INT32_MAX) {
            partition_table_free(table);
            return -1;
        }

        if (partition_table_reserve(table, count + 1) != 0) {
            partition_table_free(table);
            return -1;
        }

        table->parts[count].valid = 1;
        table->parts[count].port_index = port_index;
        table->parts[count].part_number = (int)i + 1;
        table->parts[count].start_lba = gpe->starting_lba;
        table->parts[count].sector_count = gpe->ending_lba - gpe->starting_lba + 1;
        table->parts[count].mbr_type = 0;
        table->parts[count].is_gpt = 1;
        memcpy(table->parts[count].gpt_type_guid, gpe->type_guid, 16);
        count++;
    }

    table->count = count;
    return 0;
}

int partition_scan(uint64_t port_index, partition_table_t *table) {
    ahci_port_t *port;
    uint8_t *buf;
    mbr_t *mbr;

    if (!table) return -1;
    table->count = 0;
    table->capacity = 0;
    table->is_gpt = 0;
    table->parts = NULL;

    port = ahci_get_port(port_index);
    if (!port) {
        return -1;
    }

    buf = (uint8_t *)kmalloc(512);
    if (!buf) {
        return -1;
    }

    if (ahci_read_sectors(port, 0, 1, buf) != 0) {
        kfree(buf);
        return -1;
    }

    mbr = (mbr_t *)buf;

    if (mbr->signature != MBR_SIGNATURE) {
        kfree(buf);
        return -1;
    }

    if (mbr->partitions[0].type == PART_TYPE_GPT_PROTECT) {
        kfree(buf);
        return partition_scan_gpt(port_index, table);
    }

    kfree(buf);
    return partition_scan_mbr(port_index, table);
}
