#include "heap.h"
#include "syscall.h"
#include <stdint.h>

#define HEAP_ALIGNMENT 16ULL
#define HEAP_GROWTH 4096ULL

struct heap_block {
    size_t size;
    struct heap_block *next;
    int free;
};

#define HEAP_HEADER_SIZE \
    ((sizeof(struct heap_block) + HEAP_ALIGNMENT - 1) & ~(HEAP_ALIGNMENT - 1))

static struct heap_block *first_block;
static struct heap_block *last_block;

static size_t align_size(size_t size) {
    return (size + HEAP_ALIGNMENT - 1) & ~(HEAP_ALIGNMENT - 1);
}

static void merge_free_blocks(void) {
    struct heap_block *block = first_block;
    last_block = NULL;
    while (block) {
        struct heap_block *next = block->next;
        if (next && block->free && next->free &&
            (uint8_t *)block + HEAP_HEADER_SIZE + block->size ==
                (uint8_t *)next) {
            block->size += HEAP_HEADER_SIZE + next->size;
            block->next = next->next;
            continue;
        }
        last_block = block;
        block = next;
    }
}

static int split_block(struct heap_block *block, size_t size) {
    if (size > block->size ||
        block->size - size < HEAP_HEADER_SIZE + HEAP_ALIGNMENT) return 0;
    struct heap_block *remainder = (void *)((uint8_t *)block + HEAP_HEADER_SIZE + size);
    remainder->size = block->size - size - HEAP_HEADER_SIZE;
    remainder->next = block->next;
    remainder->free = 1;
    block->next = remainder;
    block->size = size;
    if (last_block == block) last_block = remainder;
    return 1;
}

static struct heap_block *grow_heap(size_t size) {
    uint64_t current = (uint64_t)sys_brk(0);
    if (!current) return NULL;
    if (last_block && last_block->free &&
        (uint8_t *)last_block + HEAP_HEADER_SIZE + last_block->size ==
            (uint8_t *)current) {
        size_t missing = size > last_block->size ? size - last_block->size : 0;
        size_t growth = missing > HEAP_GROWTH ? missing : HEAP_GROWTH;
        growth = align_size(growth);
        if ((uint64_t)sys_brk(current + growth) != current + growth) return NULL;
        last_block->size += growth;
        return last_block;
    }

    size_t growth = size + HEAP_HEADER_SIZE;
    if (growth < HEAP_GROWTH) growth = HEAP_GROWTH;
    growth = align_size(growth);
    if ((uint64_t)sys_brk(current + growth) != current + growth) return NULL;
    struct heap_block *block = (void *)current;
    block->size = growth - HEAP_HEADER_SIZE;
    block->next = NULL;
    block->free = 1;
    if (last_block) last_block->next = block;
    else first_block = block;
    last_block = block;
    return block;
}

void *malloc(size_t size) {
    if (size == 0) return NULL;
    if (size > SIZE_MAX - HEAP_HEADER_SIZE - (HEAP_ALIGNMENT - 1))
        return NULL;
    size = align_size(size);

    for (;;) {
        for (struct heap_block *block = first_block; block; block = block->next) {
            if (!block->free || block->size < size) continue;
            (void)split_block(block, size);
            block->free = 0;
            return (uint8_t *)block + HEAP_HEADER_SIZE;
        }
        struct heap_block *block = grow_heap(size);
        if (!block) return NULL;
    }
}

void free(void *pointer) {
    if (!pointer) return;
    struct heap_block *block = (void *)((uint8_t *)pointer - HEAP_HEADER_SIZE);
    block->free = 1;
    merge_free_blocks();
}

void *calloc(size_t count, size_t size) {
    if (count && size > SIZE_MAX / count) return NULL;
    size_t total = count * size;
    uint8_t *memory = malloc(total);
    if (!memory) return NULL;
    for (size_t i = 0; i < total; i++) memory[i] = 0;
    return memory;
}
