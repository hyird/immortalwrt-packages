#include "edge_process.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_milliseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0U;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

int edge_process_run_timeout(const char *const argv[], int stdin_fd, int stdout_fd,
                             unsigned timeout_ms) {
    if (argv == NULL || argv[0] == NULL)
        return -1;
    const pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        (void)setpgid(0, 0);
        if (stdin_fd >= 0 && dup2(stdin_fd, STDIN_FILENO) < 0)
            _exit(126);
        if (stdout_fd >= 0 && dup2(stdout_fd, STDOUT_FILENO) < 0)
            _exit(126);
        edge_process_close_inherited_fds(-1);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    (void)setpgid(child, child);
    int status = 0;
    const uint64_t deadline = monotonic_milliseconds() + timeout_ms;
    for (;;) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (waited < 0 && errno != EINTR)
            return -1;
        if (timeout_ms != 0U && monotonic_milliseconds() >= deadline)
            break;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
        }
    }
    (void)kill(-child, SIGTERM);
    (void)kill(child, SIGTERM);
    const uint64_t kill_deadline = monotonic_milliseconds() + 1000U;
    while (monotonic_milliseconds() < kill_deadline) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child || (waited < 0 && errno == ECHILD))
            return -1;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
        }
    }
    (void)kill(-child, SIGKILL);
    (void)kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return -1;
}

int edge_process_run(const char *const argv[], int stdin_fd, int stdout_fd) {
    return edge_process_run_timeout(argv, stdin_fd, stdout_fd, 30000U);
}

int edge_process_detach(void) {
    const pid_t intermediate = fork();
    if (intermediate < 0)
        return -1;
    if (intermediate == 0) {
        const pid_t worker = fork();
        if (worker < 0)
            _exit(127);
        if (worker > 0)
            _exit(0);
        if (setsid() < 0)
            _exit(127);
        return 0;
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(intermediate, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == intermediate && WIFEXITED(status) && WEXITSTATUS(status) == 0
               ? 1
               : -1;
}

void edge_process_close_inherited_fds(int keep_fd) {
    DIR *directory = opendir("/proc/self/fd");
    if (directory != NULL) {
        const int directory_fd = dirfd(directory);
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            char *end = NULL;
            errno = 0;
            const long value = strtol(entry->d_name, &end, 10);
            if (errno != 0 || end == entry->d_name || *end != '\0' || value < 3L ||
                value == keep_fd || value == directory_fd)
                continue;
            close((int)value);
        }
        closedir(directory);
        return;
    }
    long maximum = sysconf(_SC_OPEN_MAX);
    if (maximum < 0L || maximum > 65536L)
        maximum = 65536L;
    for (int fd = 3; fd < (int)maximum; ++fd)
        if (fd != keep_fd)
            close(fd);
}
