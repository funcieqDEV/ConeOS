#include "process.h"
#include "../cpu/syscall.h"
#include "../cpu/syscall_numbers.h"
#include "../fs/ramfs.h"
#include "../fs/vfs.h"
#include "../mm/kmalloc.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "elf.h"
#include "task.h"
#include <stddef.h>
#include <stdint.h>

#define USER_STACK_ADDRESS 0x0000000000800000ULL
#define USER_STACK_TOP (USER_STACK_ADDRESS + PMM_PAGE_SIZE)

#define MAX_PROCESSES 16
#define MAX_MMAP_REGIONS 32
#define PROCESS_MEMORY_LIMIT_PAGES (16ULL * 1024 * 1024 / PMM_PAGE_SIZE)
#define MMAP_ADDRESS_START 0x0000000100000000ULL
#define MMAP_ADDRESS_LIMIT 0x00007F0000000000ULL

enum process_image_result {
    PROCESS_IMAGE_OUT_OF_MEMORY = -1,
    PROCESS_IMAGE_INVALID = 0,
    PROCESS_IMAGE_READY = 1,
};

struct mmap_region {
    uint64_t start;
    uint64_t end;
    uint64_t protection;
    int used;
};

enum process_state { PROCESS_UNUSED, PROCESS_RUNNING, PROCESS_ZOMBIE };

struct process_record {
    uint64_t pid;
    uint64_t ppid;
    enum process_state state;
    int exit_status;
    struct vmm_space *space;
    uint64_t heap_base;
    uint64_t program_break;
    uint64_t committed_pages;
    char cwd[FS_PATH_MAX];
    struct mmap_region mappings[MAX_MMAP_REGIONS];
};

static uint64_t align_up_page(uint64_t address) {
    return (address + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
}

static void copy_path(char *destination, const char *source) {
    size_t i = 0;
    while (source[i] && i < FS_PATH_MAX - 1) {
        destination[i] = source[i];
        i++;
    }
    destination[i] = '\0';
}

static struct process_record processes[MAX_PROCESSES];

static struct process_record *find_process(uint64_t pid) {
    for (size_t i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].state != PROCESS_UNUSED && processes[i].pid == pid)
            return &processes[i];
    return NULL;
}

static struct process_record *free_process_record(void) {
    for (size_t i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].state == PROCESS_UNUSED)
            return &processes[i];
    return NULL;
}

__attribute__((noreturn)) extern void
process_enter_user(uint64_t instruction_pointer, uint64_t stack_pointer,
                   uint64_t argc, uint64_t argv);

struct process_launch {
    uint64_t entry;
    uint64_t stack_top;
    uint64_t argc;
    uint64_t argv;
};

struct process_fork_launch {
    struct syscall_frame frame;
};

struct process_image {
    struct vmm_space *space;
    uint64_t entry;
    uint64_t heap_base;
    uint64_t stack_pointer;
    uint64_t argc;
    uint64_t argv;
    uint64_t committed_pages;
};

__asm__(".global process_enter_user\n"
        ".type process_enter_user, @function\n"
        "process_enter_user:\n"
        "    movq %rdi, %r8\n"
        "    movq %rsi, %r9\n"
        "    cli\n"
        "    movw $0x1b, %ax\n"
        "    movw %ax, %ds\n"
        "    movw %ax, %es\n"
        "    pushq $0x1b\n"
        "    pushq %r9\n"
        "    pushq $0x202\n"
        "    pushq $0x23\n"
        "    pushq %r8\n"
        "    movq %rdx, %rdi\n"
        "    movq %rcx, %rsi\n"
        "    iretq\n"
        ".size process_enter_user, .-process_enter_user\n");

static void user_process_task(void *argument) {
    struct process_launch *launch = argument;
    uint64_t entry = launch->entry;
    uint64_t stack_top = launch->stack_top;
    uint64_t argc = launch->argc;
    uint64_t argv = launch->argv;
    kfree(launch);
    process_enter_user(entry, stack_top, argc, argv);
    __builtin_unreachable();
}

