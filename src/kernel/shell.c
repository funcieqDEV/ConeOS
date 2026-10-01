#include "shell.h"
#include "../drivers/pit.h"
#include "../drivers/ps2.h"
#include "../drivers/rtc.h"
#include "../drivers/serial.h"
#include "../fs/ramfs.h"
#include "../gfx/console.h"
#include "../mm/kmalloc.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "process.h"
#include "task.h"
#include <stddef.h>

#define SHELL_LINE_SIZE 64
#define VMM_TEST_ADDRESS 0xFFFFC00000000000ULL

static char line[SHELL_LINE_SIZE];
static size_t line_length;
static volatile uint64_t scheduler_test_steps[2];
static volatile uint8_t preemption_test_started[2];
static volatile uint8_t preemption_test_observed[2];
static volatile uint8_t preemption_test_done[2];

static void shell_write(const char *text) {
    print_serial(text);
    console_write(text);
}

static void shell_write_uint(uint64_t value) {
    char buffer[21];
    size_t index = sizeof(buffer) - 1;
    buffer[index] = '\0';

    do {
        buffer[--index] = '0' + value % 10;
        value /= 10;
    } while (value);

    shell_write(&buffer[index]);
}

static void shell_write_two_digits(uint64_t value) {
    char buffer[3] = {
        '0' + (value / 10) % 10,
        '0' + value % 10,
        '\0',
    };
    shell_write(buffer);
}

static void shell_write_four_digits(uint64_t value) {
    shell_write_two_digits(value / 100);
    shell_write_two_digits(value % 100);
}

static int strings_equal(const char *left, const char *right) {
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return *left == *right;
}

static void print_prompt(void) { shell_write("cone> "); }

static const char *task_state_name(enum task_state state) {
    switch (state) {
    case TASK_READY:
        return "ready";
    case TASK_RUNNING:
        return "running";
    case TASK_BLOCKED:
        return "blocked";
    case TASK_SLEEPING:
        return "sleeping";
    case TASK_ZOMBIE:
        return "zombie";
    }
    return "unknown";
}

static void scheduler_test_worker(void *argument) {
    size_t worker = (size_t)argument;
    for (size_t i = 0; i < 100; i++) {
        scheduler_test_steps[worker]++;
        task_yield();
    }
}

static void preemption_test_worker(void *argument) {
    size_t worker = (size_t)argument;
    size_t other = worker ^ 1;
    uint64_t deadline = pit_ticks() + PIT_FREQUENCY_HZ;

    preemption_test_started[worker] = 1;
    while (!preemption_test_started[other] && pit_ticks() < deadline)
        __asm__ volatile("pause");
    preemption_test_observed[worker] = preemption_test_started[other];
    preemption_test_done[worker] = 1;
}

static void stack_test_worker(void *argument) {
    (void)argument;
    volatile uint8_t *guard = (volatile uint8_t *)task_current_stack_guard();
    *guard = 0xFF;
}

