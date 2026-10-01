#include "vfs.h"
#include "../kernel/process.h"
#include "../kernel/task.h"
#include "devfs.h"
#include "fat32.h"
#include "ramfs.h"

#define VFS_MAX_FDS 32
#define VFS_MAX_PROCESSES 16
#define VFS_DESCRIPTOR_SLOTS (VFS_MAX_FDS * VFS_MAX_PROCESSES)
#define VFS_MAX_MOUNTS 8
#define VFS_MAX_PIPES 16
#define VFS_PIPE_CAPACITY 4096

enum vfs_pipe_mode { VFS_PIPE_NONE, VFS_PIPE_READ, VFS_PIPE_WRITE };

struct vfs_open_file {
    int used;
    size_t references;
    size_t offset;
};

struct vfs_pipe {
    int used;
    size_t readers;
    size_t writers;
    size_t head;
    size_t tail;
    size_t count;
    char data[VFS_PIPE_CAPACITY];
};

struct vfs_fs_ops {
    int (*read)(const char *, char *, size_t, size_t *);
    int (*write)(const char *, const char *, size_t);
    int (*stat)(const char *, size_t *);
    int (*create)(const char *);
    int (*readdir)(const char *, size_t, struct fs_dirent *);
    int (*mkdir)(const char *);
    int (*unlink)(const char *);
    int (*rmdir)(const char *);
    int (*is_directory)(const char *);
    int (*sync)(void);
    int (*device)(const char *);
};

struct vfs_mount {
    int used;
    int removable;
    char path[FS_PATH_MAX];
    char type[16];
    const struct vfs_fs_ops *ops;
};

struct vfs_fd {
    int used;
    uint64_t owner;
    int number;
    char name[RAMFS_NAME_SIZE];
    struct vfs_open_file *file;
    enum devfs_device device;
    struct vfs_pipe *pipe;
    enum vfs_pipe_mode pipe_mode;
};

static struct vfs_fd descriptors[VFS_DESCRIPTOR_SLOTS];
static struct vfs_open_file open_files[VFS_DESCRIPTOR_SLOTS];
static struct vfs_mount mounts[VFS_MAX_MOUNTS];
static struct vfs_pipe pipes[VFS_MAX_PIPES];

static uint64_t vfs_interrupt_lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void vfs_interrupt_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

static void pipe_drop_reference_locked(struct vfs_pipe *pipe,
                                       enum vfs_pipe_mode mode) {
    if (!pipe || !pipe->used)
        return;
    if (mode == VFS_PIPE_READ && pipe->readers)
        pipe->readers--;
    if (mode == VFS_PIPE_WRITE && pipe->writers)
        pipe->writers--;
    if (pipe->readers == 0 && pipe->writers == 0) {
        pipe->used = 0;
        pipe->head = pipe->tail = pipe->count = 0;
    }
}

static void pipe_retain_reference(struct vfs_fd *fd) {
    if (fd->pipe_mode == VFS_PIPE_NONE || !fd->pipe)
        return;
    uint64_t flags = vfs_interrupt_lock();
    if (fd->pipe->used) {
        if (fd->pipe_mode == VFS_PIPE_READ)
            fd->pipe->readers++;
        else
            fd->pipe->writers++;
    }
    vfs_interrupt_restore(flags);
}

static struct vfs_open_file *allocate_open_file_locked(void) {
    for (size_t i = 0; i < VFS_DESCRIPTOR_SLOTS; i++) {
        if (open_files[i].used)
            continue;
        open_files[i].used = 1;
        open_files[i].references = 1;
        open_files[i].offset = 0;
        return &open_files[i];
    }
    return NULL;
}

static void open_file_retain_locked(struct vfs_open_file *file) {
    if (file)
        file->references++;
}

static void open_file_release_locked(struct vfs_open_file *file) {
    if (file && file->references && --file->references == 0) {
        file->used = 0;
        file->offset = 0;
    }
}