static void forked_process_task(void *argument) {
    struct process_fork_launch *launch = argument;
    struct syscall_frame frame = launch->frame;
    kfree(launch);
    syscall_resume_user(&frame);
}

static enum process_image_result
create_process_image(const char *path, const char *const arguments[],
                     size_t argc, struct process_image *image) {
    if (path == NULL || image == NULL || argc > PROCESS_MAX_ARGUMENTS ||
        (argc != 0 && arguments == NULL))
        return PROCESS_IMAGE_INVALID;

    struct vmm_space *space = vmm_space_create();
    if (space == NULL)
        return PROCESS_IMAGE_OUT_OF_MEMORY;

    static uint8_t executable_image[RAMFS_DATA_SIZE];
    size_t image_size;
    uint64_t entry, image_end, elf_pages;
    enum process_image_result failure = PROCESS_IMAGE_INVALID;
    if (!ramfs_read(path, (char *)executable_image, sizeof(executable_image),
                    &image_size))
        goto failure;
    enum elf_load_result load_result = elf_load(
        executable_image, image_size, space, PROCESS_MEMORY_LIMIT_PAGES - 1,
        &entry, &image_end, &elf_pages);
    if (load_result != ELF_LOAD_SUCCESS) {
        if (load_result == ELF_LOAD_OUT_OF_MEMORY)
            failure = PROCESS_IMAGE_OUT_OF_MEMORY;
        goto failure;
    }

    uint64_t stack_physical = pmm_alloc_user_page();
    if (stack_physical == PMM_INVALID_ADDRESS)
        goto out_of_memory;
    if (!vmm_space_map_page(space, USER_STACK_ADDRESS, stack_physical,
                            VMM_USER | VMM_WRITABLE | VMM_NO_EXECUTE)) {
        pmm_free_page(stack_physical);
        goto out_of_memory;
    }

    uint8_t *stack = pmm_physical_to_virtual(stack_physical);
    uint64_t user_arguments[PROCESS_MAX_ARGUMENTS];
    uint64_t cursor = PMM_PAGE_SIZE;
    for (size_t index = argc; index > 0; index--) {
        const char *argument = arguments[index - 1];
        if (argument == NULL)
            goto failure;
        size_t length = 0;
        while (length < PMM_PAGE_SIZE && argument[length])
            length++;
        if (length == PMM_PAGE_SIZE || length + 1 > cursor)
            goto failure;
        cursor -= length + 1;
        user_arguments[index - 1] = USER_STACK_ADDRESS + cursor;
        for (size_t i = 0; i < length; i++)
            stack[cursor + i] = argument[i];
        stack[cursor + length] = '\0';
    }

    cursor &= ~7ULL;
    uint64_t argv_bytes = (argc + 1) * sizeof(uint64_t);
    if (argv_bytes > cursor)
        goto failure;
    cursor -= argv_bytes;
    uint64_t *user_argv = (uint64_t *)&stack[cursor];
    for (size_t i = 0; i < argc; i++)
        user_argv[i] = user_arguments[i];
    user_argv[argc] = 0;

    image->space = space;
    image->entry = entry;
    image->heap_base = align_up_page(image_end);
    image->stack_pointer = USER_STACK_ADDRESS + cursor;
    image->argc = argc;
    image->argv = USER_STACK_ADDRESS + cursor;
    image->committed_pages = elf_pages + 1;
    return PROCESS_IMAGE_READY;

out_of_memory:
    failure = PROCESS_IMAGE_OUT_OF_MEMORY;

failure:
    vmm_space_destroy(space);
    return failure;
}

