#include "fat32.h"
#include "../drivers/block.h"
#include "../log.h"

static int mounted;
static uint32_t root_cluster;
static uint8_t sectors_per_cluster;
static uint32_t data_start_sector;
static uint32_t fat_start_sector;
static uint8_t fat_count;
static uint32_t fat_sectors;

struct fat32_entry {
    uint32_t cluster;
    uint32_t size;
    uint8_t attributes;
};

static uint16_t read16(const uint8_t *data, uint32_t offset) {
    return (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
}

static uint32_t read32(const uint8_t *data, uint32_t offset) {
    return (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) |
           ((uint32_t)data[offset + 2] << 16) |
           ((uint32_t)data[offset + 3] << 24);
}

static void write32(uint8_t *data, uint32_t offset, uint32_t value) {
    data[offset] = value;
    data[offset + 1] = value >> 8;
    data[offset + 2] = value >> 16;
    data[offset + 3] = value >> 24;
}
static void write16(uint8_t *data, uint32_t offset, uint16_t value) {
    data[offset] = value;
    data[offset + 1] = value >> 8;
}
static char upper(char c) { return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c; }

int fat32_mount(void) {
    uint8_t sector[512];
    mounted = 0;
    if (!block_ready() || !block_read_sector(0, sector))
        return 0;
    uint16_t bytes_per_sector = read16(sector, 11);
    sectors_per_cluster = sector[13];
    uint16_t reserved_sectors = read16(sector, 14);
    fat_count = sector[16];
    fat_sectors = read32(sector, 36);
    root_cluster = read32(sector, 44);
    if (bytes_per_sector != 512 || sectors_per_cluster == 0 ||
        reserved_sectors == 0 || fat_count == 0 || fat_sectors == 0 ||
        root_cluster < 2 || sector[510] != 0x55 || sector[511] != 0xAA) {
        LOG_ERROR("FAT32 mount failed: invalid boot sector");
        return 0;
    }
    mounted = 1;
    data_start_sector = reserved_sectors + (uint32_t)fat_count * fat_sectors;
    fat_start_sector = reserved_sectors;
    LOG_INFO("FAT32 volume mounted");
    return 1;
}

int fat32_mounted(void) { return mounted; }
static int update_fat(uint32_t cluster, uint32_t value) {
    uint32_t offset = cluster * 4;
    uint8_t sector[512];
    for (uint32_t copy = 0; copy < fat_count; copy++) {
        uint32_t target = fat_start_sector + copy * fat_sectors + offset / 512;
        if (!block_read_sector(target, sector))
            return 0;
        write32(sector, offset % 512, value);
        if (!block_write_sector(target, sector))
            return 0;
    }
    return 1;
}

static uint32_t allocate_cluster(void) {
    uint8_t sector[512];
    for (uint32_t cluster = 2; cluster < fat_sectors * 128; cluster++) {
        uint32_t offset = cluster * 4;
        if (!block_read_sector(fat_start_sector + offset / 512, sector))
            return 0;
        if ((read32(sector, offset % 512) & 0x0FFFFFFF) == 0 &&
            update_fat(cluster, 0x0FFFFFFF))
            return cluster;
    }
    return 0;
}

static uint32_t next_cluster(uint32_t cluster) {
    uint8_t sector[512];
    uint32_t offset = cluster * 4;
    if (!block_read_sector(fat_start_sector + offset / 512, sector))
        return 0x0FFFFFFF;
    return read32(sector, offset % 512) & 0x0FFFFFFF;
}

static uint32_t cluster_sector(uint32_t cluster) {
    return data_start_sector + (cluster - 2) * sectors_per_cluster;
}

static int short_name(const char *name, uint8_t result[11]) {
    for (uint32_t i = 0; i < 11; i++)
        result[i] = ' ';
    uint32_t source = 0, target = 0;
    int extension = 0;
    if (!name || !name[0])
        return 0;
    while (name[source]) {
        char c = name[source++];
        if (c == '.') {
            if (extension || target == 0)
                return 0;
            extension = 1;
            target = 8;
            continue;
        }
        if ((extension && target >= 11) || (!extension && target >= 8))
            return 0;
        c = upper(c);
        if (c < '!' || c > '~' || c == '/' || c == '\\')
            return 0;
        result[target++] = (uint8_t)c;
    }
    if (extension && target == 8)
        return 0;
    return 1;
}

static int utf8_to_utf16(const char *input, uint16_t output[260],
                         size_t *units) {
    size_t input_length = 0, bytes = 0, count = 0;
    while (input[input_length])
        input_length++;
    if (input_length == 0 || input[0] == ' ' ||
        input[input_length - 1] == ' ' || input[input_length - 1] == '.')
        return 0;
    while (bytes < input_length) {
        uint32_t codepoint;
        uint8_t first = (uint8_t)input[bytes++];
        if (first < 0x80)
            codepoint = first;
        else if (first >= 0xC2 && first <= 0xDF) {
            if (bytes >= input_length)
                return 0;
            uint8_t b1 = (uint8_t)input[bytes++];
            if ((b1 & 0xC0) != 0x80)
                return 0;
            codepoint = ((uint32_t)(first & 0x1F) << 6) | (b1 & 0x3F);
        } else if (first >= 0xE0 && first <= 0xEF) {
            if (bytes + 1 >= input_length)
                return 0;
            uint8_t b1 = (uint8_t)input[bytes++], b2 = (uint8_t)input[bytes++];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 ||
                (first == 0xE0 && b1 < 0xA0) || (first == 0xED && b1 >= 0xA0))
                return 0;
            codepoint = ((uint32_t)(first & 0x0F) << 12) |
                        ((uint32_t)(b1 & 0x3F) << 6) | (b2 & 0x3F);
        } else if (first >= 0xF0 && first <= 0xF4) {
            if (bytes + 2 >= input_length)
                return 0;
            uint8_t b1 = (uint8_t)input[bytes++], b2 = (uint8_t)input[bytes++],
                    b3 = (uint8_t)input[bytes++];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 ||
                (b3 & 0xC0) != 0x80 || (first == 0xF0 && b1 < 0x90) ||
                (first == 0xF4 && b1 >= 0x90))
                return 0;
            codepoint = ((uint32_t)(first & 7) << 18) |
                        ((uint32_t)(b1 & 0x3F) << 12) |
                        ((uint32_t)(b2 & 0x3F) << 6) | (b3 & 0x3F);
        } else
            return 0;
        if (codepoint < 0x20 || codepoint == 0x7F || codepoint == '/' ||
            codepoint == '\\' || codepoint == ':' || codepoint == '*' ||
            codepoint == '?' || codepoint == '"' || codepoint == '<' ||
            codepoint == '>' || codepoint == '|')
            return 0;
        if (codepoint <= 0xFFFF) {
            if (count == 255)
                return 0;
            output[count++] = (uint16_t)codepoint;
        } else {
            if (count > 253)
                return 0;
            codepoint -= 0x10000;
            output[count++] = (uint16_t)(0xD800 | (codepoint >> 10));
            output[count++] = (uint16_t)(0xDC00 | (codepoint & 0x3FF));
        }
    }
    if (count == 0 || count > 255)
        return 0;
    *units = count;
    return 1;
}