static void open_file_retain(struct vfs_open_file *file) {
    uint64_t flags = vfs_interrupt_lock();
    open_file_retain_locked(file);
    vfs_interrupt_restore(flags);
}

static int pipe_reader_should_wait(void *argument) {
    struct vfs_pipe *pipe = argument;
    return pipe->used && pipe->count == 0 && pipe->writers != 0;
}

static int pipe_writer_should_wait(void *argument) {
    struct vfs_pipe *pipe = argument;
    return pipe->used && pipe->count == VFS_PIPE_CAPACITY && pipe->readers != 0;
}

static int pipe_read(struct vfs_fd *fd, char *buffer, size_t length,
                     size_t *read) {
    struct vfs_pipe *pipe = fd->pipe;
    if (!pipe || fd->pipe_mode != VFS_PIPE_READ)
        return -1;
    if (length == 0)
        return 0;
    for (;;) {
        uint64_t flags = vfs_interrupt_lock();
        if (pipe->count != 0) {
            size_t count = pipe->count < length ? pipe->count : length;
            for (size_t i = 0; i < count; i++) {
                buffer[i] = pipe->data[pipe->head];
                pipe->head = (pipe->head + 1) % VFS_PIPE_CAPACITY;
            }
            pipe->count -= count;
            vfs_interrupt_restore(flags);
            if (read)
                *read = count;
            task_wake_blocked();
            return 0;
        }
        if (pipe->writers == 0) {
            vfs_interrupt_restore(flags);
            return 0;
        }
        vfs_interrupt_restore(flags);
        task_block_current_if(pipe_reader_should_wait, pipe);
    }
}

static int pipe_write(struct vfs_fd *fd, const char *buffer, size_t length,
                      size_t *written) {
    struct vfs_pipe *pipe = fd->pipe;
    if (!pipe || fd->pipe_mode != VFS_PIPE_WRITE)
        return -1;
    size_t total = 0;
    while (total < length) {
        uint64_t flags = vfs_interrupt_lock();
        if (pipe->readers == 0) {
            vfs_interrupt_restore(flags);
            if (written)
                *written = total;
            return total ? 0 : -1;
        }
        size_t available = VFS_PIPE_CAPACITY - pipe->count;
        size_t amount = length - total;
        if (amount > available)
            amount = available;
        for (size_t i = 0; i < amount; i++) {
            pipe->data[pipe->tail] = buffer[total + i];
            pipe->tail = (pipe->tail + 1) % VFS_PIPE_CAPACITY;
        }
        pipe->count += amount;
        vfs_interrupt_restore(flags);
        if (amount) {
            total += amount;
            task_wake_blocked();
        } else {
            task_block_current_if(pipe_writer_should_wait, pipe);
        }
    }
    if (written)
        *written = total;
    return 0;
}

static int ramfs_create(const char *path) { return ramfs_write(path, "", 0); }
static int ramfs_sync(void) { return 1; }
static int fat32_create(const char *path) { return fat32_create_file(path); }

static const struct vfs_fs_ops ramfs_ops = {
    .read = ramfs_read,
    .write = ramfs_write,
    .stat = ramfs_stat,
    .create = ramfs_create,
    .readdir = ramfs_readdir,
    .mkdir = ramfs_mkdir,
    .unlink = ramfs_unlink,
    .rmdir = ramfs_rmdir,
    .is_directory = ramfs_is_directory,
    .sync = ramfs_sync,
};
static const struct vfs_fs_ops fat32_ops = {
    .read = fat32_read_file,
    .write = fat32_write_file,
    .stat = fat32_stat_file,
    .create = fat32_create,
    .readdir = fat32_readdir,
    .mkdir = fat32_make_directory,
    .unlink = fat32_unlink_file,
    .rmdir = fat32_remove_directory,
    .is_directory = fat32_is_directory,
    .sync = fat32_sync,
};
static const struct vfs_fs_ops devfs_ops = {
    .stat = devfs_stat,
    .create = devfs_reject,
    .readdir = devfs_readdir,
    .mkdir = devfs_reject,
    .unlink = devfs_reject,
    .rmdir = devfs_reject,
    .is_directory = devfs_is_directory,
    .sync = devfs_sync,
    .device = devfs_device_for_path,
};

