#include "vmm.h"
#include "../../limine/limine.h"
#include "../cpu/protection.h"
#include "../log.h"
#include "kmalloc.h"
#include "pmm.h"
#include <stddef.h>

#define PAGE_ADDRESS_MASK 0x000FFFFFFFFFF000ULL
#define PAGE_SIZE_FLAG (1ULL << 7)
#define CR4_LA57 (1ULL << 12)
#define ENTRY_COUNT 512

static struct vmm_space kernel_space;

__attribute__((used, section(".limine_requests"))) static volatile struct
    limine_kernel_address_request kernel_address_request = {
        .id = LIMINE_KERNEL_ADDRESS_REQUEST,
        .revision = 0,
};

extern char __kernel_text_start[], __kernel_text_end[];
extern char __kernel_rodata_start[], __kernel_rodata_end[];

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

static uint64_t page_table_virtual_address(uint64_t prefix, size_t index,
                                           int level) {
    unsigned int shift = 12 + 9 * (unsigned int)(level - 1);
    return prefix | ((uint64_t)index << shift);
}

static int clone_user_mappings_cow(struct vmm_space *child,
                                   uint64_t table_physical_address, int level,
                                   uint64_t prefix) {
    uint64_t *table = pmm_physical_to_virtual(table_physical_address);
    for (size_t i = 0; i < ENTRY_COUNT; i++) {
        if (level == 4 && i >= ENTRY_COUNT / 2)
            break;
        uint64_t entry = table[i];
        if (!(entry & VMM_PRESENT))
            continue;

        uint64_t address = page_table_virtual_address(prefix, i, level);
        if (level > 1) {
            if (entry & PAGE_SIZE_FLAG) {
                if (entry & VMM_USER)
                    return 0;
                continue;
            }
            if (!clone_user_mappings_cow(child, table_physical(entry),
                                         level - 1, address))
                return 0;
            continue;
        }

        if (!(entry & VMM_USER))
            continue;
        uint64_t flags = entry & (VMM_WRITABLE | VMM_USER | VMM_NO_EXECUTE |
                                  VMM_COPY_ON_WRITE);
        if (flags & VMM_WRITABLE) {
            flags &= ~VMM_WRITABLE;
            flags |= VMM_COPY_ON_WRITE;
        }
        uint64_t physical = table_physical(entry);
        if (!pmm_retain_page(physical))
            return 0;
        if (!vmm_space_map_page(child, address, physical, flags)) {
            pmm_free_page(physical);
            return 0;
        }
    }
    return 1;
}

static void protect_user_mappings_cow(struct vmm_space *space,
                                      uint64_t table_physical_address,
                                      int level, uint64_t prefix) {
    uint64_t *table = pmm_physical_to_virtual(table_physical_address);
    for (size_t i = 0; i < ENTRY_COUNT; i++) {
        if (level == 4 && i >= ENTRY_COUNT / 2)
            break;
        uint64_t entry = table[i];
        if (!(entry & VMM_PRESENT))
            continue;

        uint64_t address = page_table_virtual_address(prefix, i, level);
        if (level > 1) {
            if (!(entry & PAGE_SIZE_FLAG))
                protect_user_mappings_cow(space, table_physical(entry),
                                          level - 1, address);
            continue;
        }
        if ((entry & (VMM_PRESENT | VMM_USER | VMM_WRITABLE)) !=
            (VMM_PRESENT | VMM_USER | VMM_WRITABLE))
            continue;
        table[i] = (entry & ~VMM_WRITABLE) | VMM_COPY_ON_WRITE;
        uint64_t current_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
        if ((current_cr3 & PAGE_ADDRESS_MASK) == space->pml4_physical)
            __asm__ volatile("invlpg (%0)" : : "r"(address) : "memory");
    }
}

struct vmm_space *vmm_space_clone_cow(struct vmm_space *space) {
    if (space == NULL)
        return NULL;
    struct vmm_space *child = vmm_space_create();
    if (child == NULL)
        return NULL;