static int utf16_to_utf8(const uint16_t *input, size_t units, char *output,
                         size_t capacity) {
    size_t out = 0;
    for (size_t i = 0; i < units && input[i] != 0 && input[i] != 0xFFFF; i++) {
        uint32_t codepoint = input[i];
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            if (i + 1 >= units || input[i + 1] < 0xDC00 ||
                input[i + 1] > 0xDFFF)
                return 0;
            codepoint =
                0x10000 + ((codepoint - 0xD800) << 10) + (input[++i] - 0xDC00);
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF)
            return 0;
        size_t bytes = codepoint < 0x80      ? 1
                       : codepoint < 0x800   ? 2
                       : codepoint < 0x10000 ? 3
                                             : 4;
        if (out + bytes >= capacity)
            return 0;
        if (bytes == 1)
            output[out++] = codepoint;
        else if (bytes == 2) {
            output[out++] = 0xC0 | (codepoint >> 6);
            output[out++] = 0x80 | (codepoint & 0x3F);
        } else if (bytes == 3) {
            output[out++] = 0xE0 | (codepoint >> 12);
            output[out++] = 0x80 | ((codepoint >> 6) & 0x3F);
            output[out++] = 0x80 | (codepoint & 0x3F);
        } else {
            output[out++] = 0xF0 | (codepoint >> 18);
            output[out++] = 0x80 | ((codepoint >> 12) & 0x3F);
            output[out++] = 0x80 | ((codepoint >> 6) & 0x3F);
            output[out++] = 0x80 | (codepoint & 0x3F);
        }
    }
    output[out] = '\0';
    return 1;
}