static struct vfs_fd *lookup(uint64_t owner, int number) {
    for (size_t i = 0; i < VFS_DESCRIPTOR_SLOTS; i++)
        if (descriptors[i].used && descriptors[i].owner == owner &&
            descriptors[i].number == number)
            return &descriptors[i];
    return NULL;
}

static struct vfs_fd *allocate_slot(void) {
    for (size_t i = 0; i < VFS_DESCRIPTOR_SLOTS; i++)
        if (!descriptors[i].used)
            return &descriptors[i];
    return NULL;
}

static void ensure_standard_streams(uint64_t owner) {
    static const char *names[] = {"stdin", "stdout", "stderr"};
    for (int number = 0; number < 3; number++) {
        if (lookup(owner, number) != NULL)
            continue;
        struct vfs_fd *fd = allocate_slot();
        if (fd == NULL)
            return;
        fd->used = 1;
        fd->owner = owner;
        fd->number = number;
        fd->device = DEVFS_DEVICE_CONSOLE;
        fd->file = NULL;
        fd->pipe = NULL;
        fd->pipe_mode = VFS_PIPE_NONE;
        size_t i = 0;
        while (names[number][i]) {
            fd->name[i] = names[number][i];
            i++;
        }
        fd->name[i] = '\0';
    }
}

static int names_equal(const char *left, const char *right) {
    size_t i = 0;
    while (left[i] && right[i] && left[i] == right[i])
        i++;
    return left[i] == right[i];
}

static int path_has_prefix(const char *path, const char *prefix) {
    size_t i = 0;
    while (prefix[i] && path[i] == prefix[i])
        i++;
    return prefix[i] == '\0' && (path[i] == '\0' || path[i] == '/');
}

static const struct vfs_mount *find_mount(const char *path,
                                          const char **relative) {
    const struct vfs_mount *best = NULL;
    size_t best_length = 0;
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].used || !path_has_prefix(path, mounts[i].path))
            continue;
        size_t length = 0;
        while (mounts[i].path[length])
            length++;
        if (length >= best_length) {
            best = &mounts[i];
            best_length = length;
        }
    }
    if (!best)
        return NULL;
    const char *tail = path + best_length;
    if (*tail == '/')
        tail++;
    *relative = tail;
    return best;
}

static const struct vfs_fs_ops *path_ops(const char *path,
                                         const char **backend_path) {
    const struct vfs_mount *mount = find_mount(path, backend_path);
    if (mount)
        return mount->ops;
    *backend_path = path;
    return &ramfs_ops;
}

static int install_mount(const char *path, const char *type,
                         const struct vfs_fs_ops *ops, int removable) {
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && names_equal(mounts[i].path, path))
            return 0;
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].used)
            continue;
        size_t j = 0;
        while (path[j]) {
            mounts[i].path[j] = path[j];
            j++;
        }
        mounts[i].path[j] = '\0';
        j = 0;
        while (type[j]) {
            mounts[i].type[j] = type[j];
            j++;
        }
        mounts[i].type[j] = '\0';
        mounts[i].ops = ops;
        mounts[i].removable = removable;
        mounts[i].used = 1;
        return 1;
    }
    return 0;
}

