#include "kmalloc.h"
#include "../log.h"
#include <stdbool.h>
#include <stdint.h>

#define RAM_SHORTAGE 1
#ifdef RAM_SHORTAGE
#define HEAP_SIZE 4194304
#else
// more ram
#define HEAP_SIZE 16777216
#endif
static uint8_t heap_memory[HEAP_SIZE];

// label for each memory block
typedef struct block_meta {
    size_t size;  // block size (its obvious isnt it?)
    bool is_free; // do I really need to add comment here?
    struct block_meta
        *next; // its kindof lineked list so yk, ptr to next mem block
} block_meta_t;

// thats the start of the list, head or tail or whatever
static block_meta_t *free_list = NULL;

void kmalloc_init(void) {

    free_list = (block_meta_t *)heap_memory;

    // I just realized that idk what I am doing
    free_list->size = HEAP_SIZE - sizeof(block_meta_t);
    free_list->is_free = true;
    free_list->next = NULL;

    LOG_INFO("kmalloc initialized (4MB!!!)");
}

// its like my brother, just cuts from the block amount of the memory it whats
static void split_block(block_meta_t *block, size_t size) {
    // idk how to describe this so idk think for yourself
    if (block->size > size + sizeof(block_meta_t) + 16) {
        block_meta_t *new_block =
            (block_meta_t *)((uint8_t *)block + sizeof(block_meta_t) + size);
        new_block->size = block->size - size - sizeof(block_meta_t);
        new_block->is_free = true;
        new_block->next = block->next;

        block->size = size;
        block->next = new_block;
    }
}

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;

    // alaign to 8 bytes (some dude on reddit says its good for cpu performence x86)
    size = (size + 7) & ~7;

    block_meta_t *curr = free_list;
    while (curr != NULL) {
        // find first block that can fit ts
        if (curr->is_free && curr->size >= size) {
            split_block(curr, size);
            curr->is_free = false;

            
            return (void *)(curr + 1);
        }
        curr = curr->next;
    }

    // sorry blud no memory
    return NULL;
}

void kfree(void *ptr) {
    if (!ptr)
        return;


    block_meta_t *block = (block_meta_t *)ptr - 1;
    block->is_free = true;
    block_meta_t *curr = free_list;
    while (curr != NULL) {
        if (curr->is_free && curr->next && curr->next->is_free) {
                    curr->size += sizeof(block_meta_t) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}