static int path_name_equal(const char *left, const char *right) {
    size_t i = 0;
    while (left[i] && right[i]) {
        char a = left[i], b = right[i];
        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z')
            b += 'a' - 'A';
        if (a != b)
            return 0;
        i++;
    }
    return left[i] == right[i];
}

static uint8_t lfn_checksum(const uint8_t short_entry[11]) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_entry[i]);
    return sum;
}

static int canonical_short_name(const char *name,
                                const uint8_t short_entry[11]) {
    char display[13];
    uint32_t out = 0;
    for (uint32_t i = 0; i < 8 && short_entry[i] != ' '; i++)
        display[out++] = short_entry[i];
    if (short_entry[8] != ' ') {
        display[out++] = '.';
        for (uint32_t i = 8; i < 11 && short_entry[i] != ' '; i++)
            display[out++] = short_entry[i];
    }
    display[out] = '\0';
    uint32_t i = 0;
    while (name[i] && display[i] && name[i] == display[i])
        i++;
    return !name[i] && !display[i];
}

static int short_alias_exists(uint32_t directory, const uint8_t alias[11]) {
    uint8_t sector[512];
    uint32_t cluster = directory;
    for (uint32_t chain = 0;
         chain < 65536 && cluster >= 2 && cluster < 0x0FFFFFF8; chain++) {
        uint32_t first = cluster_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!block_read_sector(first + s, sector))
                return 1;
            for (uint32_t offset = 0; offset < 512; offset += 32) {
                if (sector[offset] == 0)
                    return 0;
                if (sector[offset] == 0xE5 || sector[offset + 11] == 0x0F)
                    continue;
                uint32_t i = 0;
                while (i < 11 && sector[offset + i] == alias[i])
                    i++;
                if (i == 11)
                    return 1;
            }
        }
        cluster = next_cluster(cluster);
    }
    return 0;
}

static int make_short_alias(uint32_t directory, const char *name,
                            uint8_t alias[11], int *needs_long_name) {
    if (short_name(name, alias)) {
        *needs_long_name = !canonical_short_name(name, alias);
        return 1;
    }
    *needs_long_name = 1;
    for (uint32_t i = 0; i < 11; i++)
        alias[i] = ' ';
    size_t length = 0, dot = SIZE_MAX;
    while (name[length]) {
        if (name[length] == '.')
            dot = length;
        length++;
    }
    size_t base_end = dot == SIZE_MAX ? length : dot;
    char base[8];
    size_t base_length = 0;
    for (size_t i = 0; i < base_end && base_length < sizeof(base); i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            base[base_length++] = c;
    }
    if (!base_length)
        base[base_length++] = '_';
    for (uint32_t number = 1; number < 1000000; number++) {
        char digits[7];
        size_t digit_count = 0;
        uint32_t value = number;
        do {
            digits[digit_count++] = (char)('0' + value % 10);
            value /= 10;
        } while (value);
        size_t prefix = 7 - digit_count;
        if (prefix > base_length)
            prefix = base_length;
        for (uint32_t i = 0; i < 11; i++)
            alias[i] = ' ';
        for (size_t i = 0; i < prefix; i++)
            alias[i] = (uint8_t)base[i];
        alias[prefix] = '~';
        for (size_t i = 0; i < digit_count; i++)
            alias[prefix + 1 + i] = (uint8_t)digits[digit_count - i - 1];
        if (dot != SIZE_MAX) {
            size_t ext_length = 0;
            for (size_t i = dot + 1; name[i] && ext_length < 3; i++) {
                char c = name[i];
                if (c >= 'a' && c <= 'z')
                    c -= 'a' - 'A';
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                    alias[8 + ext_length++] = (uint8_t)c;
            }
        }
        if (!short_alias_exists(directory, alias))
            return 1;
    }
    return 0;
}