static int resolve_path(const char *input, char *output, size_t capacity) {
    if (input == NULL || output == NULL || capacity < 2)
        return 0;
    size_t out = 0, in = 0;
    if (input[0] == '/') {
        output[out++] = '/';
        while (input[in] == '/')
            in++;
    } else {
        char cwd[FS_PATH_MAX];
        if (!process_getcwd(cwd, sizeof(cwd)))
            return 0;
        while (cwd[out] && out < capacity - 1)
            output[out] = cwd[out], out++;
    }
    while (input[in]) {
        size_t start = in;
        while (input[in] && input[in] != '/')
            in++;
        size_t length = in - start;
        while (input[in] == '/')
            in++;
        if (length == 0 || (length == 1 && input[start] == '.'))
            continue;
        if (length == 2 && input[start] == '.' && input[start + 1] == '.') {
            if (out > 1) {
                if (output[out - 1] == '/')
                    out--;
                while (out > 1 && output[out - 1] != '/')
                    out--;
            }
            continue;
        }
        if (out > 1 && output[out - 1] != '/') {
            if (out >= capacity - 1)
                return 0;
            output[out++] = '/';
        }
        if (out + length >= capacity)
            return 0;
        for (size_t i = 0; i < length; i++)
            output[out++] = input[start + i];
    }
    if (out == 0)
        output[out++] = '/';
    output[out] = '\0';
    return 1;
}

void vfs_init(void) {
    for (size_t i = 0; i < VFS_DESCRIPTOR_SLOTS; i++)
        descriptors[i].used = 0;
    for (size_t i = 0; i < VFS_DESCRIPTOR_SLOTS; i++)
        open_files[i].used = 0;
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++)
        mounts[i].used = 0;
    for (size_t i = 0; i < VFS_MAX_PIPES; i++)
        pipes[i].used = 0;
    (void)install_mount("/dev", "devfs", &devfs_ops, 0);
    ensure_standard_streams(0);
}

int vfs_open(const char *name, int create) {
    if (name == NULL || !name[0])
        return -1;
    size_t name_length = 0;
    while (name[name_length] && name_length < RAMFS_NAME_SIZE)
        name_length++;
    if (name_length >= RAMFS_NAME_SIZE)
        return -1;

    char absolute[RAMFS_NAME_SIZE];
    if (!resolve_path(name, absolute, sizeof(absolute)))
        return -1;
    name = absolute;
    const char *backend;
    const struct vfs_fs_ops *ops = path_ops(name, &backend);
    size_t file_size;
    if (!ops->stat(backend, &file_size) && (!create || !ops->create(backend)))
        return -1;

    uint64_t owner = task_current_id();
    ensure_standard_streams(owner);
    uint64_t flags = vfs_interrupt_lock();
    int number;
    for (number = 3; number < VFS_MAX_FDS; number++)
        if (lookup(owner, number) == NULL)
            break;
    if (number == VFS_MAX_FDS) {
        vfs_interrupt_restore(flags);
        return -1;
    }
    struct vfs_fd *fd = allocate_slot();
    struct vfs_open_file *file = allocate_open_file_locked();
    if (file == NULL || fd == NULL) {
        if (file)
            open_file_release_locked(file);
        vfs_interrupt_restore(flags);
        return -1;
    }
    fd->used = 1;
    fd->owner = owner;
    fd->number = number;
    fd->file = file;
    fd->device = ops->device ? (enum devfs_device)ops->device(backend)
                             : DEVFS_DEVICE_NONE;
    fd->pipe = NULL;
    fd->pipe_mode = VFS_PIPE_NONE;
    size_t i = 0;
    while (name[i]) {
        fd->name[i] = name[i];
        i++;
    }
    fd->name[i] = '\0';
    vfs_interrupt_restore(flags);
    return number;
}