static int64_t process_spawn_path_args(const char *path, const char *argument) {
    struct process_record *record = free_process_record();
    if (record == NULL)
        return SYSCALL_ENOMEM;
    const char *arguments[PROCESS_MAX_ARGUMENTS];
    char argument_storage[PROCESS_MAX_ARGUMENTS - 1][64];
    size_t argc = 1;
    arguments[0] = path;
    size_t position = 0;
    while (argument != NULL && argument[position] &&
           argc < PROCESS_MAX_ARGUMENTS) {
        while (argument[position] == ' ')
            position++;
        if (!argument[position])
            break;
        size_t start = position;
        while (argument[position] && argument[position] != ' ')
            position++;
        size_t length = position - start;
        if (length >= sizeof(argument_storage[0]))
            return SYSCALL_EINVAL;
        char *copy = argument_storage[argc - 1];
        for (size_t i = 0; i < length; i++)
            copy[i] = argument[start + i];
        copy[length] = '\0';
        arguments[argc++] = copy;
    }

    struct process_image image;
    enum process_image_result image_result =
        create_process_image(path, arguments, argc, &image);
    if (image_result != PROCESS_IMAGE_READY)
        return image_result == PROCESS_IMAGE_OUT_OF_MEMORY ? SYSCALL_ENOMEM
                                                           : SYSCALL_EINVAL;

    struct process_launch *launch = kmalloc(sizeof(*launch));
    if (launch == NULL) {
        vmm_space_destroy(image.space);
        return SYSCALL_ENOMEM;
    }
    launch->entry = image.entry;
    launch->stack_top = image.stack_pointer;
    launch->argc = image.argc;
    launch->argv = image.argv;

    uint64_t pid = task_create_in_space("user-hello", user_process_task, launch,
                                        image.space);
    if (pid == UINT64_MAX) {
        kfree(launch);
        vmm_space_destroy(image.space);
        return SYSCALL_ENOMEM;
    }

    record->pid = pid;
    record->ppid = task_current_id();
    record->state = PROCESS_RUNNING;
    record->exit_status = 0;
    record->space = image.space;
    record->heap_base = image.heap_base;
    record->program_break = record->heap_base;
    record->committed_pages = image.committed_pages;
    for (size_t i = 0; i < MAX_MMAP_REGIONS; i++)
        record->mappings[i].used = 0;
    struct process_record *parent = find_process(record->ppid);
    copy_path(record->cwd, parent != NULL ? parent->cwd : "/");
    (void)vfs_inherit_standard(record->ppid, pid);
    return (int64_t)pid;
}

int process_getcwd(char *buffer, size_t capacity) {
    struct process_record *process = find_process(task_current_id());
    const char *cwd = process != NULL ? process->cwd : "/";
    size_t length = 0;
    while (cwd[length])
        length++;
    if (buffer == NULL || capacity <= length)
        return 0;
    for (size_t i = 0; i <= length; i++)
        buffer[i] = cwd[i];
    return 1;
}

int process_chdir(const char *path) {
    struct process_record *process = find_process(task_current_id());
    if (process == NULL || path == NULL)
        return 0;
    char absolute[FS_PATH_MAX];
    size_t out = 0, in = 0;
    if (path[0] == '/') {
        absolute[out++] = '/';
        while (path[in] == '/')
            in++;
    } else {
        while (process->cwd[out] && out < sizeof(absolute) - 1)
            absolute[out] = process->cwd[out], out++;
    }
    while (path[in]) {
        size_t start = in;
        while (path[in] && path[in] != '/')
            in++;
        size_t length = in - start;
        while (path[in] == '/')
            in++;
        if (length == 0 || (length == 1 && path[start] == '.'))
            continue;
        if (length == 2 && path[start] == '.' && path[start + 1] == '.') {
            if (out > 1) {
                if (absolute[out - 1] == '/')
                    out--;
                while (out > 1 && absolute[out - 1] != '/')
                    out--;
            }
            continue;
        }
        if (out > 1 && absolute[out - 1] != '/') {
            if (out >= sizeof(absolute) - 1)
                return 0;
            absolute[out++] = '/';
        }
        if (out + length >= sizeof(absolute))
            return 0;
        for (size_t i = 0; i < length; i++)
            absolute[out++] = path[start + i];
    }
    if (out == 0)
        absolute[out++] = '/';
    absolute[out] = '\0';
    if (!vfs_is_directory(absolute))
        return 0;
    copy_path(process->cwd, absolute);
    return 1;
}

