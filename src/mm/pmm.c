#include "pmm.h"
#include "../../limine/limine.h"
#include "../log.h"
#include <stddef.h>

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_memmap_request
    memmap_request = {.id = LIMINE_MEMMAP_REQUEST, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_hhdm_request
    hhdm_request = {.id = LIMINE_HHDM_REQUEST, .revision = 0};

static uint8_t *bitmap;
static uint16_t *page_references;
static uint64_t bitmap_physical;
static uint64_t bitmap_page_count;
static uint64_t references_physical;
static uint64_t references_page_count;
static uint64_t page_count;
static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t search_start;
static uint64_t hhdm_offset;
static struct limine_memmap_response *memory_map;

#define PMM_KERNEL_RESERVE_PAGES (4ULL * 1024 * 1024 / PMM_PAGE_SIZE)

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

static int bitmap_test(uint64_t page) {
    return bitmap[page / 8] & (1u << (page % 8));
}

static void bitmap_set(uint64_t page) { bitmap[page / 8] |= 1u << (page % 8); }

static void bitmap_clear(uint64_t page) {
    bitmap[page / 8] &= ~(1u << (page % 8));
}

static uint64_t interrupt_lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void interrupt_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

static int page_is_usable(uint64_t physical_address) {
    for (uint64_t i = 0; i < memory_map->entry_count; i++) {
        struct limine_memmap_entry *entry = memory_map->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;
        if (physical_address >= entry->base &&
            physical_address + PMM_PAGE_SIZE <= entry->base + entry->length)
            return 1;
    }
    return 0;
}

int pmm_init(void) {
    if (memmap_request.response == NULL || hhdm_request.response == NULL)
        return 0;

    memory_map = memmap_request.response;
    hhdm_offset = hhdm_request.response->offset;

    uint64_t highest_address = 0;
    for (uint64_t i = 0; i < memory_map->entry_count; i++) {
        struct limine_memmap_entry *entry = memory_map->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;
        if (entry->base + entry->length > highest_address)
            highest_address = entry->base + entry->length;
    }

    page_count = align_up(highest_address, PMM_PAGE_SIZE) / PMM_PAGE_SIZE;
    uint64_t bitmap_size = align_up(page_count, 8) / 8;
    bitmap_page_count = align_up(bitmap_size, PMM_PAGE_SIZE) / PMM_PAGE_SIZE;
    uint64_t references_size = page_count * sizeof(*page_references);
    references_page_count =
        align_up(references_size, PMM_PAGE_SIZE) / PMM_PAGE_SIZE;
    uint64_t metadata_page_count = bitmap_page_count + references_page_count;

    int bitmap_region_found = 0;
    for (uint64_t i = 0; i < memory_map->entry_count; i++) {
        struct limine_memmap_entry *entry = memory_map->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t start = align_up(entry->base, PMM_PAGE_SIZE);
        uint64_t end = align_down(entry->base + entry->length, PMM_PAGE_SIZE);
        if (end - start >= metadata_page_count * PMM_PAGE_SIZE) {
            bitmap_physical = start;
            references_physical = start + bitmap_page_count * PMM_PAGE_SIZE;
            bitmap_region_found = 1;
            break;
        }
    }

    if (!bitmap_region_found)
        return 0;

    bitmap = pmm_physical_to_virtual(bitmap_physical);
    page_references = pmm_physical_to_virtual(references_physical);
    for (uint64_t i = 0; i < bitmap_size; i++)
        bitmap[i] = 0xFF;
    for (uint64_t i = 0; i < page_count; i++)
        page_references[i] = 0;

    for (uint64_t i = 0; i < memory_map->entry_count; i++) {
        struct limine_memmap_entry *entry = memory_map->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t start = align_up(entry->base, PMM_PAGE_SIZE);
        uint64_t end = align_down(entry->base + entry->length, PMM_PAGE_SIZE);
        for (uint64_t address = start; address < end;
             address += PMM_PAGE_SIZE) {
            uint64_t page = address / PMM_PAGE_SIZE;
            bitmap_clear(page);
            total_pages++;
            free_pages++;
        }
    }

    uint64_t bitmap_first_page = bitmap_physical / PMM_PAGE_SIZE;
    for (uint64_t i = 0; i < metadata_page_count; i++) {
        uint64_t page = bitmap_first_page + i;
        if (!bitmap_test(page)) {
            bitmap_set(page);
            free_pages--;
        }
    }

    search_start = bitmap_first_page + metadata_page_count;
    LOG_INFO("physical memory manager initialized");
    return 1;
}

static uint64_t alloc_page_with_reserve(uint64_t reserve_pages) {
    uint64_t flags = interrupt_lock();
    if (free_pages <= reserve_pages) {
        interrupt_restore(flags);
        return PMM_INVALID_ADDRESS;
    }

    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t start = pass == 0 ? search_start : 0;
        uint64_t end = pass == 0 ? page_count : search_start;

        for (uint64_t page = start; page < end; page++) {
            if (bitmap_test(page))
                continue;

            bitmap_set(page);
            page_references[page] = 1;
            free_pages--;
            search_start = page + 1;
            interrupt_restore(flags);
            return page * PMM_PAGE_SIZE;
        }
    }

    interrupt_restore(flags);
    return PMM_INVALID_ADDRESS;
}

