#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "edge.pb.h"

bool edge_terminal_open(const iot_edge_v1_TerminalOpen *request,
                        char *error, size_t error_size);
bool edge_terminal_write(const iot_edge_v1_TerminalData *request);
bool edge_terminal_resize(const iot_edge_v1_TerminalResize *request);
void edge_terminal_close(const uint8_t terminal_id[16]);

/* Nonblocking. Returns output bytes; closed is set once the shell exits. */
ssize_t edge_terminal_read(uint8_t terminal_id[16], uint8_t *output, size_t capacity,
                           bool *closed, int32_t *exit_code);