int process_cwd_in_mount(const char *mount_path) {
    size_t length = 0;
    while (mount_path[length])
        length++;
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state != PROCESS_RUNNING)
            continue;
        size_t j = 0;
        while (j < length && processes[i].cwd[j] == mount_path[j])
            j++;
        if (j == length &&
            (processes[i].cwd[j] == '\0' || processes[i].cwd[j] == '/'))
            return 1;
    }
    return 0;
}

int64_t process_fork(struct syscall_frame *frame) {
    struct process_record *parent = find_process(task_current_id());
    struct process_record *child = free_process_record();
    if (frame == NULL || parent == NULL || parent->state != PROCESS_RUNNING ||
        parent->space == NULL || child == NULL)
        return SYSCALL_ENOMEM;

    struct process_fork_launch *launch = kmalloc(sizeof(*launch));
    if (launch == NULL)
        return SYSCALL_ENOMEM;
    launch->frame = *frame;
    launch->frame.rax = 0;

    struct vmm_space *space = vmm_space_clone_cow(parent->space);
    if (space == NULL) {
        kfree(launch);
        return SYSCALL_ENOMEM;
    }

    uint64_t pid =
        task_create_in_space("fork", forked_process_task, launch, space);
    if (pid == UINT64_MAX) {
        vmm_space_destroy(space);
        kfree(launch);
        return SYSCALL_ENOMEM;
    }
    if (!vfs_fork(parent->pid, pid)) {
        (void)task_cancel_ready(pid);
        kfree(launch);
        task_yield();
        return SYSCALL_ENOMEM;
    }

    *child = *parent;
    child->pid = pid;
    child->ppid = parent->pid;
    child->state = PROCESS_RUNNING;
    child->exit_status = 0;
    child->space = space;
    return (int64_t)pid;
}

static size_t mapping_covering(const struct process_record *process,
                               uint64_t address);

int64_t process_brk(uint64_t requested) {
    struct process_record *process = find_process(task_current_id());
    if (process == NULL || process->state != PROCESS_RUNNING || !process->space)
        return SYSCALL_EINVAL;
    if (requested == 0)
        return (int64_t)process->program_break;
    if (requested < process->heap_base ||
        requested > USER_STACK_ADDRESS - PMM_PAGE_SIZE)
        return SYSCALL_EINVAL;

    uint64_t old_break = process->program_break;
    uint64_t old_page = align_up_page(old_break);
    uint64_t new_page = align_up_page(requested);
    uint64_t old_heap_pages = (old_page - process->heap_base) / PMM_PAGE_SIZE;
    uint64_t new_heap_pages = (new_page - process->heap_base) / PMM_PAGE_SIZE;
    if (new_heap_pages > old_heap_pages) {
        uint64_t additional_pages = new_heap_pages - old_heap_pages;
        if (process->committed_pages > PROCESS_MEMORY_LIMIT_PAGES ||
            additional_pages >
                PROCESS_MEMORY_LIMIT_PAGES - process->committed_pages)
            return SYSCALL_ENOMEM;
    }

    if (requested > old_break) {
        /* A page retained across a shrink must not reveal its old contents. */
        for (uint64_t address = old_break; address < requested;) {
            uint64_t physical = vmm_space_translate(process->space, address);
            uint64_t amount = PMM_PAGE_SIZE - (address & (PMM_PAGE_SIZE - 1));
            if (amount > requested - address)
                amount = requested - address;
            if (physical != PMM_INVALID_ADDRESS) {
                uint8_t *memory = pmm_physical_to_virtual(physical);
                for (uint64_t i = 0; i < amount; i++)
                    memory[i] = 0;
            }
            address += amount;
        }
    } else {
        for (uint64_t address = new_page; address < old_page;
             address += PMM_PAGE_SIZE) {
            uint64_t physical = vmm_space_unmap_page(process->space, address);
            if (physical != PMM_INVALID_ADDRESS)
                pmm_free_page(physical);
        }
    }

    process->program_break = requested;
    if (new_heap_pages >= old_heap_pages)
        process->committed_pages += new_heap_pages - old_heap_pages;
    else
        process->committed_pages -= old_heap_pages - new_heap_pages;
    return requested;
}