int vfs_pipe(int pipe_descriptors[2]) {
    if (!pipe_descriptors)
        return -1;
    uint64_t owner = task_current_id();
    ensure_standard_streams(owner);
    uint64_t flags = vfs_interrupt_lock();
    struct vfs_pipe *pipe = NULL;
    struct vfs_fd *slots[2] = {NULL, NULL};
    int numbers[2] = {-1, -1};
    for (size_t i = 0; i < VFS_MAX_PIPES; i++)
        if (!pipes[i].used) {
            pipe = &pipes[i];
            break;
        }
    for (int number = 3; number < VFS_MAX_FDS && numbers[1] < 0; number++) {
        if (lookup(owner, number))
            continue;
        if (numbers[0] < 0)
            numbers[0] = number;
        else
            numbers[1] = number;
    }
    for (size_t i = 0; i < VFS_DESCRIPTOR_SLOTS && !slots[1]; i++) {
        if (descriptors[i].used)
            continue;
        if (!slots[0])
            slots[0] = &descriptors[i];
        else
            slots[1] = &descriptors[i];
    }
    if (!pipe || numbers[1] < 0 || !slots[1]) {
        vfs_interrupt_restore(flags);
        return -1;
    }
    pipe->used = 1;
    pipe->readers = 1;
    pipe->writers = 1;
    pipe->head = pipe->tail = pipe->count = 0;
    for (size_t i = 0; i < 2; i++) {
        struct vfs_fd *fd = slots[i];
        fd->used = 1;
        fd->owner = owner;
        fd->number = numbers[i];
        fd->file = NULL;
        fd->device = DEVFS_DEVICE_NONE;
        fd->pipe = pipe;
        fd->pipe_mode = i == 0 ? VFS_PIPE_READ : VFS_PIPE_WRITE;
        static const char name[] = "[pipe]";
        size_t j = 0;
        while (name[j]) {
            fd->name[j] = name[j];
            j++;
        }
        fd->name[j] = '\0';
    }
    pipe_descriptors[0] = numbers[0];
    pipe_descriptors[1] = numbers[1];
    vfs_interrupt_restore(flags);
    return 0;
}

int vfs_dup(int number) {
    uint64_t owner = task_current_id();
    ensure_standard_streams(owner);
    uint64_t flags = vfs_interrupt_lock();
    struct vfs_fd *source = lookup(owner, number);
    if (!source) {
        vfs_interrupt_restore(flags);
        return -1;
    }
    int destination = -1;
    for (int candidate = 0; candidate < VFS_MAX_FDS; candidate++)
        if (!lookup(owner, candidate)) {
            destination = candidate;
            break;
        }
    struct vfs_fd *target = allocate_slot();
    if (destination < 0 || !target) {
        vfs_interrupt_restore(flags);
        return -1;
    }
    *target = *source;
    target->owner = owner;
    target->number = destination;
    target->used = 1;
    open_file_retain_locked(target->file);
    if (target->pipe_mode == VFS_PIPE_READ)
        target->pipe->readers++;
    if (target->pipe_mode == VFS_PIPE_WRITE)
        target->pipe->writers++;
    vfs_interrupt_restore(flags);
    return destination;
}

int vfs_dup2(int source_number, int destination_number) {
    if (destination_number < 0 || destination_number >= VFS_MAX_FDS)
        return -1;
    uint64_t owner = task_current_id();
    ensure_standard_streams(owner);
    uint64_t flags = vfs_interrupt_lock();
    struct vfs_fd *source = lookup(owner, source_number);
    if (!source) {
        vfs_interrupt_restore(flags);
        return -1;
    }
    if (source_number == destination_number) {
        vfs_interrupt_restore(flags);
        return destination_number;
    }
    struct vfs_fd saved = *source;
    struct vfs_fd *target = lookup(owner, destination_number);
    if (!target)
        target = allocate_slot();
    if (!target) {
        vfs_interrupt_restore(flags);
        return -1;
    }
    if (target->used) {
        pipe_drop_reference_locked(target->pipe, target->pipe_mode);
        open_file_release_locked(target->file);
    }
    *target = saved;
    target->owner = owner;
    target->number = destination_number;
    target->used = 1;
    open_file_retain_locked(target->file);
    if (target->pipe_mode == VFS_PIPE_READ)
        target->pipe->readers++;
    if (target->pipe_mode == VFS_PIPE_WRITE)
        target->pipe->writers++;
    vfs_interrupt_restore(flags);
    task_wake_blocked();
    return destination_number;
}

