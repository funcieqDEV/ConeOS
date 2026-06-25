#include "vmm.h"
#include "../log.h"
#include "kmalloc.h"
#include "pmm.h"
#include <stddef.h>

#define PAGE_ADDRESS_MASK 0x000FFFFFFFFFF000ULL
#define PAGE_SIZE_FLAG (1ULL << 7)
#define CR4_LA57 (1ULL << 12)
#define ENTRY_COUNT 512

static struct vmm_space kernel_space;

static int address_is_canonical(uint64_t address) {
    uint64_t upper = address >> 48;
    return upper == ((address & (1ULL << 47)) ? 0xFFFF : 0);
}

static uint64_t interrupt_lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void interrupt_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

static uint64_t table_physical(uint64_t entry) {
    return entry & PAGE_ADDRESS_MASK;
}

static uint64_t *table_virtual(uint64_t entry) {
    return pmm_physical_to_virtual(table_physical(entry));
}

static uint64_t *space_root(const struct vmm_space *space) {
    return pmm_physical_to_virtual(space->pml4_physical);
}

static void clear_table(uint64_t *table) {
    for (size_t i = 0; i < ENTRY_COUNT; i++)
        table[i] = 0;
}

static void destroy_cloned_tables(uint64_t physical, int level) {
    uint64_t *table = pmm_physical_to_virtual(physical);
    if (level > 1) {
        for (size_t i = 0; i < ENTRY_COUNT; i++) {
            uint64_t entry = table[i];
            if ((entry & VMM_PRESENT) && !(entry & PAGE_SIZE_FLAG))
                destroy_cloned_tables(table_physical(entry), level - 1);
        }
    }
    pmm_free_page(physical);
}

static uint64_t clone_table(uint64_t source_physical, int level) {
    uint64_t destination_physical = pmm_alloc_page();
    if (destination_physical == PMM_INVALID_ADDRESS)
        return PMM_INVALID_ADDRESS;

    uint64_t *source = pmm_physical_to_virtual(source_physical);
    uint64_t *destination = pmm_physical_to_virtual(destination_physical);
    clear_table(destination);

    for (size_t i = 0; i < ENTRY_COUNT; i++) {
        uint64_t entry = source[i];
        if (!(entry & VMM_PRESENT) || level == 1 || (entry & PAGE_SIZE_FLAG)) {
            destination[i] = entry;
            continue;
        }

        uint64_t child = clone_table(table_physical(entry), level - 1);
        if (child == PMM_INVALID_ADDRESS) {
            destroy_cloned_tables(destination_physical, level);
            return PMM_INVALID_ADDRESS;
        }
        destination[i] = child | (entry & ~PAGE_ADDRESS_MASK);
    }
    return destination_physical;
}

static int table_is_empty(const uint64_t *table) {
    for (size_t i = 0; i < ENTRY_COUNT; i++) {
        if (table[i] & VMM_PRESENT)
            return 0;
    }
    return 1;
}

static int create_table(uint64_t *entry, uint64_t flags) {
    uint64_t physical = pmm_alloc_page();
    if (physical == PMM_INVALID_ADDRESS)
        return 0;

    uint64_t *table = pmm_physical_to_virtual(physical);
    clear_table(table);
    *entry = physical | VMM_PRESENT | VMM_WRITABLE | (flags & VMM_USER);
    return 1;
}

int vmm_init(void) {
    uint64_t cr3;
    uint64_t cr4;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

    if (cr4 & CR4_LA57) {
        LOG_ERROR("5-level paging is not supported");
        return 0;
    }

    kernel_space.pml4_physical = cr3 & PAGE_ADDRESS_MASK;
    LOG_INFO("virtual memory manager initialized");
    return 1;
}

struct vmm_space *vmm_kernel_space(void) { return &kernel_space; }

struct vmm_space *vmm_space_create(void) {
    struct vmm_space *space = kmalloc(sizeof(*space));
    if (space == NULL)
        return NULL;

