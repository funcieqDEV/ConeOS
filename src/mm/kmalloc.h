#pragma once
#include <stddef.h>

struct kmalloc_stats {
    size_t mapped_bytes;
    size_t used_bytes;
    size_t free_bytes;
    size_t active_allocations;
};

int kmalloc_init(void);
void *kmalloc(size_t size);
int kfree(void *pointer);
void kmalloc_get_stats(struct kmalloc_stats *stats);
