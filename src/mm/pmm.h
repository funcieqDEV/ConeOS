#pragma once

#include <stdint.h>

#define PMM_PAGE_SIZE 4096
#define PMM_INVALID_ADDRESS UINT64_MAX

int pmm_init(void);
uint64_t pmm_alloc_page(void);
int pmm_free_page(uint64_t physical_address);
void *pmm_physical_to_virtual(uint64_t physical_address);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);
