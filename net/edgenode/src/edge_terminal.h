#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "edge.pb.h"

bool edge_terminal_open(const iot_edge_v1_TerminalOpen *request,
                        char *error, size_t error_size);

typedef enum {
    EDGE_TERMINAL_INPUT_ERROR = -1,
    EDGE_TERMINAL_INPUT_IDLE = 0,
    EDGE_TERMINAL_INPUT_PENDING = 1,
    EDGE_TERMINAL_INPUT_ACKED = 2,
} edge_terminal_input_result;

/*
 * Accepts exactly one sequenced input frame at a time. ACKED is returned only
 * after every byte reached the PTY; PENDING must be retried with flush.
 */
edge_terminal_input_result edge_terminal_write(
    const iot_edge_v1_TerminalData *request, uint64_t *acked_sequence);
edge_terminal_input_result edge_terminal_flush(uint64_t *acked_sequence);
bool edge_terminal_resize(const iot_edge_v1_TerminalResize *request);
void edge_terminal_close(const uint8_t terminal_id[16]);

/* Nonblocking. Returns output bytes; closed is set once the shell exits. */
ssize_t edge_terminal_read(uint8_t terminal_id[16], uint8_t *output, size_t capacity,
                           bool *closed, int32_t *exit_code);

#ifdef EDGENODE_TERMINAL_TEST
/* Takes ownership of master. */
bool edge_terminal_test_attach(int master, const uint8_t terminal_id[16]);
#endif