int process_handle_page_fault(uint64_t address, uint64_t error_code) {
    /* Only resolve user-mode faults; reserved-bit faults stay fatal. */
    if (!(error_code & 4) || (error_code & 8))
        return 0;

    struct process_record *process = find_process(task_current_id());
    if (process == NULL || process->state != PROCESS_RUNNING ||
        process->space == NULL)
        return 0;

    if (error_code & 1) {
        if (!(error_code & 2))
            return 0;
        return vmm_space_resolve_cow(process->space, address);
    }

    uint64_t protection = 0;
    if (address >= process->heap_base && address < process->program_break) {
        protection = CONEOS_PROT_READ | CONEOS_PROT_WRITE;
    } else {
        size_t index = mapping_covering(process, address);
        if (index != MAX_MMAP_REGIONS)
            protection = process->mappings[index].protection;
    }
    if (!(protection & CONEOS_PROT_READ) ||
        ((error_code & 2) && !(protection & CONEOS_PROT_WRITE)) ||
        ((error_code & 16) && !(protection & CONEOS_PROT_EXEC)))
        return 0;

    uint64_t page_address = address & ~(uint64_t)(PMM_PAGE_SIZE - 1);
    if (vmm_space_translate(process->space, page_address) !=
        PMM_INVALID_ADDRESS)
        return 0;

    uint64_t physical = pmm_alloc_user_page();
    if (physical == PMM_INVALID_ADDRESS)
        return -1;
    uint8_t *page = pmm_physical_to_virtual(physical);
    for (size_t i = 0; i < PMM_PAGE_SIZE; i++)
        page[i] = 0;

    uint64_t flags = VMM_USER;
    if (protection & CONEOS_PROT_WRITE)
        flags |= VMM_WRITABLE;
    if (!(protection & CONEOS_PROT_EXEC))
        flags |= VMM_NO_EXECUTE;
    if (!vmm_space_map_page(process->space, page_address, physical, flags)) {
        pmm_free_page(physical);
        return -1;
    }
    return 1;
}

static int rounded_mapping_length(uint64_t length, uint64_t *rounded) {
    if (length == 0 || length > UINT64_MAX - (PMM_PAGE_SIZE - 1))
        return 0;
    *rounded = (length + PMM_PAGE_SIZE - 1) & ~(uint64_t)(PMM_PAGE_SIZE - 1);
    return 1;
}

static size_t find_free_mapping_slot(const struct process_record *process) {
    for (size_t i = 0; i < MAX_MMAP_REGIONS; i++)
        if (!process->mappings[i].used)
            return i;
    return MAX_MMAP_REGIONS;
}

static int select_mapping_address(const struct process_record *process,
                                  uint64_t hint, uint64_t length,
                                  uint64_t *address) {
    uint64_t candidates[2] = {hint ? hint : MMAP_ADDRESS_START,
                              MMAP_ADDRESS_START};
    size_t candidate_count = hint && hint != MMAP_ADDRESS_START ? 2 : 1;

    if (length > MMAP_ADDRESS_LIMIT - MMAP_ADDRESS_START)
        return 0;

    for (size_t attempt = 0; attempt < candidate_count; attempt++) {
        uint64_t candidate = candidates[attempt];
        while (candidate <= MMAP_ADDRESS_LIMIT - length) {
            uint64_t next_region = 0;
            for (size_t i = 0; i < MAX_MMAP_REGIONS; i++) {
                const struct mmap_region *region = &process->mappings[i];
                if (!region->used || candidate + length <= region->start ||
                    candidate >= region->end)
                    continue;
                if (region->end > next_region)
                    next_region = region->end;
            }
            if (next_region != 0) {
                candidate = align_up_page(next_region);
                continue;
            }

            uint64_t end = candidate + length;
            int occupied = 0;
            for (uint64_t page = candidate; page < end; page += PMM_PAGE_SIZE) {
                if (vmm_space_translate(process->space, page) ==
                    PMM_INVALID_ADDRESS)
                    continue;
                candidate += PMM_PAGE_SIZE;
                occupied = 1;
                break;
            }
            if (!occupied) {
                *address = candidate;
                return 1;
            }
        }
    }
    return 0;
}

