#pragma once
#include <stddef.h>
#include <stdint.h>
#include "dirent.h"

int fat32_mount(void);
int fat32_mounted(void);
int fat32_readdir(const char *path, size_t index, struct fs_dirent *entry);
int fat32_make_directory(const char *path);
int fat32_remove_directory(const char *path);
int fat32_is_directory(const char *path);
int fat32_read_file(const char *path, char *buffer, size_t capacity, size_t *size);
int fat32_stat_file(const char *path, size_t *size);
int fat32_write_file(const char *path, const char *buffer, size_t size);
int fat32_create_file(const char *path);
int fat32_unlink_file(const char *path);
int fat32_sync(void);
