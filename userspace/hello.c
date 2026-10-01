#include <stddef.h>
#include <stdint.h>
#include "lib/syscall.h"
#include "../src/cpu/syscall_numbers.h"

static uint64_t string_length(const char *text) {
    uint64_t length = 0;
    while (text[length])
        length++;
    return length;
}

static void write(const char *text) {
    sys_write(text, string_length(text));
}

static void write_uint(uint64_t value) {
    char buffer[21];
    size_t index = sizeof(buffer);
    do {
        buffer[--index] = '0' + value % 10;
        value /= 10;
    } while (value);
    sys_write(&buffer[index], sizeof(buffer) - index);
}

static int starts_with(const char *text, uint64_t length, const char *prefix) {
    uint64_t i = 0;
    while (prefix[i]) {
        if (i >= length || text[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

static int run_program(const char *path, const char *arguments) {
    int64_t pid = sys_spawn(path, arguments);
    if (pid < 0) return 0;
    int status = 0;
    if (sys_waitpid((uint64_t)pid, &status) < 0) write("waitpid failed\n");
    else if (status != 0) { write("process exited with status "); write_uint((uint64_t)status); write("\n"); }
    return 1;
}

static char *skip_spaces(char *text) {
    while (*text == ' ') text++;
    return text;
}

static int command_path(char *text, char path[FS_PATH_MAX],
                        const char **arguments) {
    text = skip_spaces(text);
    if (!*text) return 0;
    char *split = text;
    while (*split && *split != ' ') split++;
    *arguments = NULL;
    if (*split) {
        *split++ = '\0';
        split = skip_spaces(split);
        if (*split) *arguments = split;
    }
    const char *command = text;
    uint64_t out = 0;
    if (command[0] == '.' && command[1] == '/') command++;
    if (command[0] != '/') {
        const char prefix[] = "/bin/";
        for (uint64_t i = 0; prefix[i]; i++) path[out++] = prefix[i];
    }
    for (uint64_t i = 0; command[i]; i++) {
        if (out >= FS_PATH_MAX - 1) return 0;
        path[out++] = command[i];
    }
    path[out] = '\0';
    return 1;
}

static void run_pipeline(char *line, uint64_t length) {
    char *separator = NULL;
    for (uint64_t i = 0; i < length; i++) {
        if (line[i] != '|') continue;
        if (separator) { write("shell: only one pipe is supported\n"); return; }
        separator = &line[i];
    }
    if (!separator) return;
    char *right = separator + 1;
    *separator = '\0';
    char *left = line;
    while (*left == ' ') left++;
    char *end = separator;
    while (end > left && end[-1] == ' ') *--end = '\0';
    right = skip_spaces(right);
    if (!*left || !*right) { write("shell: expected command on both sides of |\n"); return; }

    char left_path[FS_PATH_MAX], right_path[FS_PATH_MAX];
    const char *left_args, *right_args;
    if (!command_path(left, left_path, &left_args) ||
        !command_path(right, right_path, &right_args)) {
        write("shell: invalid command\n");
        return;
    }

    int64_t saved_input = sys_dup(0);
    int64_t saved_output = sys_dup(1);
    int pipe_fds[2] = {-1, -1};
    if (saved_input < 0 || saved_output < 0 || sys_pipe(pipe_fds) < 0) {
        if (saved_input >= 0) sys_close((int)saved_input);
        if (saved_output >= 0) sys_close((int)saved_output);
        write("shell: cannot create pipe\n");
        return;
    }

    int64_t left_pid = -1, right_pid = -1;
    if (sys_dup2(pipe_fds[1], 1) >= 0)
        left_pid = sys_spawn(left_path, left_args);
    (void)sys_dup2((int)saved_output, 1);

    if (sys_dup2(pipe_fds[0], 0) >= 0)
        right_pid = sys_spawn(right_path, right_args);
    (void)sys_dup2((int)saved_input, 0);

    sys_close(pipe_fds[0]);
    sys_close(pipe_fds[1]);
    sys_close((int)saved_input);
    sys_close((int)saved_output);

    if (left_pid < 0) write("shell: left command failed to start\n");
    if (right_pid < 0) write("shell: right command failed to start\n");
    int status;
    if (left_pid >= 0 && sys_waitpid((uint64_t)left_pid, &status) < 0)
        write("shell: waitpid failed\n");
    if (right_pid >= 0 && sys_waitpid((uint64_t)right_pid, &status) < 0)
        write("shell: waitpid failed\n");
}

static void run_command(char *line) {
    char path[FS_PATH_MAX];
    const char *arguments;
    if (!command_path(line, path, &arguments)) {
        write("shell: invalid command\n");
        return;
    }
    if (!run_program(path, arguments)) { write(line); write(": command not found\n"); }
}

static void exec_command(char *text) {
    char path[FS_PATH_MAX];
    const char *arguments;
    if (!command_path(text, path, &arguments)) {
        write("exec: invalid command\n");
        return;
    }

    char *argv[17];
    size_t argc = 0;
    argv[argc++] = skip_spaces(text);
    char *cursor = (char *)arguments;
    while (cursor != NULL && *cursor) {
        cursor = skip_spaces(cursor);
        if (!*cursor) break;
        if (argc == 16) {
            write("exec: too many arguments\n");
            return;
        }
        argv[argc++] = cursor;
        while (*cursor && *cursor != ' ') cursor++;
        if (*cursor) *cursor++ = '\0';
    }
    argv[argc] = NULL;
    if (sys_execv(path, (const char *const *)argv) < 0)
        write("exec failed\n");
}

__attribute__((noreturn)) void _start(uint64_t argc, char **argv) {
    char line[FS_PATH_MAX];
    write("ConeOS userspace shell\nType 'help'\nuser> ");
    if (argc > 1) {
        write("arguments:");
        for (uint64_t i = 1; i < argc; i++) { write(" ["); write(argv[i]); write("]"); }
        write("\nuser> ");
    }
    for (;;) {
        uint64_t length = 0;
        while (length < sizeof(line) - 1) {
            char c;
            int64_t result = sys_read(&c, 1);
            if (result < 0) {
                write("shell: stdin read failed\n");
                sys_exit(1);
            }
            if (result == 0) { sys_yield(); continue; }
            if (c == '\r' || c == '\n') break;
            if (c == '\b') { if (length) { length--; write("\b \b"); } continue; }
            line[length++] = c;
            sys_write(&c, 1);
        }
        line[length] = '\0';
        write("\n");
        int has_pipe = 0;
        for (uint64_t i = 0; i < length; i++) has_pipe |= line[i] == '|';
        if (has_pipe)
            run_pipeline(line, length);
        else if (length == 4 && line[0] == 'e' && line[1] == 'x' && line[2] == 'i' && line[3] == 't')
            sys_exit(0);
        if (length == 4 && line[0] == 'h' && line[1] == 'e' && line[2] == 'l' && line[3] == 'p') {
            write("help  clear  pwd  cd  ls  cat  stat  echo  write  mkdir  rm  rmdir  cp  mount  umount  sync  uptime  lspci  pid  exit\n");
            write("pipe example: echo test | cat\n");
        }
        else if (length == 5 && starts_with(line, length, "clear"))
            write("\033[2J\033[H");
        else if (length == 3 && starts_with(line, length, "pwd")) {
            char cwd[FS_PATH_MAX];
            if (sys_getcwd(cwd, sizeof(cwd)) < 0) write("pwd failed\n");
            else { write(cwd); write("\n"); }
        } else if (length == 2 && starts_with(line, length, "cd")) {
            if (sys_chdir("/") < 0) write("cd: directory not found\n");
        } else if (length > 3 && starts_with(line, length, "cd ")) {
            if (sys_chdir(&line[3]) < 0) write("cd: directory not found\n");
        }
        else if (length == 6 && line[0] == 'u' && line[1] == 'p' && line[2] == 't' && line[3] == 'i' && line[4] == 'm' && line[5] == 'e') {
            (void)run_program("/bin/uptime", NULL);
        } else if (length == 3 && line[0] == 'p' && line[1] == 'i' && line[2] == 'd') {
            write("PID: "); write_uint(sys_getpid()); write("\n");
        } else if (length == 2 && line[0] == 'l' && line[1] == 's') {
            (void)run_program("/bin/ls", NULL);
        } else if (length > 4 && starts_with(line, length, "cat ")) {
            (void)run_program("/bin/cat", &line[4]);
        } else if (length > 5 && starts_with(line, length, "echo ")) {
            (void)run_program("/bin/echo", &line[5]);
        } else if (length > 6 && starts_with(line, length, "write ")) {
            uint64_t split = 6;
            while (split < length && line[split] != ' ') split++;
            if (split == length) write("usage: write <file> <text>\n");
            else {
                line[split] = '\0';
                int64_t fd = sys_open(&line[6], 1);
                if (fd < 0 || sys_write_fd((int)fd, &line[split + 1], length - split - 1) < 0)
                    write("write failed\n");
                else write("written\n");
                if (fd >= 0) sys_close((int)fd);
            }
        } else if (length > 5 && starts_with(line, length, "exec ")) {
            exec_command(&line[5]);
        } else if (length > 6 && starts_with(line, length, "spawn ")) {
            uint64_t split = 6;
            while (split < length && line[split] != ' ') split++;
            const char *argument = NULL;
            if (split < length) { line[split] = '\0'; argument = &line[split + 1]; }
            int64_t pid = sys_spawn(&line[6], argument);
            if (pid < 0) write("spawn failed\n");
            else {
                int status = 0;
                write("started PID "); write_uint((uint64_t)pid); write("\n");
                if (sys_waitpid((uint64_t)pid, &status) < 0) write("waitpid failed\n");
                else { write("child exited with status "); write_uint((uint64_t)status); write("\n"); }
            }
        } else if (length) run_command(line);
        write("user> ");
    }
    for (;;)
        __asm__ volatile("pause");
}
