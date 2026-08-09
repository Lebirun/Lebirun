#include <lebirun/partition.h>
#include <lebirun/drivers/sata/ahci.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdio.h>

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

    table->is_gpt = 0;
    count = 0;

    for (i = 0; i < MBR_PARTITION_COUNT; i++) {
        mbr_partition_entry_t *pe;

        pe = &mbr->partitions[i];
        if (pe->type == PART_TYPE_EMPTY)
            continue;
        if (pe->lba_start == 0 || pe->sector_count == 0)
            continue;
        if (pe->type == PART_TYPE_EXTENDED || pe->type == PART_TYPE_EXTENDED_LBA)
            continue;

        if (partition_table_reserve(table, count + 1) != 0) {
            kfree(buf);
            partition_table_free(table);
            return -1;
        }

        table->parts[count].valid = 1;
        table->parts[count].port_index = port_index;
        table->parts[count].part_number = i + 1;
        table->parts[count].start_lba = pe->lba_start;
        table->parts[count].sector_count = pe->sector_count;
        table->parts[count].mbr_type = pe->type;
        table->parts[count].is_gpt = 0;
        memset(table->parts[count].gpt_type_guid, 0, 16);
        count++;
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
