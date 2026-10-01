#pragma once

#include "dirent.h"
#include <stddef.h>

#define RAMFS_MAX_FILES 64
#define RAMFS_MAX_DIRECTORIES 16
#define RAMFS_NAME_SIZE FS_PATH_MAX
#define RAMFS_DATA_SIZE 32768

struct ramfs_file_info {
    char name[RAMFS_NAME_SIZE];
    size_t size;
};

void ramfs_init(void);
size_t ramfs_list(struct ramfs_file_info *files, size_t capacity);
int ramfs_read(const char *name, char *buffer, size_t capacity, size_t *size);
int ramfs_write(const char *name, const char *data, size_t size);
int ramfs_stat(const char *name, size_t *size);
int ramfs_mkdir(const char *path);
int ramfs_is_directory(const char *path);
int ramfs_unlink(const char *path);
int ramfs_rmdir(const char *path);
size_t ramfs_list_directories(char directories[][RAMFS_NAME_SIZE],
                              size_t capacity);
int ramfs_readdir(const char *path, size_t index, struct fs_dirent *entry);