    uint64_t flags = interrupt_lock();
    if (!clone_user_mappings_cow(child, space->pml4_physical, 4, 0)) {
        interrupt_restore(flags);
        vmm_space_destroy(child);
        return NULL;
    }
    protect_user_mappings_cow(space, space->pml4_physical, 4, 0);
    interrupt_restore(flags);
    return child;
}

static int range_overlaps(uint64_t start, uint64_t size,
                          uint64_t protected_start, uint64_t protected_end) {
    return start < protected_end &&
           (start >= protected_start || protected_start - start < size);
}

static int range_contains(uint64_t protected_start, uint64_t protected_end,
                          uint64_t start, uint64_t size) {
    return start >= protected_start && start < protected_end &&
           size <= protected_end - start;
}

/* Split only huge mappings crossing a permission boundary; keep their PAT. */
static int split_boot_mapping(uint64_t *entry, int level) {
    uint64_t table_physical_address = pmm_alloc_page();
    if (table_physical_address == PMM_INVALID_ADDRESS)
        return 0;
    uint64_t size = 1ULL << (12 + 9 * (level - 1));
    uint64_t child_size = size / ENTRY_COUNT;
    uint64_t physical = (*entry & PAGE_ADDRESS_MASK) & ~(size - 1);
    uint64_t attributes = *entry & ~PAGE_ADDRESS_MASK;
    uint64_t large_pat = *entry & (1ULL << 12);
    if (level == 2) {
        attributes &= ~PAGE_SIZE_FLAG;
        if (large_pat)
            attributes |= 1ULL << 7;
    } else {
        attributes |= large_pat;
    }
    uint64_t *table = pmm_physical_to_virtual(table_physical_address);
    for (size_t i = 0; i < ENTRY_COUNT; i++)
        table[i] = (physical + i * child_size) | attributes;
    *entry = table_physical_address | VMM_PRESENT | VMM_WRITABLE;
    return 1;
}

