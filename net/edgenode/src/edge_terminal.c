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
    if (!edge_capability_has_ttyd()) {
        set_error(error, error_size, "ttyd is not installed");
        return false;
    }
    if (request == NULL || request->terminal_id.size != 16U ||
        request->ticket.size < 16U || request->columns < 20U || request->columns > 500U ||
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

bool edge_terminal_write(const iot_edge_v1_TerminalData *request) {
    if (request == NULL || !same_terminal(&request->terminal_id) || request->data.size == 0U)
        return false;
    size_t offset = 0U;
    while (offset < request->data.size) {
        const ssize_t size = write(terminal.master, request->data.bytes + offset,
                                   request->data.size - offset);
        if (size > 0) {
            offset += (size_t)size;
            continue;
        }
        if (size < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
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