int64_t process_mmap(uint64_t hint, uint64_t length, uint64_t protection,
                     uint64_t flags) {
    struct process_record *process = find_process(task_current_id());
    const uint64_t valid_protection =
        CONEOS_PROT_READ | CONEOS_PROT_WRITE | CONEOS_PROT_EXEC;
    const uint64_t required_flags = CONEOS_MAP_PRIVATE | CONEOS_MAP_ANONYMOUS;
    uint64_t rounded_length;

    if (!process || process->state != PROCESS_RUNNING || !process->space ||
        !rounded_mapping_length(length, &rounded_length) ||
        (protection & ~valid_protection) != 0 ||
        !(protection & CONEOS_PROT_READ) ||
        (protection & (CONEOS_PROT_WRITE | CONEOS_PROT_EXEC)) ==
            (CONEOS_PROT_WRITE | CONEOS_PROT_EXEC) ||
        flags != required_flags ||
        (hint && (hint % PMM_PAGE_SIZE != 0 || hint < MMAP_ADDRESS_START ||
                  hint >= MMAP_ADDRESS_LIMIT)))
        return SYSCALL_EINVAL;

    size_t slot = find_free_mapping_slot(process);
    if (slot == MAX_MMAP_REGIONS)
        return SYSCALL_ENOMEM;

    uint64_t mapping_pages = rounded_length / PMM_PAGE_SIZE;
    if (process->committed_pages > PROCESS_MEMORY_LIMIT_PAGES ||
        mapping_pages > PROCESS_MEMORY_LIMIT_PAGES - process->committed_pages)
        return SYSCALL_ENOMEM;

    uint64_t address;
    if (!select_mapping_address(process, hint, rounded_length, &address))
        return SYSCALL_ENOMEM;

    uint64_t end = address + rounded_length;
    process->mappings[slot].start = address;
    process->mappings[slot].end = end;
    process->mappings[slot].protection = protection;
    process->mappings[slot].used = 1;
    process->committed_pages += mapping_pages;
    return (int64_t)address;
}

static size_t mapping_covering(const struct process_record *process,
                               uint64_t address) {
    for (size_t i = 0; i < MAX_MMAP_REGIONS; i++) {
        const struct mmap_region *region = &process->mappings[i];
        if (region->used && address >= region->start && address < region->end)
            return i;
    }
    return MAX_MMAP_REGIONS;
}

