#pragma once

#include <stddef.h>
#include "dirent.h"

enum devfs_device {
    DEVFS_DEVICE_NONE,
    DEVFS_DEVICE_CONSOLE,
    DEVFS_DEVICE_NULL,
    DEVFS_DEVICE_ZERO,
};

int devfs_read(enum devfs_device device, char *buffer, size_t length,
               size_t *read);
int devfs_write(enum devfs_device device, const char *buffer, size_t length,
                size_t *written);
int devfs_stat(const char *path, size_t *size);
int devfs_readdir(const char *path, size_t index, struct fs_dirent *entry);
int devfs_is_directory(const char *path);
int devfs_device_for_path(const char *path);
int devfs_reject(const char *path);
int devfs_sync(void);