    uint64_t physical = pmm_alloc_page();
    if (physical == PMM_INVALID_ADDRESS) {
        kfree(space);
        return NULL;
    }

    uint64_t *root = pmm_physical_to_virtual(physical);
    uint64_t *kernel_root = space_root(&kernel_space);
    clear_table(root);
    for (size_t i = 0; i < ENTRY_COUNT / 2; i++) {
        uint64_t entry = kernel_root[i];
        if (!(entry & VMM_PRESENT))
            continue;

        uint64_t child = clone_table(table_physical(entry), 3);
        if (child == PMM_INVALID_ADDRESS) {
            for (size_t j = 0; j < i; j++) {
                if (root[j] & VMM_PRESENT)
                    destroy_cloned_tables(table_physical(root[j]), 3);
            }
            pmm_free_page(physical);
            kfree(space);
            return NULL;
        }
        root[i] = child | (entry & ~PAGE_ADDRESS_MASK);
    }
    for (size_t i = ENTRY_COUNT / 2; i < ENTRY_COUNT; i++)
        root[i] = kernel_root[i];

    space->pml4_physical = physical;
    return space;
}

static void destroy_table(uint64_t physical, int level) {
    uint64_t *table = pmm_physical_to_virtual(physical);
    for (size_t i = 0; i < ENTRY_COUNT; i++) {
        uint64_t entry = table[i];
        if (!(entry & VMM_PRESENT))
            continue;

        uint64_t child = table_physical(entry);
        if (level == 1 || (entry & PAGE_SIZE_FLAG)) {
            if (entry & VMM_USER)
                pmm_free_page(child);
        } else
            destroy_table(child, level - 1);
    }
    pmm_free_page(physical);
}

void vmm_space_destroy(struct vmm_space *space) {
    if (space == NULL || space == &kernel_space)
        return;

    uint64_t flags = interrupt_lock();
    uint64_t *root = space_root(space);
    for (size_t i = 0; i < ENTRY_COUNT / 2; i++) {
        if (root[i] & VMM_PRESENT)
            destroy_table(table_physical(root[i]), 3);
    }
    pmm_free_page(space->pml4_physical);
    interrupt_restore(flags);
    kfree(space);
}

void vmm_space_activate(struct vmm_space *space) {
    if (space != NULL)
        __asm__ volatile("mov %0, %%cr3"
                         :
                         : "r"(space->pml4_physical)
                         : "memory");
}

int vmm_space_map_page(struct vmm_space *space, uint64_t virtual_address,
                       uint64_t physical_address, uint64_t flags) {
    if (space == NULL || !address_is_canonical(virtual_address) ||
        virtual_address % PMM_PAGE_SIZE != 0 ||
        physical_address % PMM_PAGE_SIZE != 0 ||
        physical_address == PMM_INVALID_ADDRESS)
        return 0;

    size_t indices[4] = {
        (virtual_address >> 39) & 0x1FF,
        (virtual_address >> 30) & 0x1FF,
        (virtual_address >> 21) & 0x1FF,
        (virtual_address >> 12) & 0x1FF,
    };

    uint64_t lock_flags = interrupt_lock();
    uint64_t *table = space_root(space);
    uint64_t *created_entries[3];
    uint64_t created_pages[3];
    size_t created_count = 0;

    for (size_t level = 0; level < 3; level++) {
        uint64_t *entry = &table[indices[level]];
        if (!(*entry & VMM_PRESENT)) {
            if (!create_table(entry, flags))
                goto rollback;
            created_entries[created_count] = entry;
            created_pages[created_count] = table_physical(*entry);
            created_count++;
        } else if (*entry & PAGE_SIZE_FLAG) {
            goto rollback;
        }
        table = table_virtual(*entry);
    }

    uint64_t *page_entry = &table[indices[3]];
    if (*page_entry & VMM_PRESENT)
        goto rollback;

    *page_entry = physical_address | VMM_PRESENT |
                  (flags & (VMM_WRITABLE | VMM_USER | VMM_NO_EXECUTE));
    if (flags & VMM_USER) {
        table = space_root(space);
        for (size_t level = 0; level < 3; level++) {
            table[indices[level]] |= VMM_USER;
            table = table_virtual(table[indices[level]]);
        }
    }
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if ((current_cr3 & PAGE_ADDRESS_MASK) == space->pml4_physical)
        __asm__ volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
    interrupt_restore(lock_flags);
    return 1;

rollback:
    while (created_count > 0) {
        created_count--;
        *created_entries[created_count] = 0;
        pmm_free_page(created_pages[created_count]);
    }
    interrupt_restore(lock_flags);
    return 0;
}