int64_t process_munmap(uint64_t address, uint64_t length) {
    struct process_record *process = find_process(task_current_id());
    uint64_t rounded_length;
    if (!process || process->state != PROCESS_RUNNING || !process->space ||
        address % PMM_PAGE_SIZE != 0 ||
        !rounded_mapping_length(length, &rounded_length) ||
        address < MMAP_ADDRESS_START || address >= MMAP_ADDRESS_LIMIT ||
        rounded_length > MMAP_ADDRESS_LIMIT - address)
        return SYSCALL_EINVAL;

    uint64_t end = address + rounded_length;
    uint64_t cursor = address;
    while (cursor < end) {
        size_t index = mapping_covering(process, cursor);
        if (index == MAX_MMAP_REGIONS)
            return SYSCALL_EINVAL;
        uint64_t segment_end = process->mappings[index].end;
        if (segment_end > end)
            segment_end = end;
        cursor = segment_end;
    }

    size_t split_slot = MAX_MMAP_REGIONS;
    for (size_t i = 0; i < MAX_MMAP_REGIONS; i++) {
        struct mmap_region *region = &process->mappings[i];
        if (!region->used || address >= region->end || end <= region->start)
            continue;
        if (address > region->start && end < region->end) {
            split_slot = find_free_mapping_slot(process);
            if (split_slot == MAX_MMAP_REGIONS)
                return SYSCALL_ENOMEM;
            break;
        }
    }

    for (uint64_t page = address; page < end; page += PMM_PAGE_SIZE) {
        uint64_t physical = vmm_space_unmap_page(process->space, page);
        if (physical != PMM_INVALID_ADDRESS)
            pmm_free_page(physical);
    }

    for (size_t i = 0; i < MAX_MMAP_REGIONS; i++) {
        struct mmap_region *region = &process->mappings[i];
        if (!region->used || address >= region->end || end <= region->start)
            continue;
        uint64_t cut_start = address > region->start ? address : region->start;
        uint64_t cut_end = end < region->end ? end : region->end;
        if (cut_start == region->start && cut_end == region->end) {
            region->used = 0;
        } else if (cut_start == region->start) {
            region->start = cut_end;
        } else if (cut_end == region->end) {
            region->end = cut_start;
        } else {
            process->mappings[split_slot] = *region;
            process->mappings[split_slot].start = cut_end;
            region->end = cut_start;
        }
    }
    process->committed_pages -= rounded_length / PMM_PAGE_SIZE;
    return 0;
}

int process_spawn_path(const char *path) {
    return process_spawn_path_args(path, NULL) >= 0;
}

int process_run_hello(void) { return process_spawn_path("/bin/init"); }

int64_t process_exec_path_args(const char *path, const char *const arguments[],
                               size_t argc) {
    struct process_record *process = find_process(task_current_id());
    struct process_image image;
    if (process == NULL || process->state != PROCESS_RUNNING)
        return SYSCALL_EINVAL;
    enum process_image_result image_result =
        create_process_image(path, arguments, argc, &image);
    if (image_result != PROCESS_IMAGE_READY)
        return image_result == PROCESS_IMAGE_OUT_OF_MEMORY ? SYSCALL_ENOMEM
                                                           : SYSCALL_EINVAL;

    struct vmm_space *old_space = task_replace_address_space(image.space);
    if (old_space == NULL) {
        vmm_space_destroy(image.space);
        return SYSCALL_EINVAL;
    }

    process->space = image.space;
    process->heap_base = image.heap_base;
    process->program_break = image.heap_base;
    process->committed_pages = image.committed_pages;
    process->exit_status = 0;
    for (size_t i = 0; i < MAX_MMAP_REGIONS; i++)
        process->mappings[i].used = 0;

    vmm_space_destroy(old_space);
    process_enter_user(image.entry, image.stack_pointer, image.argc,
                       image.argv);
}

int64_t process_exec_path(const char *path) {
    const char *arguments[] = {path};
    return process_exec_path_args(path, arguments, 1);
}

int64_t process_spawn_child(const char *path) {
    return process_spawn_path_args(path, NULL);
}

int64_t process_spawn_child_args(const char *path, const char *argument) {
    return process_spawn_path_args(path, argument);
}

int process_is_running(void) {
    for (size_t i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].state == PROCESS_RUNNING)
            return 1;
    return 0;
}

int process_waitpid(uint64_t pid, int *status) {
    struct process_record *process = find_process(pid);
    if (process == NULL || process->ppid != task_current_id())
        return -1;
    while (process->state != PROCESS_ZOMBIE)
        task_yield();
    if (status != NULL)
        *status = process->exit_status;
    process->state = PROCESS_UNUSED;
    return pid;
}

__attribute__((noreturn)) void process_exit_current(int status) {
    vfs_close_all(task_current_id());
    struct process_record *process = find_process(task_current_id());
    if (process != NULL) {
        process->exit_status = status;
        process->state = PROCESS_ZOMBIE;
    }
    task_exit_current();
}
