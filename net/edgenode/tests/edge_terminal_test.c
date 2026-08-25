#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edge_terminal.h"

bool edge_capability_has_terminal(void) {
    return true;
}

void edge_process_close_inherited_fds(int keep_fd) {
    (void)keep_fd;
}

static void require(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "edge terminal test failed: %s\n", message);
        exit(1);
    }
}

static iot_edge_v1_TerminalData request(const uint8_t terminal_id[16],
                                        uint64_t sequence, const char *data) {
    iot_edge_v1_TerminalData value = iot_edge_v1_TerminalData_init_zero;
    value.terminal_id.size = 16U;
    memcpy(value.terminal_id.bytes, terminal_id, 16U);
    value.sequence = sequence;
    value.data.size = strlen(data);
    memcpy(value.data.bytes, data, value.data.size);
    return value;
}

static size_t fill_pipe(int fd) {
    uint8_t filler[4096];
    memset(filler, 0x5a, sizeof(filler));
    size_t total = 0U;
    for (;;) {
        const ssize_t written = write(fd, filler, sizeof(filler));
        if (written > 0) {
            total += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        require(written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
                "pipe did not reach nonblocking backpressure");
        return total;
    }
}

static void drain_exact(int fd, size_t size) {
    uint8_t buffer[4096];
    size_t offset = 0U;
    while (offset < size) {
        const size_t wanted = size - offset < sizeof(buffer) ? size - offset
                                                             : sizeof(buffer);
        const ssize_t received = read(fd, buffer, wanted);
        if (received > 0) {
            offset += (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR)
            continue;
        require(false, "could not drain the filled pipe");
    }
}

static void read_exact(int fd, const char *expected) {
    uint8_t buffer[64];
    const size_t expected_size = strlen(expected);
    size_t offset = 0U;
    while (offset < expected_size) {
        const ssize_t received = read(fd, buffer + offset, expected_size - offset);
        if (received > 0) {
            offset += (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR)
            continue;
        require(false, "could not read acknowledged terminal input");
    }
    require(memcmp(buffer, expected, expected_size) == 0,
            "acknowledged terminal input changed bytes");
}

int main(void) {
    int pipes[2];
    require(pipe(pipes) == 0, "cannot create test pipe");
    const int flags = fcntl(pipes[1], F_GETFL, 0);
    require(flags >= 0 && fcntl(pipes[1], F_SETFL, flags | O_NONBLOCK) == 0,
            "cannot make test pipe nonblocking");

    const uint8_t terminal_id[16] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    };
    require(edge_terminal_test_attach(pipes[1], terminal_id),
            "cannot attach test terminal");

    const size_t filled = fill_pipe(pipes[1]);
    iot_edge_v1_TerminalData first = request(terminal_id, 1U, "first");
    uint64_t acked = 0U;
    require(edge_terminal_write(&first, &acked) == EDGE_TERMINAL_INPUT_PENDING &&
                acked == 0U,
            "backpressured input was acknowledged before reaching the PTY");

    drain_exact(pipes[0], filled);
    require(edge_terminal_flush(&acked) == EDGE_TERMINAL_INPUT_ACKED && acked == 1U,
            "flushed input did not acknowledge its sequence");
    read_exact(pipes[0], "first");

    acked = 0U;
    require(edge_terminal_write(&first, &acked) == EDGE_TERMINAL_INPUT_ERROR,
            "duplicate terminal input was accepted");
    iot_edge_v1_TerminalData skipped = request(terminal_id, 3U, "skipped");
    require(edge_terminal_write(&skipped, &acked) == EDGE_TERMINAL_INPUT_ERROR,
            "out-of-order terminal input was accepted");

    iot_edge_v1_TerminalData second = request(terminal_id, 2U, "second");
    require(edge_terminal_write(&second, &acked) == EDGE_TERMINAL_INPUT_ACKED &&
                acked == 2U,
            "next terminal input was not acknowledged");
    read_exact(pipes[0], "second");

    edge_terminal_close(terminal_id);
    close(pipes[0]);
    puts("edge terminal tests passed");
    return 0;
}