static int find_free_slots(uint32_t directory, size_t required,
                           uint32_t slot_sectors[21],
                           uint32_t slot_offsets[21]) {
    size_t run = 0;
    int end_seen = 0;
    uint32_t cluster = directory, last = directory;
    uint8_t sector[512];
    for (uint32_t chain = 0;
         chain < 65536 && cluster >= 2 && cluster < 0x0FFFFFF8; chain++) {
        uint32_t first = cluster_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!block_read_sector(first + s, sector))
                return 0;
            for (uint32_t offset = 0; offset < 512; offset += 32) {
                if (sector[offset] == 0)
                    end_seen = 1;
                if (end_seen || sector[offset] == 0xE5) {
                    if (run < required) {
                        slot_sectors[run] = first + s;
                        slot_offsets[run++] = offset;
                    }
                    if (run == required)
                        return 1;
                } else
                    run = 0;
            }
        }
        last = cluster;
        cluster = next_cluster(cluster);
    }
    while (run < required) {
        uint32_t added = allocate_cluster();
        if (!added || !update_fat(last, added))
            return 0;
        for (uint32_t i = 0; i < sectors_per_cluster; i++) {
            for (uint32_t j = 0; j < 512; j++)
                sector[j] = 0;
            if (!block_write_sector(cluster_sector(added) + i, sector))
                return 0;
            for (uint32_t offset = 0; offset < 512; offset += 32) {
                if (run == required)
                    return 1;
                slot_sectors[run] = cluster_sector(added) + i;
                slot_offsets[run++] = offset;
            }
        }
        last = added;
    }
    return 1;
}

struct lfn_state {
    uint16_t units[260];
    uint8_t checksum;
    uint8_t expected_ordinal;
    uint8_t total_ordinals;
    int active;
};

static void lfn_reset(struct lfn_state *state) {
    state->active = 0;
    state->expected_ordinal = 0;
    state->total_ordinals = 0;
}

static void lfn_add_entry(struct lfn_state *state, const uint8_t *entry) {
    uint8_t sequence = entry[0], ordinal = sequence & 0x1F;
    if (sequence & 0xA0) {
        lfn_reset(state);
        return;
    }
    if (sequence & 0x40) {
        lfn_reset(state);
        state->active = ordinal > 0 && ordinal <= 20;
        state->total_ordinals = ordinal;
        state->expected_ordinal = ordinal;
        state->checksum = entry[13];
        for (uint32_t i = 0; i < 260; i++)
            state->units[i] = 0xFFFF;
    }
    if (!state->active || ordinal != state->expected_ordinal ||
        entry[13] != state->checksum || entry[12] != 0 ||
        read16(entry, 26) != 0) {
        lfn_reset(state);
        return;
    }
    static const uint8_t positions[13] = {1,  3,  5,  7,  9,  14, 16,
                                          18, 20, 22, 24, 28, 30};
    size_t base = (size_t)(ordinal - 1) * 13;
    for (uint32_t i = 0; i < 13; i++)
        state->units[base + i] = read16(entry, positions[i]);
    state->expected_ordinal--;
}

static int lfn_decode(const struct lfn_state *state, const uint8_t *short_entry,
                      char *name, size_t capacity) {
    if (!state->active || state->expected_ordinal != 0 ||
        lfn_checksum(short_entry) != state->checksum)
        return 0;
    size_t units = (size_t)state->total_ordinals * 13;
    for (size_t i = 0; i < units; i++)
        if (state->units[i] == 0) {
            units = i;
            break;
        }
    return utf16_to_utf8(state->units, units, name, capacity);
}

static int find_entry(uint32_t directory, const char *name,
                      struct fat32_entry *result, uint32_t *entry_sector,
                      uint32_t *entry_offset) {
    uint8_t wanted[11], sector[512];
    int has_short_name = short_name(name, wanted);
    struct lfn_state lfn;
    lfn_reset(&lfn);
    uint32_t cluster = directory;
    for (uint32_t chain = 0;
         chain < 65536 && cluster >= 2 && cluster < 0x0FFFFFF8; chain++) {
        uint32_t first = cluster_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!block_read_sector(first + s, sector))
                return 0;
            for (uint32_t offset = 0; offset < 512; offset += 32) {
                uint8_t initial = sector[offset];
                if (initial == 0)
                    return 0;
                if (initial == 0xE5) {
                    lfn_reset(&lfn);
                    continue;
                }
                if (sector[offset + 11] == 0x0F) {
                    lfn_add_entry(&lfn, &sector[offset]);
                    continue;
                }
                if (sector[offset + 11] & 0x08) {
                    lfn_reset(&lfn);
                    continue;
                }
                uint32_t i = 0;
                if (has_short_name)
                    for (; i < 11 && sector[offset + i] == wanted[i]; i++)
                        ;
                int short_match = has_short_name && i == 11;
                char decoded_name[FS_DIRENT_NAME_MAX];
                int long_match = lfn_decode(&lfn, &sector[offset], decoded_name,
                                            sizeof(decoded_name)) &&
                                 path_name_equal(name, decoded_name);
                lfn_reset(&lfn);
                if (!short_match && !long_match)
                    continue;
                if (result) {
                    result->attributes = sector[offset + 11];
                    result->cluster =
                        ((uint32_t)read16(sector, offset + 20) << 16) |
                        read16(sector, offset + 26);
                    result->size = read32(sector, offset + 28);
                }
                if (entry_sector)
                    *entry_sector = first + s;
                if (entry_offset)
                    *entry_offset = offset;
                return 1;
            }
        }
        uint32_t next = next_cluster(cluster);
        if (next >= 0x0FFFFFF8)
            break;
        cluster = next;
    }
    return 0;
}