int vfs_close(int number) {
    uint64_t flags = vfs_interrupt_lock();
    struct vfs_fd *fd = lookup(task_current_id(), number);
    if (fd == NULL || number < 3) {
        vfs_interrupt_restore(flags);
        return -1;
    }
    int pipe_endpoint = fd->pipe_mode != VFS_PIPE_NONE;
    pipe_drop_reference_locked(fd->pipe, fd->pipe_mode);
    open_file_release_locked(fd->file);
    fd->used = 0;
    fd->file = NULL;
    fd->pipe = NULL;
    fd->pipe_mode = VFS_PIPE_NONE;
    vfs_interrupt_restore(flags);
    if (pipe_endpoint)
        task_wake_blocked();
    return 0;
}

void vfs_close_all(uint64_t owner) {
    int closed_pipe = 0;
    uint64_t flags = vfs_interrupt_lock();
    for (size_t i = 0; i < VFS_DESCRIPTOR_SLOTS; i++)
        if (descriptors[i].used && descriptors[i].owner == owner) {
            if (descriptors[i].pipe_mode != VFS_PIPE_NONE)
                closed_pipe = 1;
            pipe_drop_reference_locked(descriptors[i].pipe,
                                       descriptors[i].pipe_mode);
            open_file_release_locked(descriptors[i].file);
            descriptors[i].used = 0;
            descriptors[i].file = NULL;
            descriptors[i].pipe = NULL;
            descriptors[i].pipe_mode = VFS_PIPE_NONE;
        }
    vfs_interrupt_restore(flags);
    if (closed_pipe)
        task_wake_blocked();
}

int vfs_inherit_standard(uint64_t parent, uint64_t child) {
    ensure_standard_streams(parent);
    vfs_close_all(child);
    for (int number = 0; number < 3; number++) {
        struct vfs_fd *source = lookup(parent, number);
        struct vfs_fd *target = allocate_slot();
        if (source == NULL || target == NULL) {
            vfs_close_all(child);
            return 0;
        }
        *target = *source;
        target->owner = child;
        open_file_retain(target->file);
        pipe_retain_reference(target);
    }
    return 1;
}

int vfs_fork(uint64_t parent, uint64_t child) {
    ensure_standard_streams(parent);
    vfs_close_all(child);

    uint64_t flags = vfs_interrupt_lock();
    size_t needed = 0;
    size_t available = 0;
    for (size_t i = 0; i < VFS_DESCRIPTOR_SLOTS; i++) {
        if (descriptors[i].used && descriptors[i].owner == parent)
            needed++;
        if (!descriptors[i].used)
            available++;
    }
    if (needed > available) {
        vfs_interrupt_restore(flags);
        return 0;
    }

    int inherited_pipe = 0;
    for (size_t i = 0; i < VFS_DESCRIPTOR_SLOTS; i++) {
        struct vfs_fd *source = &descriptors[i];
        if (!source->used || source->owner != parent)
            continue;
        struct vfs_fd *target = allocate_slot();
        if (target == NULL) {
            vfs_interrupt_restore(flags);
            vfs_close_all(child);
            return 0;
        }
        *target = *source;
        target->owner = child;
        target->used = 1;
        open_file_retain_locked(target->file);
        if (target->pipe_mode == VFS_PIPE_READ && target->pipe->used) {
            target->pipe->readers++;
            inherited_pipe = 1;
        } else if (target->pipe_mode == VFS_PIPE_WRITE && target->pipe->used) {
            target->pipe->writers++;
            inherited_pipe = 1;
        }
    }
    vfs_interrupt_restore(flags);
    if (inherited_pipe)
        task_wake_blocked();
    return 1;
}

