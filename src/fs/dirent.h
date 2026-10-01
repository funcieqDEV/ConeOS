#pragma once

#include <stdint.h>

#define FS_PATH_MAX 256
#define FS_DIRENT_NAME_MAX 768

enum fs_dirent_type {
    FS_DIRENT_FILE = 1,
    FS_DIRENT_DIRECTORY = 2,
};

struct fs_dirent {
    char name[FS_DIRENT_NAME_MAX];
    uint64_t size;
    uint8_t type;
};

struct fs_mount_info {
    char type[16];
    char path[FS_PATH_MAX];
};
