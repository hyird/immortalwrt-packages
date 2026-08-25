#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "edge_process.h"

static uint64_t monotonic_ms(void) {
    struct timespec value;
    assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

int main(void) {
    const char *exit_seven[] = {"sh", "-c", "exit 7", NULL};
    assert(edge_process_run(exit_seven, -1, -1) == 7);

    const char *terminated[] = {"sh", "-c", "kill -TERM $$", NULL};
    assert(edge_process_run(terminated, -1, -1) == -1);

    int descriptors[2];
    assert(pipe(descriptors) == 0);
    assert(dup2(descriptors[0], 9) == 9);
    close(descriptors[0]);
    close(descriptors[1]);
    const char *closed_fds[] = {"sh", "-c", "test ! -e /proc/self/fd/9", NULL};
    assert(edge_process_run(closed_fds, -1, -1) == 0);
    close(9);

    const char *sleeper[] = {"sh", "-c", "sleep 10", NULL};
    const uint64_t started = monotonic_ms();
    assert(edge_process_run_timeout(sleeper, -1, -1, 100U) == -1);
    const uint64_t elapsed = monotonic_ms() - started;
    assert(elapsed >= 100U && elapsed < 2000U);
    return 0;
}