static void execute_command(void) {
    line[line_length] = '\0';

    if (line_length == 0) {
        return;
    }

    if (strings_equal(line, "help")) {
        shell_write("Commands:\n"
                    "  help\n"
                    "  clear\n"
                    "  uptime\n"
                    "  date\n"
                    "  ls\n"
                    "  cat <file>\n"
                    "  write <file> <text>\n"
                    "  meminfo\n"
                    "  pmmtest\n"
                    "  vmmtest\n"
                    "  heapinfo\n"
                    "  heaptest\n"
                    "  tasks\n"
                    "  schedtest\n"
                    "  preempttest\n"
                    "  stacktest\n"
                    "  runuser\n"
                    "  processes\n"
                    "  panic\n");
    } else if (strings_equal(line, "clear")) {
        console_clear();
    } else if (strings_equal(line, "uptime")) {
        uint64_t milliseconds = pit_uptime_ms();
        shell_write("Uptime: ");
        shell_write_uint(milliseconds / 1000);
        shell_write(".");
        shell_write_two_digits(milliseconds % 1000 / 10);
        shell_write(" seconds\n");
    } else if (strings_equal(line, "date")) {
        struct rtc_time time;
        if (!rtc_read(&time)) {
            shell_write("Failed to read RTC\n");
        } else {
            shell_write_four_digits(time.year);
            shell_write("-");
            shell_write_two_digits(time.month);
            shell_write("-");
            shell_write_two_digits(time.day);
            shell_write(" ");
            shell_write_two_digits(time.hour);
            shell_write(":");
            shell_write_two_digits(time.minute);
            shell_write(":");
            shell_write_two_digits(time.second);
            shell_write(" UTC\n");
        }
    } else if (strings_equal(line, "ls")) {
        struct ramfs_file_info files[RAMFS_MAX_FILES];
        size_t count = ramfs_list(files, RAMFS_MAX_FILES);
        for (size_t i = 0; i < count; i++) {
            shell_write(files[i].name);
            shell_write(" ");
            shell_write_uint(files[i].size);
            shell_write(" bytes\n");
        }
    } else if (line_length > 4 && line[0] == 'c' && line[1] == 'a' &&
               line[2] == 't' && line[3] == ' ') {
        char buffer[RAMFS_DATA_SIZE];
        size_t size;
        if (!ramfs_read(&line[4], buffer, sizeof(buffer), &size)) {
            shell_write("cat: file not found\n");
        } else {
            for (size_t i = 0; i < size; i++)
                console_putchar(buffer[i]);
            console_putchar('\n');
        }
    } else if (line_length > 6 && line[0] == 'w' && line[1] == 'r' &&
               line[2] == 'i' && line[3] == 't' && line[4] == 'e' &&
               line[5] == ' ') {
        char *separator = &line[6];
        while (*separator && *separator != ' ')
            separator++;
        if (!*separator) {
            shell_write("write: invalid file or data\n");
        } else {
            *separator = '\0';
            int written = ramfs_write(&line[6], separator + 1,
                                       line_length -
                                           (size_t)(separator + 1 - line));
            *separator = ' ';
            if (!written)
                shell_write("write: invalid file or data\n");
            else
                shell_write("written\n");
        }
    } else if (strings_equal(line, "meminfo")) {
        uint64_t total = pmm_total_pages();
        uint64_t free = pmm_free_pages();
        shell_write("Physical memory: ");
        shell_write_uint(total * PMM_PAGE_SIZE / 1024 / 1024);
        shell_write(" MiB total, ");
        shell_write_uint(free * PMM_PAGE_SIZE / 1024 / 1024);
        shell_write(" MiB free, ");
        shell_write_uint((total - free) * PMM_PAGE_SIZE / 1024);
        shell_write(" KiB used\n");
    } else if (strings_equal(line, "pmmtest")) {
        uint64_t free_before = pmm_free_pages();
        uint64_t first = pmm_alloc_page();
        uint64_t second = pmm_alloc_page();
        int valid = first != PMM_INVALID_ADDRESS &&
                    second != PMM_INVALID_ADDRESS && first != second;
        int first_released =
            first == PMM_INVALID_ADDRESS || pmm_free_page(first);
        int second_released =
            second == PMM_INVALID_ADDRESS || pmm_free_page(second);
        int released = first_released && second_released;
        if (valid && released && pmm_free_pages() == free_before)
            shell_write("PMM test passed\n");
        else
            shell_write("PMM test failed\n");
    } else if (strings_equal(line, "vmmtest")) {
        uint64_t free_before = pmm_free_pages();
        uint64_t physical = pmm_alloc_page();
        int mapped = physical != PMM_INVALID_ADDRESS &&
                     vmm_map_page(VMM_TEST_ADDRESS, physical,
                                   VMM_WRITABLE | VMM_NO_EXECUTE);
        int contents_valid = 0;
        if (mapped) {
            volatile uint64_t *test = (volatile uint64_t *)VMM_TEST_ADDRESS;
            *test = 0x434F4E454F53564DULL;
            contents_valid = *test == 0x434F4E454F53564DULL &&
                             vmm_translate(VMM_TEST_ADDRESS) == physical;
        }

        uint64_t unmapped =
            mapped ? vmm_unmap_page(VMM_TEST_ADDRESS) : PMM_INVALID_ADDRESS;
        int released = physical == PMM_INVALID_ADDRESS ||
                       (unmapped == physical && pmm_free_page(physical));
        int cleaned = unmapped == physical &&
                      vmm_translate(VMM_TEST_ADDRESS) == PMM_INVALID_ADDRESS &&
                      pmm_free_pages() == free_before;
        if (mapped && contents_valid && released && cleaned)
            shell_write("VMM test passed\n");
        else
            shell_write("VMM test failed\n");
    } else if (strings_equal(line, "heapinfo")) {
        struct kmalloc_stats stats;
        kmalloc_get_stats(&stats);
        shell_write("Heap: ");
        shell_write_uint(stats.mapped_bytes / 1024);
        shell_write(" KiB mapped, ");
        shell_write_uint(stats.used_bytes);
        shell_write(" bytes used, ");
        shell_write_uint(stats.free_bytes);
        shell_write(" bytes free, ");
        shell_write_uint(stats.active_allocations);
        shell_write(" allocations\n");
    } else if (strings_equal(line, "heaptest")) {
        struct kmalloc_stats before;
        kmalloc_get_stats(&before);

        uint8_t *small = kmalloc(7);
        uint8_t *middle = kmalloc(1000);
        uint8_t *large = kmalloc(20000);
        int allocated = small != NULL && middle != NULL && large != NULL &&
                        (uint64_t)small % 16 == 0 &&
                        (uint64_t)middle % 16 == 0 && (uint64_t)large % 16 == 0;
        int contents_valid = allocated;
        if (allocated) {
            for (size_t i = 0; i < 7; i++)
                small[i] = (uint8_t)(0x10 + i);
            for (size_t i = 0; i < 1000; i++)
                middle[i] = (uint8_t)(i * 3);
            for (size_t i = 0; i < 20000; i++)
                large[i] = (uint8_t)(i * 7);
            for (size_t i = 0; i < 7; i++)
                contents_valid &= small[i] == (uint8_t)(0x10 + i);
            for (size_t i = 0; i < 1000; i++)
                contents_valid &= middle[i] == (uint8_t)(i * 3);
            for (size_t i = 0; i < 20000; i++)
                contents_valid &= large[i] == (uint8_t)(i * 7);
        }

        int middle_freed = middle != NULL && kfree(middle);
        int double_free_rejected = middle != NULL && !kfree(middle);
        int small_freed = small != NULL && kfree(small);
        int large_freed = large != NULL && kfree(large);

        struct kmalloc_stats after;
        kmalloc_get_stats(&after);
        int cleaned = after.used_bytes == before.used_bytes &&
                      after.active_allocations == before.active_allocations;
        if (allocated && contents_valid && middle_freed &&
            double_free_rejected && small_freed && large_freed && cleaned)
            shell_write("Heap test passed\n");
        else
            shell_write("Heap test failed\n");
    } else if (strings_equal(line, "tasks")) {
        size_t count = task_count();
        shell_write("Tasks: ");
        shell_write_uint(count);
        shell_write("\n");
        for (size_t i = 0; i < count; i++) {
            struct task_info info;
            if (!task_get_info(i, &info))
                continue;
            shell_write("  ");
            shell_write_uint(info.id);
            shell_write(" ");
            shell_write(info.name);
            shell_write(" ");
            shell_write(task_state_name(info.state));
            shell_write("\n");
        }
    } else if (strings_equal(line, "schedtest")) {
        scheduler_test_steps[0] = 0;
        scheduler_test_steps[1] = 0;
        uint64_t first =
            task_create("sched-a", scheduler_test_worker, (void *)0);
        uint64_t second =
            task_create("sched-b", scheduler_test_worker, (void *)1);

        if (first != UINT64_MAX || second != UINT64_MAX) {
            while ((first != UINT64_MAX && scheduler_test_steps[0] < 100) ||
                   (second != UINT64_MAX && scheduler_test_steps[1] < 100))
                task_yield();
            task_yield();
        }

        if (first != UINT64_MAX && second != UINT64_MAX &&
            scheduler_test_steps[0] == 100 && scheduler_test_steps[1] == 100 &&
            task_count() == 1)
            shell_write("Scheduler test passed\n");
        else
            shell_write("Scheduler test failed\n");
    } else if (strings_equal(line, "preempttest")) {
        preemption_test_started[0] = 0;
        preemption_test_started[1] = 0;
        preemption_test_observed[0] = 0;
        preemption_test_observed[1] = 0;
        preemption_test_done[0] = 0;
        preemption_test_done[1] = 0;

        uint64_t first =
            task_create("preempt-a", preemption_test_worker, (void *)0);
        uint64_t second =
            task_create("preempt-b", preemption_test_worker, (void *)1);

        if (first != UINT64_MAX || second != UINT64_MAX) {
            while ((first != UINT64_MAX && !preemption_test_done[0]) ||
                   (second != UINT64_MAX && !preemption_test_done[1]))
                task_yield();
            task_yield();
        }

        if (first != UINT64_MAX && second != UINT64_MAX &&
            preemption_test_observed[0] && preemption_test_observed[1] &&
            task_count() == 1)
            shell_write("Preemption test passed\n");
        else
            shell_write("Preemption test failed\n");
    } else if (strings_equal(line, "stacktest")) {
        shell_write("Triggering a task stack guard page fault...\n");
        if (task_create("stack-fault", stack_test_worker, NULL) == UINT64_MAX) {
            shell_write("Failed to create stack test task\n");
        } else {
            task_yield();
        }
    } else if (strings_equal(line, "runuser")) {
        if (!process_run_hello()) {
            shell_write("Failed to start user process\n");
        } else {
            while (process_is_running())
                task_yield();
            task_yield();
            shell_write("User process exited\n");
        }
    } else if (strings_equal(line, "processes")) {
        shell_write(process_is_running() ? "Processes: 1 running\n"
                                         : "Processes: 0 running\n");
    } else if (strings_equal(line, "panic")) {
        __asm__ volatile("ud2");
    } else {
        shell_write("Unknown command: ");
        shell_write(line);
        shell_write("\n");
    }
}

static void handle_char(char c) {
    if (c == '\r' || c == '\n') {
        shell_write("\n");
        execute_command();
        line_length = 0;
        print_prompt();
        return;
    }

    if (c == '\b') {
        if (line_length > 0) {
            line_length--;
            shell_write("\b \b");
        }
        return;
    }

    if (c < ' ' || c > '~' || line_length >= SHELL_LINE_SIZE - 1)
        return;

    line[line_length++] = c;
    print_serial_char(c);
    console_putchar(c);
}

void shell_init(void) {
    shell_write("\nConeOS shell. Type 'help' for commands.\n");
    print_prompt();
}

void shell_poll(void) {
    char c;
    while (keyboard_read_char(&c))
        handle_char(c);
}
