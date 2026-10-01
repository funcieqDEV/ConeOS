#include "initramfs.h"
#include "../../limine/limine.h"
#include "ramfs.h"
#include <stddef.h>
#include <stdint.h>

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_module_request
    module_request = {
        .id = LIMINE_MODULE_REQUEST,
        .revision = 0,
};

static size_t octal_size(const char *text, size_t length) {
    size_t value = 0;
    for (size_t i = 0; i < length && text[i]; i++) {
        if (text[i] < '0' || text[i] > '7')
            continue;
        value = value * 8 + (size_t)(text[i] - '0');
    }
    return value;
}

static int block_empty(const uint8_t *block) {
    for (size_t i = 0; i < 512; i++)
        if (block[i])
            return 0;
    return 1;
}

int initramfs_mount(void) {
    if (module_request.response == NULL ||
        module_request.response->module_count == 0)
        return 0;
    struct limine_file *module = module_request.response->modules[0];
    const uint8_t *archive = module->address;
    size_t offset = 0;
    while (offset + 512 <= module->size) {
        const uint8_t *header = archive + offset;
        if (block_empty(header))
            break;
        const char *name = (const char *)header;
        size_t size = octal_size((const char *)header + 124, 12);
        char type = (char)header[156];
        offset += 512;
        if (offset + size > module->size)
            return 0;
        if (type == '0' || type == '\0') {
            if (size > RAMFS_DATA_SIZE)
                return 0;
            char path[RAMFS_NAME_SIZE];
            size_t source = 0, target = 0;
            if (name[0] == '.' && name[1] == '/')
                source = 1;
            if (name[source] != '/')
                path[target++] = '/';
            while (name[source] && target < sizeof(path) - 1)
                path[target++] = name[source++];
            path[target] = '\0';
            if (!ramfs_write(path, (const char *)archive + offset, size))
                return 0;
        }
        offset += (size + 511) & ~511ULL;
    }
    return 1;
}