uint64_t pmm_alloc_page(void) { return alloc_page_with_reserve(0); }

uint64_t pmm_alloc_user_page(void) {
    return alloc_page_with_reserve(PMM_KERNEL_RESERVE_PAGES);
}

uint64_t pmm_alloc_contiguous(uint64_t count) {
    if (count == 0)
        return PMM_INVALID_ADDRESS;
    uint64_t flags = interrupt_lock();
    if (count > free_pages) {
        interrupt_restore(flags);
        return PMM_INVALID_ADDRESS;
    }
    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t start = pass == 0 ? search_start : 0;
        uint64_t end = pass == 0 ? page_count : search_start;
        for (uint64_t page = start; page < end && count <= end - page; page++) {
            uint64_t offset = 0;
            while (offset < count && !bitmap_test(page + offset))
                offset++;
            if (offset != count) {
                page += offset;
                continue;
            }
            for (uint64_t i = 0; i < count; i++) {
                bitmap_set(page + i);
                page_references[page + i] = 1;
            }
            free_pages -= count;
            search_start = page + count;
            interrupt_restore(flags);
            return page * PMM_PAGE_SIZE;
        }
    }
    interrupt_restore(flags);
    return PMM_INVALID_ADDRESS;
}

int pmm_free_page(uint64_t physical_address) {
    if (physical_address % PMM_PAGE_SIZE != 0 ||
        physical_address == PMM_INVALID_ADDRESS ||
        !page_is_usable(physical_address))
        return 0;

    uint64_t metadata_end =
        references_physical + references_page_count * PMM_PAGE_SIZE;
    if (physical_address >= bitmap_physical && physical_address < metadata_end)
        return 0;

    uint64_t page = physical_address / PMM_PAGE_SIZE;
    uint64_t flags = interrupt_lock();
    if (!bitmap_test(page)) {
        interrupt_restore(flags);
        return 0;
    }

    if (page_references[page] > 1) {
        page_references[page]--;
        interrupt_restore(flags);
        return 1;
    }

    page_references[page] = 0;
    bitmap_clear(page);
    free_pages++;
    if (page < search_start)
        search_start = page;
    interrupt_restore(flags);
    return 1;
}

int pmm_retain_page(uint64_t physical_address) {
    if (physical_address % PMM_PAGE_SIZE != 0 ||
        physical_address == PMM_INVALID_ADDRESS ||
        !page_is_usable(physical_address))
        return 0;

    uint64_t page = physical_address / PMM_PAGE_SIZE;
    uint64_t flags = interrupt_lock();
    if (!bitmap_test(page) || page_references[page] == 0 ||
        page_references[page] == UINT16_MAX) {
        interrupt_restore(flags);
        return 0;
    }
    page_references[page]++;
    interrupt_restore(flags);
    return 1;
}

uint16_t pmm_page_references(uint64_t physical_address) {
    if (physical_address % PMM_PAGE_SIZE != 0 ||
        physical_address == PMM_INVALID_ADDRESS ||
        !page_is_usable(physical_address))
        return 0;

    uint64_t page = physical_address / PMM_PAGE_SIZE;
    uint64_t flags = interrupt_lock();
    uint16_t references = bitmap_test(page) ? page_references[page] : 0;
    interrupt_restore(flags);
    return references;
}

void *pmm_physical_to_virtual(uint64_t physical_address) {
    return (void *)(physical_address + hhdm_offset);
}

uint64_t pmm_total_pages(void) { return total_pages; }

uint64_t pmm_free_pages(void) {
    uint64_t flags = interrupt_lock();
    uint64_t result = free_pages;
    interrupt_restore(flags);
    return result;
}
