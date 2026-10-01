#pragma once

#include <stddef.h>
#include <stdint.h>

int copy_from_user(void *destination, uint64_t source, size_t size);
int copy_to_user(uint64_t destination, const void *source, size_t size);
int user_range_readable(uint64_t address, size_t size);
int user_range_writable(uint64_t address, size_t size);