static int create_named_entry(uint32_t directory, const char *name,
                              uint8_t attributes, uint32_t first_cluster) {
    struct fat32_entry existing;
    if (find_entry(directory, name, &existing, NULL, NULL))
        return 0;
    uint16_t units[260];
    size_t unit_count;
    uint8_t alias[11];
    int needs_long_name;
    if (!utf8_to_utf16(name, units, &unit_count) ||
        !make_short_alias(directory, name, alias, &needs_long_name))
        return 0;
    size_t long_entries = needs_long_name ? (unit_count + 12) / 13 : 0;
    size_t entry_count = long_entries + 1;
    uint32_t sectors[21], offsets[21];
    if (entry_count > 21 ||
        !find_free_slots(directory, entry_count, sectors, offsets))
        return 0;
    uint8_t sector[512];
    static const uint8_t positions[13] = {1,  3,  5,  7,  9,  14, 16,
                                          18, 20, 22, 24, 28, 30};
    uint8_t checksum = lfn_checksum(alias);
    for (size_t i = 0; i < long_entries; i++) {
        uint8_t item[32];
        for (uint32_t j = 0; j < 32; j++)
            item[j] = 0;
        uint32_t ordinal = (uint32_t)(long_entries - i);
        item[0] = (uint8_t)(ordinal | (i == 0 ? 0x40 : 0));
        item[11] = 0x0F;
        item[13] = checksum;
        write16(item, 26, 0);
        for (uint32_t j = 0; j < 13; j++) {
            size_t unit = (size_t)(ordinal - 1) * 13 + j;
            uint16_t value = unit < unit_count    ? units[unit]
                             : unit == unit_count ? 0
                                                  : 0xFFFF;
            write16(item, positions[j], value);
        }
        if (!block_read_sector(sectors[i], sector))
            return 0;
        for (uint32_t j = 0; j < 32; j++)
            sector[offsets[i] + j] = item[j];
        if (!block_write_sector(sectors[i], sector))
            return 0;
    }
    uint32_t short_index = long_entries;
    if (!block_read_sector(sectors[short_index], sector))
        return 0;
    for (uint32_t j = 0; j < 32; j++)
        sector[offsets[short_index] + j] = 0;
    for (uint32_t j = 0; j < 11; j++)
        sector[offsets[short_index] + j] = alias[j];
    sector[offsets[short_index] + 11] = attributes;
    write16(sector, offsets[short_index] + 20, first_cluster >> 16);
    write16(sector, offsets[short_index] + 26, first_cluster);
    write32(sector, offsets[short_index] + 28, 0);
    return block_write_sector(sectors[short_index], sector);
}

static int mark_entry_deleted(uint32_t directory, uint32_t target_sector,
                              uint32_t target_offset) {
    struct {
        uint32_t sector, offset;
    } long_slots[20];
    uint32_t long_count = 0, cluster = directory;
    struct lfn_state lfn;
    lfn_reset(&lfn);
    uint8_t sector[512];
    for (uint32_t chain = 0;
         chain < 65536 && cluster >= 2 && cluster < 0x0FFFFFF8; chain++) {
        uint32_t first = cluster_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!block_read_sector(first + s, sector))
                return 0;
            for (uint32_t offset = 0; offset < 512; offset += 32) {
                if (sector[offset] == 0)
                    return 0;
                if (sector[offset] == 0xE5) {
                    lfn_reset(&lfn);
                    long_count = 0;
                    continue;
                }
                if (sector[offset + 11] == 0x0F) {
                    lfn_add_entry(&lfn, &sector[offset]);
                    if (lfn.active && long_count < 20) {
                        long_slots[long_count].sector = first + s;
                        long_slots[long_count++].offset = offset;
                    } else
                        long_count = 0;
                    continue;
                }
                if (first + s == target_sector && offset == target_offset) {
                    char decoded[FS_DIRENT_NAME_MAX];
                    if (!lfn_decode(&lfn, &sector[offset], decoded,
                                    sizeof(decoded)))
                        long_count = 0;
                    for (uint32_t i = 0; i < long_count; i++) {
                        uint8_t lfn_sector[512];
                        if (!block_read_sector(long_slots[i].sector,
                                               lfn_sector))
                            return 0;
                        lfn_sector[long_slots[i].offset] = 0xE5;
                        if (!block_write_sector(long_slots[i].sector,
                                                lfn_sector))
                            return 0;
                    }
                    if (!block_read_sector(first + s, sector))
                        return 0;
                    sector[offset] = 0xE5;
                    return block_write_sector(first + s, sector);
                }
                lfn_reset(&lfn);
                long_count = 0;
            }
        }
        cluster = next_cluster(cluster);
    }
    return 0;
}