int vmm_map_page(uint64_t virtual_address, uint64_t physical_address,
                 uint64_t flags) {
    return vmm_space_map_page(&kernel_space, virtual_address, physical_address,
                              flags);
}

uint64_t vmm_unmap_page(uint64_t virtual_address) {
    if (!address_is_canonical(virtual_address) ||
        virtual_address % PMM_PAGE_SIZE != 0)
        return PMM_INVALID_ADDRESS;

    size_t indices[4] = {
        (virtual_address >> 39) & 0x1FF,
        (virtual_address >> 30) & 0x1FF,
        (virtual_address >> 21) & 0x1FF,
        (virtual_address >> 12) & 0x1FF,
    };

    uint64_t lock_flags = interrupt_lock();
    uint64_t *root = space_root(&kernel_space);
    uint64_t *tables[4] = {root};
    uint64_t *table = root;

    for (size_t level = 0; level < 3; level++) {
        uint64_t entry = table[indices[level]];
        if (!(entry & VMM_PRESENT) || (entry & PAGE_SIZE_FLAG)) {
            interrupt_restore(lock_flags);
            return PMM_INVALID_ADDRESS;
        }
        table = table_virtual(entry);
        tables[level + 1] = table;
    }

    uint64_t *page_entry = &tables[3][indices[3]];
    if (!(*page_entry & VMM_PRESENT)) {
        interrupt_restore(lock_flags);
        return PMM_INVALID_ADDRESS;
    }

    uint64_t physical_address = table_physical(*page_entry);
    *page_entry = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");

    for (size_t level = 3; level > 0; level--) {
        if (!table_is_empty(tables[level]))
            break;

        uint64_t *parent_entry = &tables[level - 1][indices[level - 1]];
        uint64_t table_page = table_physical(*parent_entry);
        *parent_entry = 0;
        pmm_free_page(table_page);
    }

    interrupt_restore(lock_flags);
    return physical_address;
}

uint64_t vmm_space_translate(const struct vmm_space *space,
                             uint64_t virtual_address) {
    if (space == NULL || !address_is_canonical(virtual_address))
        return PMM_INVALID_ADDRESS;

    size_t indices[4] = {
        (virtual_address >> 39) & 0x1FF,
        (virtual_address >> 30) & 0x1FF,
        (virtual_address >> 21) & 0x1FF,
        (virtual_address >> 12) & 0x1FF,
    };

    uint64_t lock_flags = interrupt_lock();
    uint64_t *table = space_root(space);
    for (size_t level = 0; level < 3; level++) {
        uint64_t entry = table[indices[level]];
        if (!(entry & VMM_PRESENT) || (entry & PAGE_SIZE_FLAG)) {
            interrupt_restore(lock_flags);
            return PMM_INVALID_ADDRESS;
        }
        table = table_virtual(entry);
    }

    uint64_t entry = table[indices[3]];
    interrupt_restore(lock_flags);
    if (!(entry & VMM_PRESENT))
        return PMM_INVALID_ADDRESS;

    return table_physical(entry) + (virtual_address & (PMM_PAGE_SIZE - 1));
}

uint64_t vmm_translate(uint64_t virtual_address) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    struct vmm_space current = {
        .pml4_physical = cr3 & PAGE_ADDRESS_MASK,
    };
    return vmm_space_translate(&current, virtual_address);
}
