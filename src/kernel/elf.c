#include "elf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

#define ELF_CLASS_64 2
#define ELF_DATA_LSB 1
#define ELF_CURRENT_VERSION 1
#define ELF_TYPE_EXECUTABLE 2
#define ELF_MACHINE_X86_64 62
#define ELF_PROGRAM_LOAD 1
#define ELF_FLAG_EXECUTE 1
#define ELF_FLAG_WRITE 2
#define USER_ADDRESS_LIMIT 0x0000800000000000ULL
#define USER_STACK_ADDRESS 0x0000000000800000ULL

struct elf64_header {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_offset;
    uint64_t section_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_entry_size;
    uint16_t program_count;
    uint16_t section_entry_size;
    uint16_t section_count;
    uint16_t section_names;
} __attribute__((packed));

struct elf64_program_header {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
} __attribute__((packed));

static uint64_t page_floor(uint64_t value) {
    return value & ~(PMM_PAGE_SIZE - 1);
}

static int range_valid(uint64_t start, uint64_t size, uint64_t limit) {
    return start <= limit && size <= limit - start;
}

static void copy_bytes(uint8_t *destination, const uint8_t *source,
                       uint64_t count) {
    for (uint64_t i = 0; i < count; i++)
        destination[i] = source[i];
}

static enum elf_load_result load_segment(
    const uint8_t *image, size_t image_size,
    const struct elf64_program_header *program, struct vmm_space *space) {
    if (program->file_size > program->memory_size ||
        !range_valid(program->offset, program->file_size, image_size) ||
        !range_valid(program->virtual_address, program->memory_size,
                     USER_ADDRESS_LIMIT) ||
        (program->flags & ELF_FLAG_WRITE &&
         program->flags & ELF_FLAG_EXECUTE))
        return ELF_LOAD_INVALID;
    if (program->memory_size == 0)
        return ELF_LOAD_SUCCESS;

    uint64_t end = program->virtual_address + program->memory_size;
    uint64_t flags = VMM_USER;
    if (program->flags & ELF_FLAG_WRITE)
        flags |= VMM_WRITABLE;
    if (!(program->flags & ELF_FLAG_EXECUTE))
        flags |= VMM_NO_EXECUTE;

    for (uint64_t page = page_floor(program->virtual_address); page < end;
         page += PMM_PAGE_SIZE) {
        if (vmm_space_translate(space, page) != PMM_INVALID_ADDRESS)
            return ELF_LOAD_INVALID;
        uint64_t physical = pmm_alloc_user_page();
        if (physical == PMM_INVALID_ADDRESS)
            return ELF_LOAD_OUT_OF_MEMORY;
        uint8_t *memory = pmm_physical_to_virtual(physical);
        for (size_t i = 0; i < PMM_PAGE_SIZE; i++)
            memory[i] = 0;
        if (!vmm_space_map_page(space, page, physical, flags)) {
            pmm_free_page(physical);
            return ELF_LOAD_OUT_OF_MEMORY;
        }
    }

    uint64_t copied = 0;
    while (copied < program->file_size) {
        uint64_t address = program->virtual_address + copied;
        uint64_t physical = vmm_space_translate(space, address);
        if (physical == PMM_INVALID_ADDRESS)
            return ELF_LOAD_INVALID;
        uint64_t chunk = PMM_PAGE_SIZE - (address & (PMM_PAGE_SIZE - 1));
        if (chunk > program->file_size - copied)
            chunk = program->file_size - copied;
        copy_bytes(pmm_physical_to_virtual(physical),
                   image + program->offset + copied, chunk);
        copied += chunk;
    }
    return ELF_LOAD_SUCCESS;
}

enum elf_load_result elf_load(const uint8_t *image, size_t image_size,
                              struct vmm_space *space, uint64_t page_limit,
                              uint64_t *entry_point, uint64_t *image_end,
                              uint64_t *mapped_pages) {
    if (image == NULL || space == NULL || entry_point == NULL ||
        image_end == NULL || mapped_pages == NULL ||
        image_size < sizeof(struct elf64_header))
        return ELF_LOAD_INVALID;

    const struct elf64_header *header = (const void *)image;
    if (header->ident[0] != 0x7F || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F' ||
        header->ident[4] != ELF_CLASS_64 ||
        header->ident[5] != ELF_DATA_LSB ||
        header->ident[6] != ELF_CURRENT_VERSION ||
        header->type != ELF_TYPE_EXECUTABLE ||
        header->machine != ELF_MACHINE_X86_64 ||
        header->version != ELF_CURRENT_VERSION ||
        header->program_entry_size != sizeof(struct elf64_program_header) ||
        header->program_count == 0 ||
        !range_valid(header->program_offset,
                     (uint64_t)header->program_count *
                         header->program_entry_size,
                     image_size) ||
        header->entry >= USER_ADDRESS_LIMIT)
        return ELF_LOAD_INVALID;

    int entry_is_executable = 0;
    uint64_t highest_end = 0, total_pages = 0;
    const struct elf64_program_header *programs =
        (const void *)(image + header->program_offset);
    for (size_t i = 0; i < header->program_count; i++) {
        const struct elf64_program_header *program = &programs[i];
        if (program->type != ELF_PROGRAM_LOAD)
            continue;
        if (!range_valid(program->virtual_address, program->memory_size,
                         USER_ADDRESS_LIMIT))
            return ELF_LOAD_INVALID;
        uint64_t segment_end = program->virtual_address + program->memory_size;
        uint64_t segment_pages = 0;
        if (program->memory_size != 0) {
            segment_pages =
                (segment_end - 1 - page_floor(program->virtual_address)) /
                    PMM_PAGE_SIZE +
                1;
        }
        if (segment_pages > page_limit - total_pages)
            return ELF_LOAD_OUT_OF_MEMORY;

        enum elf_load_result result =
            load_segment(image, image_size, program, space);
        if (result != ELF_LOAD_SUCCESS)
            return result;
        total_pages += segment_pages;
        if (segment_end > highest_end) highest_end = segment_end;
        if ((program->flags & ELF_FLAG_EXECUTE) &&
            header->entry >= program->virtual_address &&
            header->entry - program->virtual_address < program->memory_size)
            entry_is_executable = 1;
    }
    if (!entry_is_executable)
        return ELF_LOAD_INVALID;

    if (highest_end >= USER_STACK_ADDRESS - PMM_PAGE_SIZE)
        return ELF_LOAD_INVALID;
    *entry_point = header->entry;
    *image_end = highest_end;
    *mapped_pages = total_pages;
    return ELF_LOAD_SUCCESS;
}