static int resolve_directory(const char *path, uint32_t *directory) {
    if (!mounted || !path || !directory)
        return 0;
    uint32_t current = root_cluster;
    while (*path == '/')
        path++;
    while (*path) {
        char part[FS_PATH_MAX];
        uint32_t n = 0;
        while (*path && *path != '/') {
            if (n >= sizeof(part) - 1)
                return 0;
            part[n++] = *path++;
        }
        while (*path == '/')
            path++;
        part[n] = 0;
        if (!n || (n == 1 && part[0] == '.'))
            continue;
        if (n == 2 && part[0] == '.' && part[1] == '.')
            return 0;
        struct fat32_entry entry;
        if (!find_entry(current, part, &entry, NULL, NULL) ||
            !(entry.attributes & 0x10) || entry.cluster < 2)
            return 0;
        current = entry.cluster;
    }
    *directory = current;
    return 1;
}

static int split_parent(const char *path, char parent[FS_PATH_MAX],
                        char leaf[FS_PATH_MAX]) {
    size_t length = 0;
    while (path && path[length] && length < FS_PATH_MAX)
        length++;
    if (!path || !length || path[length])
        return 0;
    while (length && path[length - 1] == '/')
        length--;
    if (!length)
        return 0;
    size_t start = length;
    while (start && path[start - 1] != '/')
        start--;
    size_t leaf_length = length - start;
    if (!leaf_length || leaf_length >= FS_PATH_MAX)
        return 0;
    for (size_t i = 0; i < leaf_length; i++)
        leaf[i] = path[start + i];
    leaf[leaf_length] = 0;
    size_t parent_length = start ? start - 1 : 0;
    if (parent_length >= FS_PATH_MAX)
        return 0;
    for (size_t i = 0; i < parent_length; i++)
        parent[i] = path[i];
    parent[parent_length] = 0;
    return 1;
}

int fat32_readdir(const char *path, size_t index, struct fs_dirent *entry) {
    uint32_t directory;
    if (!entry || !resolve_directory(path, &directory))
        return -1;
    uint8_t sector[512];
    struct lfn_state lfn;
    lfn_reset(&lfn);
    uint32_t cluster = directory;
    size_t visible = 0;
    for (uint32_t chain = 0;
         chain < 65536 && cluster >= 2 && cluster < 0x0FFFFFF8; chain++) {
        uint32_t first = cluster_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!block_read_sector(first + s, sector))
                return -1;
            for (uint32_t offset = 0; offset < 512; offset += 32) {
                if (sector[offset] == 0)
                    return 0;
                if (sector[offset] == 0xE5) {
                    lfn_reset(&lfn);
                    continue;
                }
                if (sector[offset + 11] == 0x0F) {
                    lfn_add_entry(&lfn, &sector[offset]);
                    continue;
                }
                if (sector[offset + 11] & 0x08) {
                    lfn_reset(&lfn);
                    continue;
                }
                if (visible++ != index) {
                    lfn_reset(&lfn);
                    continue;
                }
                if (!lfn_decode(&lfn, &sector[offset], entry->name,
                                sizeof(entry->name))) {
                    uint32_t out = 0;
                    for (uint32_t i = 0; i < 8 && sector[offset + i] != ' ';
                         i++)
                        entry->name[out++] = sector[offset + i];
                    if (sector[offset + 8] != ' ') {
                        entry->name[out++] = '.';
                        for (uint32_t i = 0;
                             i < 3 && sector[offset + 8 + i] != ' '; i++)
                            entry->name[out++] = sector[offset + 8 + i];
                    }
                    entry->name[out] = '\0';
                }
                entry->type = sector[offset + 11] & 0x10 ? FS_DIRENT_DIRECTORY
                                                         : FS_DIRENT_FILE;
                entry->size = read32(sector, offset + 28);
                return 1;
            }
        }
        cluster = next_cluster(cluster);
    }
    return 0;
}

