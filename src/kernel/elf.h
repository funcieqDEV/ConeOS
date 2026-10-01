#pragma once

#include <stddef.h>
#include <stdint.h>

struct vmm_space;

enum elf_load_result {
    ELF_LOAD_OUT_OF_MEMORY = -1,
    ELF_LOAD_INVALID = 0,
    ELF_LOAD_SUCCESS = 1,
};

enum elf_load_result elf_load(const uint8_t *image, size_t image_size,
                              struct vmm_space *space, uint64_t page_limit,
                              uint64_t *entry_point, uint64_t *image_end,
                              uint64_t *mapped_pages);
