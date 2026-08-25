#include "edge_terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edge_capability.h"
#include "edge_process.h"

typedef struct {
    int master;
    pid_t child;
    uint8_t id[16];
    uint8_t pending_input[4096];
    size_t pending_input_size;
    size_t pending_input_offset;
    uint64_t pending_input_sequence;
    uint64_t last_input_sequence;
} terminal_state;

static terminal_state terminal = {.master = -1, .child = -1};

static void terminate_and_reap(pid_t child) {
    if (child <= 0)
        return;
    (void)kill(child, SIGHUP);
    for (unsigned attempt = 0U; attempt < 20U; ++attempt) {
        pid_t waited;
        do {
            waited = waitpid(child, NULL, WNOHANG);
        } while (waited < 0 && errno == EINTR);
        if (waited == child || (waited < 0 && errno == ECHILD))
            return;
        usleep(5000U);
    }
    (void)kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
}

static void set_error(char *output, size_t capacity, const char *message) {
    if (output != NULL && capacity != 0U)
        snprintf(output, capacity, "%s", message);
}

static bool same_terminal(const void *field) {
    pb_size_t size = 0U;
    memcpy(&size, field, sizeof(size));
    return terminal.master >= 0 && size == 16U &&
           memcmp((const uint8_t *)field + sizeof(size), terminal.id, 16U) == 0;
}

bool edge_terminal_open(const iot_edge_v1_TerminalOpen *request,
                        char *error, size_t error_size) {
    if (!edge_capability_has_terminal()) {
        set_error(error, error_size, "terminal PTY is unavailable");
        return false;
    }
    if (request == NULL || request->terminal_id.size != 16U ||
        request->columns < 20U || request->columns > 500U ||
        request->rows < 5U || request->rows > 200U) {
        set_error(error, error_size, "terminal request is invalid");
        return false;
    }
    if (terminal.master >= 0) {
        set_error(error, error_size, "another terminal is active");
        return false;
    }
    struct winsize size = {
        .ws_row = (unsigned short)request->rows,
        .ws_col = (unsigned short)request->columns,
    };
    int master = -1;
    const pid_t child = forkpty(&master, NULL, NULL, &size);
    if (child < 0) {
        set_error(error, error_size, "cannot open terminal pty");
        return false;
    }
    if (child == 0) {
        edge_process_close_inherited_fds(-1);
        setenv("TERM", "xterm-256color", 1);
        execl("/bin/ash", "ash", "-l", (char *)NULL);
        _exit(127);
    }
    const int flags = fcntl(master, F_GETFL, 0);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(master);
        terminate_and_reap(child);
        set_error(error, error_size, "cannot configure terminal pty");
        return false;
    }
    terminal.master = master;
    terminal.child = child;
    memcpy(terminal.id, request->terminal_id.bytes, sizeof(terminal.id));
    return true;
}

edge_terminal_input_result edge_terminal_flush(uint64_t *acked_sequence) {
    if (acked_sequence != NULL)
        *acked_sequence = 0U;
    if (terminal.master < 0)
        return EDGE_TERMINAL_INPUT_ERROR;
    if (terminal.pending_input_size == 0U)
        return EDGE_TERMINAL_INPUT_IDLE;
    while (terminal.pending_input_offset < terminal.pending_input_size) {
        const ssize_t size = write(
            terminal.master, terminal.pending_input + terminal.pending_input_offset,
            terminal.pending_input_size - terminal.pending_input_offset);
        if (size > 0) {
            terminal.pending_input_offset += (size_t)size;
            continue;
        }
        if (size < 0 && errno == EINTR)
            continue;
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return EDGE_TERMINAL_INPUT_PENDING;
        return EDGE_TERMINAL_INPUT_ERROR;
    }
    terminal.last_input_sequence = terminal.pending_input_sequence;
    terminal.pending_input_size = 0U;
    terminal.pending_input_offset = 0U;
    terminal.pending_input_sequence = 0U;
    if (acked_sequence != NULL)
        *acked_sequence = terminal.last_input_sequence;
    return EDGE_TERMINAL_INPUT_ACKED;
}

edge_terminal_input_result edge_terminal_write(
    const iot_edge_v1_TerminalData *request, uint64_t *acked_sequence) {
    if (acked_sequence != NULL)
        *acked_sequence = 0U;
    if (request == NULL || !same_terminal(&request->terminal_id) ||
        request->data.size == 0U || request->data.size > sizeof(terminal.pending_input) ||
        request->sequence == 0U)
        return EDGE_TERMINAL_INPUT_ERROR;
    if (terminal.pending_input_size != 0U)
        return EDGE_TERMINAL_INPUT_ERROR;
    if (terminal.last_input_sequence == UINT64_MAX ||
        request->sequence != terminal.last_input_sequence + 1U)
        return EDGE_TERMINAL_INPUT_ERROR;
    memcpy(terminal.pending_input, request->data.bytes, request->data.size);
    terminal.pending_input_size = request->data.size;
    terminal.pending_input_offset = 0U;
    terminal.pending_input_sequence = request->sequence;
    return edge_terminal_flush(acked_sequence);
}

bool edge_terminal_resize(const iot_edge_v1_TerminalResize *request) {
    if (request == NULL || !same_terminal(&request->terminal_id) ||
        request->columns < 20U || request->columns > 500U ||
        request->rows < 5U || request->rows > 200U)
        return false;
    const struct winsize size = {
        .ws_row = (unsigned short)request->rows,
        .ws_col = (unsigned short)request->columns,
    };
    return ioctl(terminal.master, TIOCSWINSZ, &size) == 0;
}

void edge_terminal_close(const uint8_t terminal_id[16]) {
    if (terminal.master < 0 || terminal_id == NULL ||
        memcmp(terminal.id, terminal_id, 16U) != 0)
        return;
    close(terminal.master);
    terminate_and_reap(terminal.child);
    memset(&terminal, 0, sizeof(terminal));
    terminal.master = -1;
    terminal.child = -1;
}

#ifdef EDGENODE_TERMINAL_TEST
bool edge_terminal_test_attach(int master, const uint8_t terminal_id[16]) {
    if (master < 0 || terminal_id == NULL || terminal.master >= 0)
        return false;
    memset(&terminal, 0, sizeof(terminal));
    terminal.master = master;
    terminal.child = -1;
    memcpy(terminal.id, terminal_id, sizeof(terminal.id));
    return true;
}
#endif

ssize_t edge_terminal_read(uint8_t terminal_id[16], uint8_t *output, size_t capacity,
                           bool *closed, int32_t *exit_code) {
    if (closed != NULL)
        *closed = false;
    if (terminal.master < 0 || output == NULL || capacity == 0U)
        return 0;
    memcpy(terminal_id, terminal.id, 16U);
    const ssize_t size = read(terminal.master, output, capacity);
    if (size > 0)
        return size;
    int status = 0;
    const pid_t result = waitpid(terminal.child, &status, WNOHANG);
    if (result == terminal.child || (size == 0) || (size < 0 && errno != EAGAIN && errno != EINTR)) {
        if (closed != NULL)
            *closed = true;
        if (exit_code != NULL)
            *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        edge_terminal_close(terminal.id);
    }
    return 0;
}
