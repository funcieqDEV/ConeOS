#include "devfs.h"
#include "../drivers/ps2.h"
#include "../drivers/serial.h"
#include "../gfx/console.h"
#include "../kernel/task.h"

struct device_entry {
    const char *name;
    enum devfs_device device;
};

static const struct device_entry devices[] = {
    {"console", DEVFS_DEVICE_CONSOLE},
    {"null", DEVFS_DEVICE_NULL},
    {"zero", DEVFS_DEVICE_ZERO},
};

static int keyboard_input_waiting(void *argument) {
    (void)argument;
    return !keyboard_char_available();
}

static int path_is_root(const char *path) { return path && path[0] == '\0'; }

int devfs_device_for_path(const char *path) {
    if (!path)
        return DEVFS_DEVICE_NONE;
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        size_t j = 0;
        while (devices[i].name[j] && path[j] == devices[i].name[j])
            j++;
        if (!devices[i].name[j] && !path[j])
            return devices[i].device;
    }
    return DEVFS_DEVICE_NONE;
}

int devfs_read(enum devfs_device device, char *buffer, size_t length,
               size_t *read) {
    if (read)
        *read = 0;
    if (!buffer && length)
        return 0;
    if (device == DEVFS_DEVICE_NULL)
        return 1;
    if (device == DEVFS_DEVICE_ZERO) {
        for (size_t i = 0; i < length; i++)
            buffer[i] = 0;
        if (read)
            *read = length;
        return 1;
    }
    if (device != DEVFS_DEVICE_CONSOLE)
        return 0;

    size_t count = 0;
    while (count == 0) {
        char value;
        while (count < length && keyboard_read_char(&value))
            buffer[count++] = value;
        if (count == 0 && length)
            task_block_current_if(keyboard_input_waiting, NULL);
        if (length == 0)
            break;
    }
    if (read)
        *read = count;
    return 1;
}

int devfs_write(enum devfs_device device, const char *buffer, size_t length,
                size_t *written) {
    if (written)
        *written = 0;
    if (!buffer && length)
        return 0;
    if (device == DEVFS_DEVICE_CONSOLE) {
        for (size_t i = 0; i < length; i++) {
            print_serial_char(buffer[i]);
            console_putchar(buffer[i]);
        }
    } else if (device != DEVFS_DEVICE_NULL && device != DEVFS_DEVICE_ZERO) {
        return 0;
    }
    if (written)
        *written = length;
    return 1;
}

int devfs_stat(const char *path, size_t *size) {
    if (!size || devfs_device_for_path(path) == DEVFS_DEVICE_NONE)
        return 0;
    *size = 0;
    return 1;
}

int devfs_readdir(const char *path, size_t index, struct fs_dirent *entry) {
    if (!path_is_root(path) || !entry ||
        index >= sizeof(devices) / sizeof(devices[0]))
        return 0;
    const char *name = devices[index].name;
    size_t i = 0;
    while (name[i] && i < sizeof(entry->name) - 1) {
        entry->name[i] = name[i];
        i++;
    }
    entry->name[i] = '\0';
    entry->size = 0;
    entry->type = FS_DIRENT_FILE;
    return 1;
}

int devfs_is_directory(const char *path) { return path_is_root(path); }
int devfs_reject(const char *path) {
    (void)path;
    return 0;
}
int devfs_sync(void) { return 1; }