int vfs_read(int number, void *buffer, size_t length, size_t *read) {
    if (read)
        *read = 0;
    uint64_t owner = task_current_id();
    ensure_standard_streams(owner);
    struct vfs_fd *fd = lookup(owner, number);
    if (fd == NULL || buffer == NULL)
        return -1;
    if (fd->pipe_mode != VFS_PIPE_NONE)
        return pipe_read(fd, buffer, length, read);
    if (number == 1 || number == 2)
        return -1;
    if (fd->device != DEVFS_DEVICE_NONE)
        return devfs_read(fd->device, buffer, length, read) ? 0 : -1;
    if (number < 3)
        return -1;
    char data[RAMFS_DATA_SIZE];
    size_t size;
    const char *backend;
    const struct vfs_fs_ops *ops = path_ops(fd->name, &backend);
    if (!fd->file)
        return -1;
    int ok = ops->read(backend, data, sizeof(data), &size);
    if (!ok)
        return -1;
    if (fd->file->offset >= size)
        return 0;
    size_t n = size - fd->file->offset;
    if (n > length)
        n = length;
    for (size_t i = 0; i < n; i++)
        ((char *)buffer)[i] = data[fd->file->offset + i];
    fd->file->offset += n;
    if (read)
        *read = n;
    return 0;
}

int vfs_write(int number, const void *buffer, size_t length, size_t *written) {
    if (written)
        *written = 0;
    uint64_t owner = task_current_id();
    ensure_standard_streams(owner);
    struct vfs_fd *fd = lookup(owner, number);
    if (fd == NULL || buffer == NULL || length > RAMFS_DATA_SIZE)
        return -1;
    if (fd->pipe_mode != VFS_PIPE_NONE)
        return pipe_write(fd, buffer, length, written);
    if (number == 0)
        return -1;
    if (fd->device != DEVFS_DEVICE_NONE)
        return devfs_write(fd->device, buffer, length, written) ? 0 : -1;
    const char *backend;
    const struct vfs_fs_ops *ops = path_ops(fd->name, &backend);
    if (!fd->file)
        return -1;
    if (!ops->write(backend, buffer, length))
        return -1;
    fd->file->offset = length;
    if (written)
        *written = length;
    return 0;
}

int64_t vfs_seek(int number, int64_t offset, int whence) {
    struct vfs_fd *fd = lookup(task_current_id(), number);
    if (fd == NULL || number < 3 || fd->device != DEVFS_DEVICE_NONE ||
        fd->pipe_mode != VFS_PIPE_NONE || !fd->file)
        return -1;
    size_t size;
    const char *backend;
    const struct vfs_fs_ops *ops = path_ops(fd->name, &backend);
    if (!ops->stat(backend, &size))
        return -1;
    int64_t base = whence == 0   ? 0
                   : whence == 1 ? (int64_t)fd->file->offset
                   : whence == 2 ? (int64_t)size
                                 : -1;
    if (base < 0 || offset < -base || base + offset < 0 ||
        (uint64_t)(base + offset) > size)
        return -1;
    fd->file->offset = (size_t)(base + offset);
    return (int64_t)fd->file->offset;
}

int vfs_stat(const char *name, size_t *size) {
    char absolute[RAMFS_NAME_SIZE];
    if (!resolve_path(name, absolute, sizeof(absolute)))
        return 0;
    const char *backend;
    return path_ops(absolute, &backend)->stat(backend, size);
}

int vfs_readdir(const char *path, size_t index, struct fs_dirent *entry) {
    char absolute[RAMFS_NAME_SIZE];
    if (!entry || !resolve_path(path, absolute, sizeof(absolute)))
        return -1;
    const char *backend;
    return path_ops(absolute, &backend)->readdir(backend, index, entry);
}

