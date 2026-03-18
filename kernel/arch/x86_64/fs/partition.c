#include <kernel/partition.h>
#include <kernel/drivers/sata/ahci.h>
#include <kernel/mem_map.h>
#include <string.h>
#include <stdio.h>

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

        if (count >= PARTITION_MAX)
            break;

        table->parts[count].valid = 1;
        table->parts[count].port_index = port_index;
        table->parts[count].part_number = count + 1;
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
    uint8_t *entry_buf;
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
    if (num_entries > 128)
        num_entries = 128;

    sectors_needed = (num_entries * entry_size + 511) / 512;
    entry_buf = (uint8_t *)kmalloc(sectors_needed * 512);
    if (!entry_buf)
        return -1;

    if (ahci_read_sectors(port, entry_lba, sectors_needed, entry_buf) != 0) {
        kfree(entry_buf);
        return -1;
    }

    table->is_gpt = 1;
    count = 0;

    for (i = 0; i < num_entries; i++) {
        gpt_partition_entry_t *gpe;

        gpe = (gpt_partition_entry_t *)(entry_buf + i * entry_size);

        if (partition_is_guid_zero(gpe->type_guid))
            continue;
        if (gpe->starting_lba == 0 || gpe->ending_lba == 0)
            continue;

        if (count >= PARTITION_MAX)
            break;

        table->parts[count].valid = 1;
        table->parts[count].port_index = port_index;
        table->parts[count].part_number = count + 1;
        table->parts[count].start_lba = gpe->starting_lba;
        table->parts[count].sector_count = gpe->ending_lba - gpe->starting_lba + 1;
        table->parts[count].mbr_type = 0;
        table->parts[count].is_gpt = 1;
        memcpy(table->parts[count].gpt_type_guid, gpe->type_guid, 16);
        count++;
    }

    table->count = count;
    kfree(entry_buf);
    return 0;
}

int partition_scan(uint64_t port_index, partition_table_t *table) {
    ahci_port_t *port;
    uint8_t *buf;
    mbr_t *mbr;

    memset(table, 0, sizeof(partition_table_t));

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

    if (mbr->partitions[0].type == PART_TYPE_GPT_PROTECT) {
        kfree(buf);
        printf("PART: Detected GPT partition table on port %u\n", port_index);
        return partition_scan_gpt(port_index, table);
    }

    kfree(buf);
    printf("PART: Detected MBR partition table on port %u\n", port_index);
    return partition_scan_mbr(port_index, table);
}
