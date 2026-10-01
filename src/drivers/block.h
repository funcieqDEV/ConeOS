#pragma once
#include <stddef.h>
#include <stdint.h>

int block_init(void);
int block_ready(void);
uint64_t block_sector_count(void);
int block_read_sector(uint64_t sector, void *buffer);
int block_write_sector(uint64_t sector, const void *buffer);
int block_flush(void);