int fat32_is_directory(const char *path) {
    uint32_t directory;
    return resolve_directory(path, &directory);
}

int fat32_make_directory(const char *path) {
    char parent[FS_PATH_MAX], leaf[FS_PATH_MAX];
    uint32_t parent_cluster;
    if (!split_parent(path, parent, leaf) ||
        !resolve_directory(parent, &parent_cluster))
        return 0;
    uint8_t sector[512];
    uint32_t child = allocate_cluster();
    if (!child)
        return 0;
    for (uint32_t i = 0; i < sectors_per_cluster; i++) {
        for (uint32_t j = 0; j < 512; j++)
            sector[j] = 0;
        if (!block_write_sector(cluster_sector(child) + i, sector))
            return 0;
    }
    uint8_t dot[512];
    if (!block_read_sector(cluster_sector(child), dot))
        return 0;
    for (uint32_t i = 0; i < 32; i++)
        dot[i] = 0;
    for (uint32_t i = 0; i < 11; i++)
        dot[i] = i == 0 ? '.' : ' ';
    dot[11] = 0x10;
    write16(dot, 20, child >> 16);
    write16(dot, 26, child);
    for (uint32_t i = 0; i < 11; i++)
        dot[32 + i] = i < 2 ? '.' : ' ';
    dot[43] = 0x10;
    write16(dot, 52, parent_cluster >> 16);
    write16(dot, 58, parent_cluster);
    if (!block_write_sector(cluster_sector(child), dot) ||
        !create_named_entry(parent_cluster, leaf, 0x10, child)) {
        (void)update_fat(child, 0);
        return 0;
    }
    return 1;
}

int fat32_remove_directory(const char *path) {
    char parent[FS_PATH_MAX], leaf[FS_PATH_MAX];
    uint32_t parent_cluster, dir_sector, dir_offset;
    struct fat32_entry entry;
    if (!split_parent(path, parent, leaf) ||
        !resolve_directory(parent, &parent_cluster) ||
        !find_entry(parent_cluster, leaf, &entry, &dir_sector, &dir_offset) ||
        !(entry.attributes & 0x10) || entry.cluster < 2)
        return 0;
    uint8_t sector[512];
    uint32_t cluster = entry.cluster;
    for (uint32_t chain = 0;
         chain < 65536 && cluster >= 2 && cluster < 0x0FFFFFF8; chain++) {
        uint32_t first = cluster_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!block_read_sector(first + s, sector))
                return 0;
            for (uint32_t offset = 0; offset < 512; offset += 32) {
                if (sector[offset] == 0)
                    goto empty;
                if (sector[offset] == 0xE5)
                    continue;
                if (sector[offset] == '.' &&
                    (sector[offset + 1] == ' ' || sector[offset + 1] == '.'))
                    continue;
                return 0;
            }
        }
        cluster = next_cluster(cluster);
    }
empty:
    if (!mark_entry_deleted(parent_cluster, dir_sector, dir_offset))
        return 0;
    cluster = entry.cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t next = next_cluster(cluster);
        if (!update_fat(cluster, 0))
            return 0;
        cluster = next;
    }
    return 1;
}

static int locate_path_entry(const char *path, uint32_t *parent_cluster,
                             char leaf[FS_PATH_MAX], struct fat32_entry *entry,
                             uint32_t *entry_sector, uint32_t *entry_offset) {
    char parent[FS_PATH_MAX];
    if (!split_parent(path, parent, leaf) ||
        !resolve_directory(parent, parent_cluster))
        return 0;
    return find_entry(*parent_cluster, leaf, entry, entry_sector, entry_offset);
}

int fat32_stat_file(const char *path, size_t *size) {
    char leaf[FS_PATH_MAX];
    uint32_t parent;
    struct fat32_entry entry;
    if (!size || !locate_path_entry(path, &parent, leaf, &entry, NULL, NULL) ||
        (entry.attributes & 0x10))
        return 0;
    *size = entry.size;
    return 1;
}