static int protect_boot_table(uint64_t physical_table, int level,
                              uint64_t prefix, uint64_t readonly_start,
                              uint64_t readonly_end) {
    uint64_t *table = pmm_physical_to_virtual(physical_table);
    uint64_t text_start = (uint64_t)__kernel_text_start;
    uint64_t text_end = (uint64_t)__kernel_text_end;
    for (size_t i = 0; i < ENTRY_COUNT; i++) {
        uint64_t *entry = &table[i];
        if (!(*entry & VMM_PRESENT))
            continue;
        uint64_t address = page_table_virtual_address(prefix, i, level);
        if (level == 4 && (address & (1ULL << 47)))
            address |= 0xFFFF000000000000ULL;

        if (level == 1 || (*entry & PAGE_SIZE_FLAG)) {
            if (level == 4)
                return 0;
            uint64_t size = 1ULL << (12 + 9 * (level - 1));
            uint64_t physical = (*entry & PAGE_ADDRESS_MASK) & ~(size - 1);
            int readonly =
                range_contains(readonly_start, readonly_end, physical, size);
            int executable =
                range_contains(text_start, text_end, address, size);
            if (level > 1 &&
                ((!readonly && range_overlaps(physical, size, readonly_start,
                                              readonly_end)) ||
                 (!executable &&
                  range_overlaps(address, size, text_start, text_end)))) {
                if (!split_boot_mapping(entry, level))
                    return 0;
            } else {
                *entry &= ~VMM_USER;
                if (readonly || executable)
                    *entry &= ~VMM_WRITABLE;
                if (executable)
                    *entry &= ~VMM_NO_EXECUTE;
                else
                    *entry |= VMM_NO_EXECUTE;
                continue;
            }
        }

        /* Permissions belong to leaves, including future user mappings. */
        *entry = (*entry | VMM_WRITABLE) & ~(VMM_USER | VMM_NO_EXECUTE);
        if (!protect_boot_table(table_physical(*entry), level - 1, address,
                                readonly_start, readonly_end))
            return 0;
    }
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
    if (!cpu_enable_nx() || kernel_address_request.response == NULL) {
        LOG_ERROR("NX or kernel address information unavailable");
        return 0;
    }
    uint64_t physical_base = kernel_address_request.response->physical_base;
    uint64_t virtual_base = kernel_address_request.response->virtual_base;
    uint64_t readonly_start =
        physical_base + (uint64_t)__kernel_text_start - virtual_base;
    uint64_t readonly_end =
        physical_base + (uint64_t)__kernel_rodata_end - virtual_base;
    if (!protect_boot_table(kernel_space.pml4_physical, 4, 0, readonly_start,
                            readonly_end)) {
        LOG_ERROR("failed to protect kernel mappings");
        return 0;
    }
    /* CR3 alone leaves global translations cached when PGE is enabled. */
    if (cr4 & (1ULL << 7)) {
        uint64_t without_pge = cr4 & ~(1ULL << 7);
        __asm__ volatile("mov %0, %%cr4; mov %1, %%cr4"
                         :
                         : "r"(without_pge), "r"(cr4)
                         : "memory");
    } else {
        vmm_space_activate(&kernel_space);
    }
    cpu_enable_memory_protection();
    LOG_INFO("kernel mappings protected (W^X, supervisor-only)");
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
        physical_address == PMM_INVALID_ADDRESS ||
        ((flags & VMM_WRITABLE) && !(flags & VMM_NO_EXECUTE)) ||
        ((flags & VMM_USER) && (virtual_address >= 0x0000800000000000ULL ||
                                space == &kernel_space)) ||
        (virtual_address >= 0xFFFF800000000000ULL && space != &kernel_space))
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

    *page_entry =
        physical_address | VMM_PRESENT |
        (flags & (VMM_WRITABLE | VMM_USER | VMM_WRITE_THROUGH |
                  VMM_CACHE_DISABLE | VMM_NO_EXECUTE | VMM_COPY_ON_WRITE));
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

