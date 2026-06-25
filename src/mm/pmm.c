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
static uint64_t bitmap_physical;
static uint64_t bitmap_page_count;
static uint64_t page_count;
static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t search_start;
static uint64_t hhdm_offset;
static struct limine_memmap_response *memory_map;

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

    int bitmap_region_found = 0;
    for (uint64_t i = 0; i < memory_map->entry_count; i++) {
        struct limine_memmap_entry *entry = memory_map->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t start = align_up(entry->base, PMM_PAGE_SIZE);
        uint64_t end = align_down(entry->base + entry->length, PMM_PAGE_SIZE);
        if (end - start >= bitmap_page_count * PMM_PAGE_SIZE) {
            bitmap_physical = start;
            bitmap_region_found = 1;
            break;
        }
    }

    if (!bitmap_region_found)
        return 0;

    bitmap = pmm_physical_to_virtual(bitmap_physical);
    for (uint64_t i = 0; i < bitmap_size; i++)
        bitmap[i] = 0xFF;

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
    for (uint64_t i = 0; i < bitmap_page_count; i++) {
        uint64_t page = bitmap_first_page + i;
        if (!bitmap_test(page)) {
            bitmap_set(page);
            free_pages--;
        }
    }

    search_start = bitmap_first_page + bitmap_page_count;
    LOG_INFO("physical memory manager initialized");
    return 1;
}

uint64_t pmm_alloc_page(void) {
    uint64_t flags = interrupt_lock();

    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t start = pass == 0 ? search_start : 0;
        uint64_t end = pass == 0 ? page_count : search_start;

        for (uint64_t page = start; page < end; page++) {
            if (bitmap_test(page))
                continue;

            bitmap_set(page);
            free_pages--;
            search_start = page + 1;
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

    uint64_t bitmap_end = bitmap_physical + bitmap_page_count * PMM_PAGE_SIZE;
    if (physical_address >= bitmap_physical && physical_address < bitmap_end)
        return 0;

    uint64_t page = physical_address / PMM_PAGE_SIZE;
    uint64_t flags = interrupt_lock();
    if (!bitmap_test(page)) {
        interrupt_restore(flags);
        return 0;
    }

    bitmap_clear(page);
    free_pages++;
    if (page < search_start)
        search_start = page;
    interrupt_restore(flags);
    return 1;
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