int fat32_read_file(const char *path, char *buffer, size_t capacity,
                    size_t *size) {
    char leaf[FS_PATH_MAX];
    uint32_t parent;
    struct fat32_entry entry;
    if (size)
        *size = 0;
    if (!buffer || !size ||
        !locate_path_entry(path, &parent, leaf, &entry, NULL, NULL) ||
        (entry.attributes & 0x10))
        return 0;
    size_t remaining = entry.size < capacity ? entry.size : capacity,
           copied = 0;
    uint32_t cluster = entry.cluster;
    uint8_t sector[512];
    while (remaining && cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t first = cluster_sector(cluster);
        for (uint32_t i = 0; i < sectors_per_cluster && remaining; i++) {
            if (!block_read_sector(first + i, sector))
                return 0;
            size_t amount = remaining < 512 ? remaining : 512;
            for (size_t j = 0; j < amount; j++)
                buffer[copied + j] = sector[j];
            copied += amount;
            remaining -= amount;
        }
        cluster = next_cluster(cluster);
    }
    if (remaining)
        return 0;
    *size = copied;
    return 1;
}

int fat32_create_file(const char *path) {
    char leaf[FS_PATH_MAX];
    uint32_t parent;
    struct fat32_entry existing;
    char parent_path[FS_PATH_MAX];
    if (!split_parent(path, parent_path, leaf) ||
        !resolve_directory(parent_path, &parent))
        return 0;
    if (find_entry(parent, leaf, &existing, NULL, NULL))
        return !(existing.attributes & 0x10);
    uint32_t data_cluster = allocate_cluster();
    if (!data_cluster)
        return 0;
    if (!create_named_entry(parent, leaf, 0x20, data_cluster)) {
        (void)update_fat(data_cluster, 0);
        return 0;
    }
    return 1;
}

int fat32_write_file(const char *path, const char *buffer, size_t size) {
    char leaf[FS_PATH_MAX];
    uint32_t parent, directory_sector, directory_offset;
    struct fat32_entry entry;
    if (!buffer || size > UINT32_MAX ||
        !locate_path_entry(path, &parent, leaf, &entry, &directory_sector,
                           &directory_offset) ||
        (entry.attributes & 0x10) || entry.cluster < 2)
        return 0;
    size_t cluster_bytes = (size_t)sectors_per_cluster * 512;
    size_t required = (size + cluster_bytes - 1) / cluster_bytes;
    if (!required)
        required = 1;
    uint32_t clusters[32];
    size_t count = 0;
    uint32_t cluster = entry.cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8 && count < 32) {
        clusters[count++] = cluster;
        cluster = next_cluster(cluster);
    }
    if (!count || required > 32)
        return 0;
    while (count < required) {
        uint32_t next = allocate_cluster();
        if (!next || !update_fat(clusters[count - 1], next))
            return 0;
        clusters[count++] = next;
    }
    if (count > required) {
        uint32_t next = next_cluster(clusters[required - 1]);
        if (!update_fat(clusters[required - 1], 0x0FFFFFFF))
            return 0;
        while (next >= 2 && next < 0x0FFFFFF8) {
            uint32_t following = next_cluster(next);
            if (!update_fat(next, 0))
                return 0;
            next = following;
        }
    }
    size_t copied = 0;
    uint8_t sector[512];
    for (size_t c = 0; c < required; c++) {
        uint32_t first = cluster_sector(clusters[c]);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            for (uint32_t j = 0; j < 512; j++)
                sector[j] = 0;
            size_t amount = copied < size ? size - copied : 0;
            if (amount > 512)
                amount = 512;
            for (size_t j = 0; j < amount; j++)
                sector[j] = buffer[copied + j];
            if (!block_write_sector(first + s, sector))
                return 0;
            copied += amount;
        }
    }
    if (!block_read_sector(directory_sector, sector))
        return 0;
    write32(sector, directory_offset + 28, (uint32_t)size);
    return block_write_sector(directory_sector, sector);
}

int fat32_unlink_file(const char *path) {
    char leaf[FS_PATH_MAX];
    uint32_t parent, directory_sector, directory_offset;
    struct fat32_entry entry;
    if (!locate_path_entry(path, &parent, leaf, &entry, &directory_sector,
                           &directory_offset) ||
        (entry.attributes & 0x10))
        return 0;
    if (!mark_entry_deleted(parent, directory_sector, directory_offset))
        return 0;
    uint32_t cluster = entry.cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t next = next_cluster(cluster);
        if (!update_fat(cluster, 0))
            return 0;
        cluster = next;
    }
    return 1;
}

int fat32_sync(void) { return mounted && block_flush(); }