uint64_t vmm_space_unmap_page(struct vmm_space *space,
                              uint64_t virtual_address) {
    if (!address_is_canonical(virtual_address) ||
        virtual_address % PMM_PAGE_SIZE != 0 || space == NULL ||
        (virtual_address >= 0xFFFF800000000000ULL && space != &kernel_space))
        return PMM_INVALID_ADDRESS;

    size_t indices[4] = {
        (virtual_address >> 39) & 0x1FF,
        (virtual_address >> 30) & 0x1FF,
        (virtual_address >> 21) & 0x1FF,
        (virtual_address >> 12) & 0x1FF,
    };

    uint64_t lock_flags = interrupt_lock();
    uint64_t *root = space_root(space);
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
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if ((current_cr3 & PAGE_ADDRESS_MASK) == space->pml4_physical)
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

uint64_t vmm_unmap_page(uint64_t virtual_address) {
    return vmm_space_unmap_page(&kernel_space, virtual_address);
}

uint64_t vmm_space_translate(const struct vmm_space *space,
                             uint64_t virtual_address) {
    uint64_t physical;
    if (!vmm_space_query(space, virtual_address, &physical, NULL))
        return PMM_INVALID_ADDRESS;
    return physical;
}

int vmm_space_query(const struct vmm_space *space, uint64_t virtual_address,
                    uint64_t *physical_address, uint64_t *flags) {
    if (space == NULL || !address_is_canonical(virtual_address))
        return 0;

    size_t indices[4] = {
        (virtual_address >> 39) & 0x1FF,
        (virtual_address >> 30) & 0x1FF,
        (virtual_address >> 21) & 0x1FF,
        (virtual_address >> 12) & 0x1FF,
    };

    uint64_t lock_flags = interrupt_lock();
    uint64_t *table = space_root(space);
    uint64_t effective = VMM_WRITABLE | VMM_USER;
    uint64_t no_execute = 0;
    for (size_t level = 0; level < 4; level++) {
        uint64_t entry = table[indices[level]];
        if (!(entry & VMM_PRESENT) ||
            (level == 0 && (entry & PAGE_SIZE_FLAG))) {
            interrupt_restore(lock_flags);
            return 0;
        }
        effective &= entry & (VMM_WRITABLE | VMM_USER);
        no_execute |= entry & VMM_NO_EXECUTE;
        if (level == 3 || (entry & PAGE_SIZE_FLAG)) {
            uint64_t size = 1ULL << (12 + 9 * (3 - level));
            if (physical_address != NULL)
                *physical_address =
                    ((entry & PAGE_ADDRESS_MASK) & ~(size - 1)) +
                    (virtual_address & (size - 1));
            if (flags != NULL)
                *flags = VMM_PRESENT | effective | no_execute |
                         (entry & VMM_COPY_ON_WRITE);
            interrupt_restore(lock_flags);
            return 1;
        }
        table = table_virtual(entry);
    }
    interrupt_restore(lock_flags);
    return 0;
}

int vmm_space_resolve_cow(struct vmm_space *space, uint64_t virtual_address) {
    if (space == NULL || !address_is_canonical(virtual_address))
        return 0;

    size_t indices[4] = {
        (virtual_address >> 39) & 0x1FF,
        (virtual_address >> 30) & 0x1FF,
        (virtual_address >> 21) & 0x1FF,
        (virtual_address >> 12) & 0x1FF,
    };

    uint64_t flags = interrupt_lock();
    uint64_t *table = space_root(space);
    for (size_t level = 0; level < 3; level++) {
        uint64_t entry = table[indices[level]];
        if (!(entry & VMM_PRESENT) || (entry & PAGE_SIZE_FLAG)) {
            interrupt_restore(flags);
            return 0;
        }
        table = table_virtual(entry);
    }

    uint64_t *page_entry = &table[indices[3]];
    uint64_t old_entry = *page_entry;
    if ((old_entry & (VMM_PRESENT | VMM_USER | VMM_COPY_ON_WRITE)) !=
            (VMM_PRESENT | VMM_USER | VMM_COPY_ON_WRITE) ||
        (old_entry & VMM_WRITABLE)) {
        interrupt_restore(flags);
        return 0;
    }

    uint64_t old_physical = table_physical(old_entry);
    uint16_t references = pmm_page_references(old_physical);
    if (references == 0) {
        interrupt_restore(flags);
        return 0;
    }

    uint64_t new_entry;
    if (references == 1) {
        new_entry = (old_entry | VMM_WRITABLE) & ~VMM_COPY_ON_WRITE;
    } else {
        uint64_t new_physical = pmm_alloc_user_page();
        if (new_physical == PMM_INVALID_ADDRESS) {
            interrupt_restore(flags);
            return -1;
        }
        uint8_t *source = pmm_physical_to_virtual(old_physical);
        uint8_t *destination = pmm_physical_to_virtual(new_physical);
        for (size_t i = 0; i < PMM_PAGE_SIZE; i++)
            destination[i] = source[i];
        new_entry = new_physical | (old_entry & ~PAGE_ADDRESS_MASK);
        new_entry = (new_entry | VMM_WRITABLE) & ~VMM_COPY_ON_WRITE;
        pmm_free_page(old_physical);
    }
    *page_entry = new_entry;

    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    uint64_t page_address = virtual_address & ~(uint64_t)(PMM_PAGE_SIZE - 1);
    if ((current_cr3 & PAGE_ADDRESS_MASK) == space->pml4_physical)
        __asm__ volatile("invlpg (%0)" : : "r"(page_address) : "memory");
    interrupt_restore(flags);
    return 1;
}

uint64_t vmm_translate(uint64_t virtual_address) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    struct vmm_space current = {
        .pml4_physical = cr3 & PAGE_ADDRESS_MASK,
    };
    return vmm_space_translate(&current, virtual_address);
}
