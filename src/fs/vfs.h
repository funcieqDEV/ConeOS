#pragma once

#include <stddef.h>
#include <stdint.h>
#include "dirent.h"

void vfs_init(void);
int vfs_open(const char *name, int create);
int vfs_close(int fd);
int vfs_read(int fd, void *buffer, size_t length, size_t *read);
int vfs_write(int fd, const void *buffer, size_t length, size_t *written);
void vfs_close_all(uint64_t owner);
int vfs_inherit_standard(uint64_t parent, uint64_t child);
int vfs_fork(uint64_t parent, uint64_t child);
int64_t vfs_seek(int fd, int64_t offset, int whence);
int vfs_stat(const char *name, size_t *size);
int vfs_readdir(const char *path, size_t index, struct fs_dirent *entry);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_is_directory(const char *path);
int vfs_sync(void);
int vfs_mount(const char *type, const char *target);
int vfs_unmount(const char *target);
int vfs_mount_info(size_t index, struct fs_mount_info *info);
int vfs_pipe(int descriptors[2]);
int vfs_dup(int descriptor);
int vfs_dup2(int source, int destination);