int vfs_mkdir(const char *path) {
    char absolute[RAMFS_NAME_SIZE];
    if (!resolve_path(path, absolute, sizeof(absolute)))
        return 0;
    const char *backend;
    return path_ops(absolute, &backend)->mkdir(backend);
}

int vfs_unlink(const char *path) {
    char absolute[RAMFS_NAME_SIZE];
    if (!resolve_path(path, absolute, sizeof(absolute)))
        return 0;
    const char *backend;
    return path_ops(absolute, &backend)->unlink(backend);
}

int vfs_rmdir(const char *path) {
    char absolute[RAMFS_NAME_SIZE];
    if (!resolve_path(path, absolute, sizeof(absolute)))
        return 0;
    const char *backend;
    return path_ops(absolute, &backend)->rmdir(backend);
}

int vfs_is_directory(const char *path) {
    char absolute[RAMFS_NAME_SIZE];
    if (!resolve_path(path, absolute, sizeof(absolute)))
        return 0;
    if (names_equal(absolute, "/"))
        return 1;
    const char *backend;
    const struct vfs_mount *mount = find_mount(absolute, &backend);
    if (mount && names_equal(absolute, mount->path))
        return 1;
    return path_ops(absolute, &backend)->is_directory(backend);
}

int vfs_sync(void) {
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && !mounts[i].ops->sync())
            return 0;
    return 1;
}

int vfs_mount(const char *type, const char *target) {
    if (!type || !target || !names_equal(type, "fat32") || !fat32_mounted())
        return 0;
    char absolute[FS_PATH_MAX];
    if (!resolve_path(target, absolute, sizeof(absolute)) ||
        names_equal(absolute, "/") || !vfs_is_directory(absolute))
        return 0;
    return install_mount(absolute, "fat32", &fat32_ops, 1);
}

int vfs_mount_info(size_t index, struct fs_mount_info *info) {
    if (!info)
        return 0;
    if (index == 0) {
        const char *type = "ramfs", *path = "/";
        size_t i = 0;
        while (type[i]) {
            info->type[i] = type[i];
            i++;
        }
        info->type[i] = '\0';
        i = 0;
        while (path[i]) {
            info->path[i] = path[i];
            i++;
        }
        info->path[i] = '\0';
        return 1;
    }
    index--;
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].used)
            continue;
        if (index--)
            continue;
        size_t j = 0;
        while (mounts[i].type[j]) {
            info->type[j] = mounts[i].type[j];
            j++;
        }
        info->type[j] = '\0';
        j = 0;
        while (mounts[i].path[j]) {
            info->path[j] = mounts[i].path[j];
            j++;
        }
        info->path[j] = '\0';
        return 1;
    }
    return 0;
}

int vfs_unmount(const char *target) {
    char absolute[FS_PATH_MAX];
    if (!target || !resolve_path(target, absolute, sizeof(absolute)) ||
        names_equal(absolute, "/"))
        return 0;
    size_t mount_index = VFS_MAX_MOUNTS;
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && names_equal(mounts[i].path, absolute)) {
            mount_index = i;
            break;
        }
    if (mount_index == VFS_MAX_MOUNTS || !mounts[mount_index].removable ||
        process_cwd_in_mount(absolute))
        return 0;
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (i != mount_index && mounts[i].used &&
            path_has_prefix(mounts[i].path, absolute))
            return 0;
    }
    for (size_t i = 0; i < VFS_DESCRIPTOR_SLOTS; i++) {
        if (descriptors[i].used && descriptors[i].number >= 3 &&
            path_has_prefix(descriptors[i].name, absolute))
            return 0;
    }
    if (!mounts[mount_index].ops->sync())
        return 0;
    mounts[mount_index].used = 0;
    mounts[mount_index].path[0] = '\0';
    mounts[mount_index].type[0] = '\0';
    mounts[mount_index].ops = NULL;
    mounts[mount_index].removable = 0;
    return 1;
}
