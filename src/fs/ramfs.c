#include "ramfs.h"
#include "../mm/kmalloc.h"
#include <stdint.h>

struct ramfs_file {
    int used;
    char name[RAMFS_NAME_SIZE];
    char *data;
    size_t size;
};

static struct ramfs_file files[RAMFS_MAX_FILES];
static char directories[RAMFS_MAX_DIRECTORIES][RAMFS_NAME_SIZE];
static size_t directory_count;

static int name_equal(const char *left, const char *right) {
    size_t i = 0;
    while (left[i] && right[i] && left[i] == right[i])
        i++;
    return left[i] == right[i];
}

static struct ramfs_file *find_file(const char *name) {
    for (size_t i = 0; i < RAMFS_MAX_FILES; i++)
        if (files[i].used && name_equal(files[i].name, name))
            return &files[i];
    return NULL;
}

void ramfs_init(void) {
    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        files[i].used = 0;
        files[i].data = NULL;
        files[i].size = 0;
    }
    directory_count = 0;
    ramfs_mkdir("/");
    ramfs_mkdir("/bin");
    ramfs_mkdir("/dev");
    ramfs_mkdir("/disk");
    ramfs_write("/hello.txt", "ConeOS ramfs\n", 13);
}

size_t ramfs_list(struct ramfs_file_info *output, size_t capacity) {
    size_t count = 0;
    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used)
            continue;
        if (output != NULL && count < capacity) {
            for (size_t j = 0; j < RAMFS_NAME_SIZE; j++)
                output[count].name[j] = files[i].name[j];
            output[count].size = files[i].size;
        }
        count++;
    }
    return count;
}

int ramfs_read(const char *name, char *buffer, size_t capacity, size_t *size) {
    struct ramfs_file *file = find_file(name);
    if (file == NULL || buffer == NULL || size == NULL)
        return 0;
    size_t count = file->size < capacity ? file->size : capacity;
    for (size_t i = 0; i < count; i++)
        buffer[i] = file->data[i];
    *size = count;
    return 1;
}

int ramfs_write(const char *name, const char *data, size_t size) {
    if (name == NULL || data == NULL || size > RAMFS_DATA_SIZE)
        return 0;
    struct ramfs_file *file = find_file(name);
    if (file == NULL) {
        for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
            if (!files[i].used) {
                file = &files[i];
                file->used = 1;
                size_t j = 0;
                while (name[j] && j < RAMFS_NAME_SIZE - 1) {
                    file->name[j] = name[j];
                    j++;
                }
                file->name[j] = '\0';
                file->data = kmalloc(RAMFS_DATA_SIZE);
                break;
            }
        }
    }
    if (file == NULL || file->data == NULL)
        return 0;
    for (size_t i = 0; i < size; i++)
        file->data[i] = data[i];
    file->size = size;
    return 1;
}

int ramfs_stat(const char *name, size_t *size) {
    struct ramfs_file *file = find_file(name);
    if (file == NULL || size == NULL)
        return 0;
    *size = file->size;
    return 1;
}

int ramfs_is_directory(const char *path) {
    if (path == NULL)
        return 0;
    for (size_t i = 0; i < directory_count; i++)
        if (name_equal(directories[i], path))
            return 1;
    return 0;
}

int ramfs_mkdir(const char *path) {
    if (path == NULL || !path[0] || directory_count >= RAMFS_MAX_DIRECTORIES)
        return 0;
    if (ramfs_is_directory(path))
        return 0;
    size_t length = 0;
    while (path[length] && length < RAMFS_NAME_SIZE)
        length++;
    if (length >= RAMFS_NAME_SIZE)
        return 0;
    for (size_t i = 0; i <= length; i++)
        directories[directory_count][i] = path[i];
    directory_count++;
    return 1;
}

int ramfs_unlink(const char *path) {
    struct ramfs_file *file = find_file(path);
    if (file == NULL)
        return 0;
    if (file->data != NULL)
        kfree(file->data);
    file->used = 0;
    file->data = NULL;
    file->size = 0;
    return 1;
}

int ramfs_rmdir(const char *path) {
    if (path == NULL || name_equal(path, "/") || name_equal(path, "/bin"))
        return 0;
    size_t index = 0;
    while (index < directory_count && !name_equal(directories[index], path))
        index++;
    if (index == directory_count)
        return 0;
    char prefix[RAMFS_NAME_SIZE];
    size_t length = 0;
    while (path[length] && length < RAMFS_NAME_SIZE - 1) {
        prefix[length] = path[length];
        length++;
    }
    if (length == 0 || length >= RAMFS_NAME_SIZE - 1)
        return 0;
    if (prefix[length - 1] != '/')
        prefix[length++] = '/';
    prefix[length] = '\0';
    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used)
            continue;
        size_t j = 0;
        while (prefix[j] && files[i].name[j] == prefix[j])
            j++;
        if (prefix[j] == '\0')
            return 0;
    }
    for (size_t i = 0; i < directory_count; i++) {
        if (i == index)
            continue;
        size_t j = 0;
        while (prefix[j] && directories[i][j] == prefix[j])
            j++;
        if (prefix[j] == '\0')
            return 0;
    }
    for (size_t i = index + 1; i < directory_count; i++)
        for (size_t j = 0; j < RAMFS_NAME_SIZE; j++)
            directories[i - 1][j] = directories[i][j];
    directory_count--;
    return 1;
}

size_t ramfs_list_directories(char output[][RAMFS_NAME_SIZE], size_t capacity) {
    size_t count = directory_count < capacity ? directory_count : capacity;
    for (size_t i = 0; i < count; i++)
        for (size_t j = 0; j < RAMFS_NAME_SIZE; j++)
            output[i][j] = directories[i][j];
    return directory_count;
}

static const char *ramfs_direct_child(const char *directory, const char *path) {
    size_t directory_length = 0;
    while (directory[directory_length])
        directory_length++;
    size_t start;
    if (directory_length == 1 && directory[0] == '/') {
        if (path[0] != '/' || path[1] == '\0')
            return NULL;
        start = 1;
    } else {
        size_t i = 0;
        while (i < directory_length && path[i] == directory[i])
            i++;
        if (i != directory_length || path[i] != '/' || path[i + 1] == '\0')
            return NULL;
        start = i + 1;
    }
    for (size_t i = start; path[i]; i++)
        if (path[i] == '/')
            return NULL;
    return &path[start];
}

static void copy_entry_name(struct fs_dirent *entry, const char *name) {
    size_t i = 0;
    while (name[i] && i < sizeof(entry->name) - 1) {
        entry->name[i] = name[i];
        i++;
    }
    entry->name[i] = '\0';
}

int ramfs_readdir(const char *path, size_t index, struct fs_dirent *entry) {
    if (!ramfs_is_directory(path) || !entry)
        return -1;
    size_t visible = 0;
    for (size_t i = 0; i < directory_count; i++) {
        const char *name = ramfs_direct_child(path, directories[i]);
        if (!name)
            continue;
        if (visible++ != index)
            continue;
        copy_entry_name(entry, name);
        entry->size = 0;
        entry->type = FS_DIRENT_DIRECTORY;
        return 1;
    }
    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used)
            continue;
        const char *name = ramfs_direct_child(path, files[i].name);
        if (!name)
            continue;
        if (visible++ != index)
            continue;
        copy_entry_name(entry, name);
        entry->size = files[i].size;
        entry->type = FS_DIRENT_FILE;
        return 1;
    }
    return 0;
}
