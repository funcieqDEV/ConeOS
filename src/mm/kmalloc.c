#include "kmalloc.h"
#include "../log.h"
#include "pmm.h"
#include "vmm.h"
#include <stdint.h>

#define HEAP_BASE 0xFFFFD00000000000ULL
#define HEAP_MAX_SIZE (256ULL * 1024 * 1024)
#define HEAP_GROW_PAGES 4
#define BLOCK_MAGIC 0xC04ECAFE
#define BLOCK_FREE 0
#define BLOCK_USED 1
#define ALIGNMENT 16

typedef struct block_header {
    size_t size;
    struct block_header *previous;
    struct block_header *next;
    uint32_t magic;
    uint32_t state;
} block_header_t;

_Static_assert(sizeof(block_header_t) % ALIGNMENT == 0,
               "heap header must preserve payload alignment");

static block_header_t *first_block;
static uint64_t mapped_size;
static size_t active_allocations;

static uint64_t interrupt_lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void interrupt_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

static size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static block_header_t *last_block(void) {
    block_header_t *block = first_block;
    if (block == NULL)
        return NULL;
    while (block->next != NULL)
        block = block->next;
    return block;
}

static int grow_heap(size_t minimum_payload) {
    size_t required = minimum_payload + sizeof(block_header_t);
    size_t minimum_growth = (size_t)HEAP_GROW_PAGES * (size_t)PMM_PAGE_SIZE;
    size_t growth = align_up(required, PMM_PAGE_SIZE);
    if (growth < minimum_growth)
        growth = minimum_growth;
    if (growth > HEAP_MAX_SIZE - mapped_size)
        return 0;

    uint64_t old_end = HEAP_BASE + mapped_size;
    size_t mapped_pages = 0;
    size_t page_count = growth / PMM_PAGE_SIZE;

    for (size_t page = 0; page < page_count; page++) {
        uint64_t physical = pmm_alloc_page();
        uint64_t virtual = old_end + page * PMM_PAGE_SIZE;
        if (physical == PMM_INVALID_ADDRESS ||
            !vmm_map_page(virtual, physical, VMM_WRITABLE)) {
            if (physical != PMM_INVALID_ADDRESS)
                pmm_free_page(physical);
            while (mapped_pages > 0) {
                mapped_pages--;
                uint64_t rollback_virtual =
                    old_end + mapped_pages * PMM_PAGE_SIZE;
                uint64_t rollback_physical = vmm_unmap_page(rollback_virtual);
                if (rollback_physical != PMM_INVALID_ADDRESS)
                    pmm_free_page(rollback_physical);
            }
            return 0;
        }
        mapped_pages++;
    }

    block_header_t *last = last_block();
    if (last != NULL && last->state == BLOCK_FREE) {
        last->size += growth;
    } else {
        block_header_t *new_block = (block_header_t *)old_end;
        new_block->size = growth - sizeof(block_header_t);
        new_block->previous = last;
        new_block->next = NULL;
        new_block->magic = BLOCK_MAGIC;
        new_block->state = BLOCK_FREE;
        if (last != NULL)
            last->next = new_block;
        else
            first_block = new_block;
    }

    mapped_size += growth;
    return 1;
}

static void split_block(block_header_t *block, size_t size) {
    if (block->size < size + sizeof(block_header_t) + ALIGNMENT)
        return;

    block_header_t *remainder =
        (block_header_t *)((uint8_t *)(block + 1) + size);
    remainder->size = block->size - size - sizeof(block_header_t);
    remainder->previous = block;
    remainder->next = block->next;
    remainder->magic = BLOCK_MAGIC;
    remainder->state = BLOCK_FREE;
    if (remainder->next != NULL)
        remainder->next->previous = remainder;

    block->size = size;
    block->next = remainder;
}

static void merge_with_next(block_header_t *block) {
    block_header_t *next = block->next;
    if (next == NULL || next->state != BLOCK_FREE)
        return;

    block->size += sizeof(block_header_t) + next->size;
    block->next = next->next;
    if (block->next != NULL)
        block->next->previous = block;
}

static block_header_t *find_block_for_pointer(void *pointer) {
    for (block_header_t *block = first_block; block != NULL;
         block = block->next) {
        if (block->magic != BLOCK_MAGIC)
            return NULL;
        if ((void *)(block + 1) == pointer)
            return block;
    }
    return NULL;
}

int kmalloc_init(void) {
    if (!grow_heap(ALIGNMENT))
        return 0;
    LOG_INFO("kernel heap initialized");
    return 1;
}

void *kmalloc(size_t size) {
    if (size == 0 || size > HEAP_MAX_SIZE - sizeof(block_header_t))
        return NULL;

    size = align_up(size, ALIGNMENT);
    uint64_t flags = interrupt_lock();

    for (;;) {
        for (block_header_t *block = first_block; block != NULL;
             block = block->next) {
            if (block->magic == BLOCK_MAGIC && block->state == BLOCK_FREE &&
                block->size >= size) {
                split_block(block, size);
                block->state = BLOCK_USED;
                active_allocations++;
                interrupt_restore(flags);
                return block + 1;
            }
        }

        if (!grow_heap(size)) {
            interrupt_restore(flags);
            return NULL;
        }
    }
}

int kfree(void *pointer) {
    if (pointer == NULL)
        return 0;

    uint64_t address = (uint64_t)pointer;
    if (address < HEAP_BASE + sizeof(block_header_t) ||
        address >= HEAP_BASE + mapped_size || address % ALIGNMENT != 0)
        return 0;

    uint64_t flags = interrupt_lock();
    block_header_t *block = find_block_for_pointer(pointer);
    if (block == NULL || block->state != BLOCK_USED) {
        interrupt_restore(flags);
        return 0;
    }

    block->state = BLOCK_FREE;
    active_allocations--;
    merge_with_next(block);
    if (block->previous != NULL && block->previous->state == BLOCK_FREE) {
        block = block->previous;
        merge_with_next(block);
    }

    interrupt_restore(flags);
    return 1;
}

void kmalloc_get_stats(struct kmalloc_stats *stats) {
    if (stats == NULL)
        return;

    uint64_t flags = interrupt_lock();
    stats->mapped_bytes = mapped_size;
    stats->used_bytes = 0;
    stats->free_bytes = 0;
    stats->active_allocations = active_allocations;

    for (block_header_t *block = first_block; block != NULL;
         block = block->next) {
        if (block->state == BLOCK_USED)
            stats->used_bytes += block->size;
        else
            stats->free_bytes += block->size;
    }
    interrupt_restore(flags);
}
