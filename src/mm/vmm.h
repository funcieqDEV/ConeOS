#pragma once

#include <stdint.h>

#define VMM_PRESENT (1ULL << 0)
#define VMM_WRITABLE (1ULL << 1)
#define VMM_USER (1ULL << 2)
#define VMM_NO_EXECUTE (1ULL << 63)

struct vmm_space {
    uint64_t pml4_physical;
};

int vmm_init(void);
struct vmm_space *vmm_kernel_space(void);
struct vmm_space *vmm_space_create(void);
void vmm_space_destroy(struct vmm_space *space);
void vmm_space_activate(struct vmm_space *space);
int vmm_space_map_page(struct vmm_space *space, uint64_t virtual_address,
                       uint64_t physical_address, uint64_t flags);
uint64_t vmm_space_translate(const struct vmm_space *space,
                             uint64_t virtual_address);
int vmm_map_page(uint64_t virtual_address, uint64_t physical_address,
                 uint64_t flags);
uint64_t vmm_unmap_page(uint64_t virtual_address);
uint64_t vmm_translate(uint64_t virtual_address);
